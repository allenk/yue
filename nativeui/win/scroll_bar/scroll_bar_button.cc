// Copyright 2016 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/win/scroll_bar/scroll_bar_button.h"

#include "nativeui/state.h"
#include "nativeui/win/scroll_bar/scroll_bar.h"

namespace nu {

ScrollBarButton::ScrollBarButton(Type type, ScrollBarView* scroll_bar)
    : BaseView(ControlType::ScrollBarButton),
      theme_(State::current()->GetNativeTheme()),
      repeater_(base::Bind(&ScrollBarButton::OnClick,
                           base::Unretained(this))),
      type_(type),
      scroll_bar_(scroll_bar) {
}

ScrollBarButton::~ScrollBarButton() {
}

void ScrollBarButton::OnMouseEnter() {
  set_state(ControlState::Hovered);
  Invalidate();
}

void ScrollBarButton::OnMouseLeave() {
  set_state(ControlState::Normal);
  repeater_.Stop();
  Invalidate();
}

void ScrollBarButton::OnMouseClick(UINT message, UINT flags, const Point&) {
  if (message == WM_LBUTTONDOWN) {
    set_state(ControlState::Pressed);
    OnClick();
    repeater_.Start();
  } else {
    set_state(ControlState::Hovered);
    repeater_.Stop();
  }
  Invalidate();
}

void ScrollBarButton::Draw(PainterWin* painter, const Rect& dirty) {
  HDC dc = painter->GetHDC();
  theme_->PaintScrollbarArrow(
      dc, static_cast<int>(type_), state(),
      Rect(size_allocation().size()) + painter->origin(), params_);
  painter->ReleaseHDC(dc);
}

void ScrollBarButton::OnClick() {
  if (type_ == Up || type_ == Left)
    scroll_bar_->LineUp();
  else
    scroll_bar_->LineDown();
}

}  // namespace nu