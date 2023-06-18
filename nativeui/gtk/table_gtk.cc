// Copyright 2019 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/table.h"

#include "base/logging.h"
#include "base/notreached.h"
#include "base/values.h"
#include "nativeui/gtk/table/nu_custom_cell_renderer.h"
#include "nativeui/gtk/table/nu_tree_model.h"
#include "nativeui/gtk/util/widget_util.h"
#include "nativeui/table_model.h"

namespace nu {

namespace {

// Calculate the default row height of cell.
int GetDefaultRowHeight() {
  // Cache calls.
  static int preferred = -1;
  if (preferred > -1)
    return preferred;

  // Calculate the height of a normal text row.
  GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
  GtkWidget* widget = gtk_label_new("some text");
  gtk_cell_renderer_get_preferred_height(renderer, widget, nullptr, &preferred);

  // Correctly free floaing references.
  g_object_ref_sink(renderer);
  g_object_ref_sink(widget);
  gtk_widget_destroy(widget);
  g_object_unref(widget);
  g_object_unref(renderer);
  return preferred;
}

// Get the row index from path.
gint RowFromTreePath(const gchar* path) {
  auto* tree_path = gtk_tree_path_new_from_string(path);
  if (!tree_path)
    return -1;
  gint row = gtk_tree_path_get_indices(tree_path)[0];
  gtk_tree_path_free(tree_path);
  return row;
}

// Called when user double-clicks a row.
void OnTableRowActivated(GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*,
                         Table* table) {
  table->on_row_activate.Emit(table, table->GetSelectedRow());
}

// Called when selection of row has changed.
void OnTableSelectionChanged(GtkTreeSelection*, Table* table) {
  table->on_selection_change.Emit(table);
}

// Called when user has done editing a cell.
void OnCellEdited(GtkCellRendererText* cell,
                  const gchar* path,
                  const gchar* new_text,
                  Table* table) {
  gint column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "column"));
  table->GetModel()->SetValue(column, RowFromTreePath(path),
                              base::Value(new_text));
}

// Called when user clicks the checkbox in a cell.
void OnCellToggled(GtkCellRendererToggle* cell,
                   const gchar* path,
                   Table* table) {
  gint column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "column"));
  gint row = RowFromTreePath(path);
  table->GetModel()->SetValue(
      column, row,
      base::Value(!table->GetModel()->GetValue(column, row).GetBool()));
  table->on_toggle_checkbox.Emit(table, column, row);
}

// Called to provide data to cell renderer.
void TreeCellData(GtkTreeViewColumn* tree_column,
                  GtkCellRenderer* renderer,
                  GtkTreeModel* tree_model,
                  GtkTreeIter* iter,
                  void* user_data) {
  auto* options = static_cast<Table::ColumnOptions*>(user_data);

  // Read value from model.
  GValue gval = G_VALUE_INIT;
  gtk_tree_model_get_value(tree_model, iter, options->column, &gval);
  auto* value = static_cast<base::Value*>(g_value_get_boxed(&gval));

  // Pass value.
  switch (options->type) {
    case Table::ColumnType::Text:
    case Table::ColumnType::Edit:
      if (value && value->is_string())
        g_object_set(renderer, "text", value->GetString().c_str(), nullptr);
      break;

    case Table::ColumnType::Checkbox:
      if (value && value->is_bool())
        g_object_set(renderer, "active", value->GetBool(), nullptr);
      break;

    case Table::ColumnType::Custom:
      g_object_set(renderer, "value", value, nullptr);
      break;
  }
  g_value_unset(&gval);
}

}  // namespace

NativeView Table::PlatformCreate() {
  GtkWidget* tree_view = gtk_tree_view_new();
  gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(tree_view), true);
  g_signal_connect(tree_view, "row-activated", G_CALLBACK(OnTableRowActivated),
                   this);
  gtk_widget_show(tree_view);

  GtkTreeSelection* selection =
      gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
  g_signal_connect(selection, "changed", G_CALLBACK(OnTableSelectionChanged),
                   this);

  GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
  g_object_set_data(G_OBJECT(scroll), "widget", tree_view);
  g_object_set_data(G_OBJECT(scroll), "row-height",
                    GINT_TO_POINTER(GetDefaultRowHeight()));
  gtk_container_add(GTK_CONTAINER(scroll), tree_view);
  return scroll;
}

void Table::PlatformDestroy() {
  // The widget relies on Table to get items, so we must ensure the
  // widget is destroyed before this class.
  View::PlatformDestroy();
}

void Table::PlatformSetModel(TableModel* model) {
  auto* tree_view = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()),
                                                    "widget"));
  NUTreeModel* tree_model = nu_tree_model_new(this, model);
  gtk_tree_view_set_model(tree_view, GTK_TREE_MODEL(tree_model));
}

void Table::AddColumnWithOptions(const std::string& title,
                                 const ColumnOptions& options) {
  auto* tree_view = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()),
                                                    "widget"));
  // Create renderer.
  GtkCellRenderer* renderer = nullptr;
  switch (options.type) {
    case Table::ColumnType::Text:
    case Table::ColumnType::Edit:
      renderer = gtk_cell_renderer_text_new();
      if (options.type == Table::ColumnType::Edit) {
        g_object_set(renderer, "editable", true, nullptr);
        g_signal_connect(renderer, "edited", G_CALLBACK(OnCellEdited), this);
      }
      break;
    case Table::ColumnType::Checkbox:
      renderer = gtk_cell_renderer_toggle_new();
      g_signal_connect(renderer, "toggled", G_CALLBACK(OnCellToggled), this);
      break;
    case Table::ColumnType::Custom:
      renderer = nu_custom_cell_renderer_new(options);
      break;
  }
  // Store the column index for later use.
  int column = options.column == -1 ? GetColumnCount() : options.column;
  g_object_set_data(G_OBJECT(renderer), "column", GINT_TO_POINTER(column));
  // Set row height.
  g_object_set(G_OBJECT(renderer), "height",
               static_cast<int>(GetRowHeight()), nullptr);

  // Create column.
  auto* tree_column = gtk_tree_view_column_new_with_attributes(
      title.c_str(), renderer, nullptr);
  gtk_tree_view_column_set_sizing(tree_column, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_resizable(tree_column, true);
  if (options.width != -1)
    gtk_tree_view_column_set_fixed_width(tree_column, options.width);
  gtk_tree_view_append_column(tree_view, tree_column);

  // Pass the ColumnOptions to renderer.
  auto* data = new ColumnOptions(options);
  data->column = column;
  gtk_tree_view_column_set_cell_data_func(
      tree_column, renderer, &TreeCellData, data, &Delete<ColumnOptions>);
}

int Table::GetColumnCount() const {
  auto* tree_view = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()),
                                                    "widget"));
  return gtk_tree_view_get_n_columns(tree_view);
}

void Table::SetColumnsVisible(bool visible) {
  auto* tree_view = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()),
                                                    "widget"));
  gtk_tree_view_set_headers_visible(tree_view, visible);
}

bool Table::IsColumnsVisible() const {
  auto* tree_view = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()),
                                                    "widget"));
  return gtk_tree_view_get_headers_visible(tree_view);
}

void Table::SetRowHeight(float height) {
  if (GetColumnCount() > 0) {
    LOG(ERROR) << "Setting row height only works before adding any column";
    return;
  }
  g_object_set_data(G_OBJECT(GetNative()),
                    "row-height", GINT_TO_POINTER(static_cast<int>(height)));
}

float Table::GetRowHeight() const {
  return static_cast<float>(GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(GetNative()), "row-height")));
}

void Table::SetHasBorder(bool yes) {
  if (yes == HasBorder())
    return;
  if (yes) {
    ApplyStyle(GetNative(), "border", "scrolledwindow { border: 1px solid }");
  } else {
    void* border = g_object_get_data(G_OBJECT(GetNative()), "border");
    gtk_style_context_remove_provider(gtk_widget_get_style_context(GetNative()),
                                      GTK_STYLE_PROVIDER(border));
  }
}

bool Table::HasBorder() const {
  return g_object_get_data(G_OBJECT(GetNative()), "border");
}

void Table::EnableMultipleSelection(bool enable) {
  GtkTreeSelection* selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()), "widget")));
  gtk_tree_selection_set_mode(selection, enable ? GTK_SELECTION_MULTIPLE
                                                : GTK_SELECTION_SINGLE);
}

bool Table::IsMultipleSelectionEnabled() const {
  GtkTreeSelection* selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()), "widget")));
  GtkSelectionMode mode = gtk_tree_selection_get_mode(selection);
  return mode == GTK_SELECTION_MULTIPLE;
}

void Table::SelectRow(int row) {
  SelectRows({row});
}

int Table::GetSelectedRow() const {
  GtkTreeSelection* selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()), "widget")));
  GtkTreeIter iter;
  if (gtk_tree_selection_get_selected(selection, nullptr, &iter))
    return GPOINTER_TO_INT(iter.user_data);
  return -1;
}

void Table::SelectRows(std::set<int> rows) {
  GtkTreeSelection* selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()), "widget")));
  gtk_tree_selection_unselect_all(selection);
  for (int row : rows) {
    GtkTreeIter iter = {true, GINT_TO_POINTER(row)};
    gtk_tree_selection_select_iter(selection, &iter);
  }
}

std::set<int> Table::GetSelectedRows() const {
  GtkTreeSelection* selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()), "widget")));
  GList* list = gtk_tree_selection_get_selected_rows(selection, nullptr);
  std::set<int> rows;
  for (GList* node = list; node != nullptr; node = node->next) {
    auto* path = static_cast<GtkTreePath*>(node->data);
    gint* indices = gtk_tree_path_get_indices(path);
    if (!indices) {
      NOTREACHED();
      continue;
    }
    rows.insert(indices[0]);
  }
  g_list_foreach(list, (GFunc)gtk_tree_path_free, nullptr);
  g_list_free(list);
  return rows;
}

void Table::NotifyRowInsertion(uint32_t row) {
  auto* tree_view = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()),
                                                    "widget"));
  auto* tree_model = gtk_tree_view_get_model(tree_view);
  if (!tree_model)
    return;
  GtkTreeIter iter = {true, GINT_TO_POINTER(row)};
  GtkTreePath* tree_path = gtk_tree_path_new_from_indices(row, -1);
  gtk_tree_model_row_inserted(tree_model, tree_path, &iter);
  gtk_tree_path_free(tree_path);
}

void Table::NotifyRowDeletion(uint32_t row) {
  auto* tree_view = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()),
                                                    "widget"));
  auto* tree_model = gtk_tree_view_get_model(tree_view);
  if (!tree_model)
    return;
  GtkTreePath* tree_path = gtk_tree_path_new_from_indices(row, -1);
  gtk_tree_model_row_deleted(tree_model, tree_path);
  gtk_tree_path_free(tree_path);
}

void Table::NotifyValueChange(uint32_t column, uint32_t row) {
  auto* tree_view = GTK_TREE_VIEW(g_object_get_data(G_OBJECT(GetNative()),
                                                    "widget"));
  auto* tree_model = gtk_tree_view_get_model(tree_view);
  if (!tree_model)
    return;
  GtkTreeIter iter = {true, GINT_TO_POINTER(row)};
  GtkTreePath* tree_path = gtk_tree_path_new_from_indices(row, -1);
  gtk_tree_model_row_changed(tree_model, tree_path, &iter);
  gtk_tree_path_free(tree_path);
}

}  // namespace nu
