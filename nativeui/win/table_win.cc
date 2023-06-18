// Copyright 2019 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/win/table_win.h"

#include <commctrl.h>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "nativeui/gfx/attributed_text.h"
#include "nativeui/table_model.h"
#include "nativeui/win/util/hwnd_util.h"

namespace nu {

TableImpl::TableImpl(Table* delegate)
    : SubwinView(delegate, WC_LISTVIEW,
                 LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_REPORT |
                 LVS_OWNERDATA | LVS_EDITLABELS | WS_CHILD | WS_VISIBLE) {
  set_focusable(true);
  ListView_SetExtendedListViewStyle(
      hwnd(),
      LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
}

TableImpl::~TableImpl() {
  if (image_list_)
    ImageList_Destroy(image_list_);
}

void TableImpl::AddColumnWithOptions(const std::wstring& title,
                                     Table::ColumnOptions options) {
  LVCOLUMNA col = {0};
  col.mask = LVCF_TEXT;
  // The pszText is LPSTR even under Unicode build.
  col.pszText = const_cast<char*>(reinterpret_cast<const char*>(title.c_str()));

  // Insert.
  if (options.column == -1)
    options.column = GetColumnCount();
  ListView_InsertColumn(hwnd(), GetColumnCount(), &col);
  columns_.emplace_back(std::move(options));
  UpdateColumnsWidth(static_cast<Table*>(delegate())->GetModel());

  // Get checkbox information.
  if (options.type == Table::ColumnType::Checkbox && !checkbox_icons_) {
    checkbox_icons_ = ListView_GetImageList(hwnd(), LVSIL_STATE);
    int width, height;
    if (ImageList_GetIconSize(checkbox_icons_, &width, &height))
      checkbox_size_ = Size(width, height);
  }

  // Optimization in the custom draw handler.
  if (options.type == Table::ColumnType::Checkbox ||
      options.type == Table::ColumnType::Custom)
    has_custom_column_ = true;
}

int TableImpl::GetColumnCount() const {
  return static_cast<int>(columns_.size());
}

void TableImpl::UpdateColumnsWidth(TableModel* model) {
  int count = GetColumnCount();
  if (count == 0)
    return;

  // The AUTOSIZE style does not work for virtual list, we have to guess a
  // best width for each column.
  for (int i = 0; i < count - 1; ++i) {
    int width = columns_[i].width * scale_factor();
    if (width < 0) {  // autosize
      width = kDefaultColumnWidth * scale_factor();
      // If there is data in model, use the first cell's width.
      if (model && model->GetRowCount() > 0) {
        base::Value value = model->GetValue(i, 0);
        if (value.is_string()) {
          std::wstring text = base::UTF8ToWide(value.GetString());
          int text_width = ListView_GetStringWidth(hwnd(), text.c_str());
          // Add some padding.
          text_width += (i == 0 ? 7 : 14) * scale_factor();
          // Do not choose a too small width.
          width = std::max(width, text_width);
        }
      }
    }
    ListView_SetColumnWidth(hwnd(), i, width);
  }

  // Make the last column use USEHEADER style, which fills it to rest of the
  // list control.
  int width = columns_[count - 1].width * scale_factor();
  ListView_SetColumnWidth(hwnd(), count - 1,
                          width < 0 ? LVSCW_AUTOSIZE_USEHEADER : width);
}

void TableImpl::SetRowHeight(int height) {
  // ListView does not have a way to change row height, so we have to work out
  // our own way.
  if (!image_list_)
    image_list_ = ImageList_Create(1, height, 0, 0, 0);
  else
    ImageList_SetIconSize(image_list_, 1, height);
  ListView_SetImageList(hwnd(), image_list_, LVSIL_SMALL);
}

int TableImpl::GetRowHeight() const {
  if (image_list_) {
    int cx, cy;
    if (ImageList_GetIconSize(image_list_, &cx, &cy))
      return cy;
  }
  // Default row height should be able to draw full text.
  scoped_refptr<AttributedText> text =
      new AttributedText(L"bp", TextAttributes(font()));
  return std::ceil(text->GetOneLineHeight());
}

LRESULT TableImpl::OnEraseBkgnd(HDC dc) {
  // Reduce flicker on cell update.
  return 1;
}

void TableImpl::OnPaint(HDC dc) {
  // Block redrawing of leftmost item when editing sub item.
  if (edit_proc_) {
    RECT rc;
    ListView_GetItemRect(hwnd(), edit_row_, &rc, LVIR_LABEL);
    ::ValidateRect(hwnd(), &rc);
  }
  SetMsgHandled(false);
}

void TableImpl::OnWindowPosChanged(WINDOWPOS* pos) {
  SetMsgHandled(false);
  if (!window())
    return;

  // Resize the last column to fill the control.
  int count = GetColumnCount();
  if (count > 0 && columns_[count - 1].width == -1)
    ListView_SetColumnWidth(hwnd(), count - 1, LVSCW_AUTOSIZE_USEHEADER);
}

LRESULT TableImpl::OnNotify(int code, LPNMHDR pnmh) {
  switch (pnmh->code) {
    case LVN_GETDISPINFO: {
      auto* nm = reinterpret_cast<NMLVDISPINFO*>(pnmh);
      return OnGetDispInfo(nm, nm->item.iSubItem, nm->item.iItem);
    }
    case NM_CUSTOMDRAW: {
      auto* nm = reinterpret_cast<NMLVCUSTOMDRAW*>(pnmh);
      return OnCustomDraw(nm, nm->nmcd.dwItemSpec);
    }
    case LVN_BEGINLABELEDIT: {
      auto* nm = reinterpret_cast<NMLVDISPINFO*>(pnmh);
      return OnBeginEdit(nm, nm->item.iItem);
    }
    case LVN_ENDLABELEDIT: {
      auto* nm = reinterpret_cast<NMLVDISPINFO*>(pnmh);
      return OnEndEdit(nm, nm->item.iItem);
    }
    case NM_CLICK: {
      auto* nm = reinterpret_cast<NMITEMACTIVATE*>(pnmh);
      return OnItemClick(Point(nm->ptAction), nm->iSubItem, nm->iItem);
    }
    case LVN_ITEMACTIVATE: {
      static_assert(_WIN32_IE >= 0x0400);
      auto* nm = reinterpret_cast<NMITEMACTIVATE*>(pnmh);
      auto* table = static_cast<Table*>(delegate());
      table->on_row_activate.Emit(table, nm->iItem);
    }
    case LVN_ITEMCHANGED: {
      auto* nm = reinterpret_cast<NMLISTVIEW*>(pnmh);
      if ((nm->uChanged & LVIF_STATE) && (nm->uOldState ^ nm->uNewState)) {
        auto* table = static_cast<Table*>(delegate());
        table->on_selection_change.Emit(table);
      }
    }
    default:
      return 0;
  }
}

LRESULT TableImpl::OnGetDispInfo(NMLVDISPINFO* nm, int column, int row) {
  // When editing the sub item, hide the text.
  if (edit_proc_ && column == edit_column_ && row == edit_row_) {
    nm->item.pszText = const_cast<wchar_t*>(static_cast<const wchar_t*>(L""));
    return TRUE;
  }

  auto* model = static_cast<Table*>(delegate())->GetModel();
  if (!model)
    return 0;
  base::Value value = model->GetValue(column, row);
  // Always set text regardless of cell type, for increased accessbility.
  if ((nm->item.mask & LVIF_TEXT) && value.is_string()) {
    text_cache_ = base::UTF8ToWide(value.GetString());
    nm->item.pszText = const_cast<wchar_t*>(text_cache_.c_str());
    return TRUE;
  }
  return 0;
}

LRESULT TableImpl::OnCustomDraw(NMLVCUSTOMDRAW* nm, int row) {
  if (!has_custom_column_)
    return 0;
  auto* model = static_cast<Table*>(delegate())->GetModel();
  if (!model)
    return 0;

  // Handle the post paint stage.
  switch (nm->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
      return CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPOSTPAINT:
      break;
    default:
      return CDRF_DODEFAULT;
  }

  // Draw custom type cells.
  for (int i = 0; i < GetColumnCount(); ++i) {
    const auto& options = columns_[i];
    // Draw cells.
    if (options.type == Table::ColumnType::Checkbox)
      DrawCheckboxCell(nm->nmcd.hdc, i, row, model->GetValue(i, row));
    else if (options.type == Table::ColumnType::Custom)
      InvokeOnDraw(options, nm->nmcd.hdc, i, row, model->GetValue(i, row));
  }
  return CDRF_SKIPDEFAULT;
}

LRESULT TableImpl::OnBeginEdit(NMLVDISPINFO* nm, int row) {
  // Find out the column.
  LVHITTESTINFO hit = {{0}};
  ::GetCursorPos(&hit.pt);
  ::ScreenToClient(hwnd(), &hit.pt);
  ListView_SubItemHitTest(hwnd(), &hit);
  if (hit.iSubItem < 0 || hit.iSubItem >= GetColumnCount() || row != hit.iItem)
    return TRUE;
  int column = hit.iSubItem;

  // Only allow editing Edit type.
  if (columns_[column].type != Table::ColumnType::Edit)
    return TRUE;

  edit_row_ = row;
  edit_column_ = column;

  // Make the edit window work when editing sub items.
  if (column > 0) {
    // Subclass the edit window.
    edit_hwnd_ = ListView_GetEditControl(hwnd());
    SetWindowUserData(edit_hwnd_, this);
    edit_proc_ = SetWindowProc(edit_hwnd_, &EditWndProc);
    // Set pos to cell.
    RECT rc;
    ListView_GetSubItemRect(hwnd(), row, column, LVIR_LABEL, &rc);
    edit_pos_ = Rect(rc).origin();
    // Update text in edit window.
    auto* model = static_cast<Table*>(delegate())->GetModel();
    if (model) {
      base::Value value = model->GetValue(column, row);
      if (value.is_string()) {
        std::wstring text16 = base::UTF8ToWide(value.GetString());
        ::SetWindowTextW(edit_hwnd_, text16.c_str());
      }
    }
  }
  return FALSE;
}

LRESULT TableImpl::OnEndEdit(NMLVDISPINFO* nm, int row) {
  // Notify the result.
  DCHECK_EQ(row, edit_row_);
  if (nm->item.pszText != nullptr) {
    auto* model = static_cast<Table*>(delegate())->GetModel();
    if (model)
      model->SetValue(edit_column_, edit_row_,
                      base::Value(base::WideToUTF8(nm->item.pszText)));
  }

  edit_row_ = -1;
  edit_column_ = -1;

  if (edit_proc_) {
    // Revert edit window subclass.
    SetWindowProc(edit_hwnd_, edit_proc_);
    edit_proc_ = nullptr;
    // Do nothing as ListView thinks we are editing the first column.
    return FALSE;
  }
  return TRUE;
}

LRESULT TableImpl::OnItemClick(Point point, int column, int row) {
  if (columns_[column].type != Table::ColumnType::Checkbox)
    return FALSE;
  if (!GetCheckboxBounds(column, row).Contains(point))
    return FALSE;
  auto* table = static_cast<Table*>(delegate());
  auto* model = table->GetModel();
  if (!model)
    return FALSE;
  model->SetValue(column, row,
                  base::Value(!model->GetValue(column, row).GetBool()));
  table->on_toggle_checkbox.Emit(table, column, row);
  return TRUE;
}

Rect TableImpl::GetCellBounds(int column, int row) {
  RECT rc = {0};
  ListView_GetSubItemRect(hwnd(), row, column, LVIR_BOUNDS, &rc);
  return Rect(rc);
}

Rect TableImpl::GetCheckboxBounds(int column, int row) {
  Rect rect = GetCellBounds(column, row);
  return Rect(rect.x() + (rect.width() - checkbox_size_.width()) / 2,
              rect.y() + (rect.height() - checkbox_size_.height()) / 2,
              checkbox_size_.width(),
              checkbox_size_.height());
}

void TableImpl::DrawCheckboxCell(HDC hdc, int column, int row,
                                 const base::Value& value) {
  if (!value.is_bool())
    return;
  Rect checkbox = GetCheckboxBounds(column, row);
  ImageList_Draw(checkbox_icons_, value.GetBool(), hdc, checkbox.x(),
                 checkbox.y(), ILD_TRANSPARENT);
}

void TableImpl::InvokeOnDraw(const Table::ColumnOptions& options, HDC hdc,
                             int column, int row, const base::Value& value) {
  // Reduce the cell area so the focus ring can show.
  Rect rect = GetCellBounds(column, row);
  int space = 1 * scale_factor();
  rect.Inset(space, space);
  // Draw.
  PainterWin painter(hdc, rect.size(), scale_factor());
  painter.TranslatePixel(rect.OffsetFromOrigin());
  painter.ClipRectPixel(Rect(rect.size()));
  options.on_draw(&painter,
                  RectF(ScaleSize(SizeF(rect.size()), 1.f / scale_factor())),
                  value);
}

// static
LRESULT TableImpl::EditWndProc(HWND hwnd,
                               UINT message,
                               WPARAM w_param,
                               LPARAM l_param) {
  auto* self = reinterpret_cast<TableImpl*>(GetWindowUserData(hwnd));
  // Force moving the edit window.
  if (message == WM_WINDOWPOSCHANGING) {
    auto* pos = reinterpret_cast<LPWINDOWPOS>(l_param);
    pos->x = self->edit_pos_.x();
    pos->y = self->edit_pos_.y();
  }
  return CallWindowProc(self->edit_proc_, hwnd, message, w_param, l_param);
}

///////////////////////////////////////////////////////////////////////////////
// Public Table API implementation.

NativeView Table::PlatformCreate() {
  return new TableImpl(this);
}

void Table::PlatformDestroy() {
}

void Table::PlatformSetModel(TableModel* model) {
  auto* table = static_cast<TableImpl*>(GetNative());
  if (GetModel()) {
    // Deselect everything.
    ListView_SetItemState(table->hwnd(), -1, LVIF_STATE, LVIS_SELECTED);
    // Scroll back to top, otherwise listview will have rendering bugs.
    ListView_EnsureVisible(table->hwnd(), 0, FALSE);
  }
  // Update row count.
  ListView_SetItemCountEx(table->hwnd(), model ? model->GetRowCount() : 0, 0);
  if (model) {
    // Listview does not update column width automatically, recalculate width
    // after changing model.
    table->UpdateColumnsWidth(model);
    // After updating column width, we have to set SetItemCount again to force
    // listview to update scrollbar.
    ListView_SetItemCountEx(table->hwnd(), model->GetRowCount(), 0);
  }
}

void Table::AddColumnWithOptions(const std::string& title,
                                 const ColumnOptions& options) {
  auto* table = static_cast<TableImpl*>(GetNative());
  table->AddColumnWithOptions(base::UTF8ToWide(title), options);
}

int Table::GetColumnCount() const {
  auto* table = static_cast<TableImpl*>(GetNative());
  return table->GetColumnCount();
}

void Table::SetColumnsVisible(bool visible) {
  auto* table = static_cast<TableImpl*>(GetNative());
  LONG styles = ::GetWindowLong(table->hwnd(), GWL_STYLE);
  if (!visible)
    styles |= LVS_NOCOLUMNHEADER;
  else
    styles &= ~LVS_NOCOLUMNHEADER;
  ::SetWindowLong(table->hwnd(), GWL_STYLE, styles);
}

bool Table::IsColumnsVisible() const {
  auto* table = static_cast<TableImpl*>(GetNative());
  return !(::GetWindowLong(table->hwnd(), GWL_STYLE) & LVS_NOCOLUMNHEADER);
}

void Table::SetRowHeight(float height) {
  auto* table = static_cast<TableImpl*>(GetNative());
  table->SetRowHeight(std::ceil(height * table->scale_factor()));
  // Update scrollbar after changing row height.
  if (GetModel())
    ListView_SetItemCountEx(table->hwnd(), GetModel()->GetRowCount(), 0);
}

float Table::GetRowHeight() const {
  auto* table = static_cast<TableImpl*>(GetNative());
  return table->GetRowHeight();
}

void Table::SetHasBorder(bool yes) {
  auto* table = static_cast<TableImpl*>(GetNative());
  return table->SetWindowStyle(WS_BORDER, yes);
}

bool Table::HasBorder() const {
  auto* table = static_cast<TableImpl*>(GetNative());
  return table->HasWindowStyle(WS_BORDER);
}

void Table::EnableMultipleSelection(bool enable) {
  auto* table = static_cast<TableImpl*>(GetNative());
  table->SetWindowStyle(LVS_SINGLESEL, !enable);
}

bool Table::IsMultipleSelectionEnabled() const {
  auto* table = static_cast<TableImpl*>(GetNative());
  return !table->HasWindowStyle(LVS_SINGLESEL);
}

void Table::SelectRow(int row) {
  SelectRows({row});
}

int Table::GetSelectedRow() const {
  auto* table = static_cast<TableImpl*>(GetNative());
  return ListView_GetNextItem(table->hwnd(), -1, LVNI_SELECTED);
}

void Table::SelectRows(std::set<int> rows) {
  auto* table = static_cast<TableImpl*>(GetNative());
  ListView_SetItemState(table->hwnd(), -1, 0, LVIS_SELECTED);
  for (int row : rows)
    ListView_SetItemState(table->hwnd(), row, LVIS_SELECTED, LVIS_SELECTED);
}

std::set<int> Table::GetSelectedRows() const {
  auto* table = static_cast<TableImpl*>(GetNative());
  std::set<int> rows;
  int i = ListView_GetNextItem(table->hwnd(), -1, LVNI_SELECTED);
  while (i >= 0) {
    rows.insert(i);
    i = ListView_GetNextItem(table->hwnd(), i, LVNI_SELECTED);
  }
  return rows;
}

void Table::NotifyRowInsertion(uint32_t row) {
  auto* table = static_cast<TableImpl*>(GetNative());
  ListView_SetItemCountEx(table->hwnd(), GetModel()->GetRowCount(),
                          LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
}

void Table::NotifyRowDeletion(uint32_t row) {
  auto* table = static_cast<TableImpl*>(GetNative());
  ListView_SetItemCountEx(table->hwnd(), GetModel()->GetRowCount(),
                          LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
}

void Table::NotifyValueChange(uint32_t column, uint32_t row) {
  auto* table = static_cast<TableImpl*>(GetNative());
  ListView_Update(table->hwnd(), row);
}

}  // namespace nu
