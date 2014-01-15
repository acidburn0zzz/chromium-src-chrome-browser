// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/input_method/candidate_window_controller_impl.h"

#include <string>
#include <vector>

#include "ash/ime/infolist_window.h"
#include "ash/shell.h"
#include "ash/shell_window_ids.h"
#include "ash/wm/window_util.h"
#include "base/logging.h"
#include "chrome/browser/chromeos/input_method/mode_indicator_controller.h"
#include "ui/gfx/screen.h"
#include "ui/views/widget/widget.h"

namespace chromeos {
namespace input_method {

namespace {

}  // namespace

CandidateWindowControllerImpl::CandidateWindowControllerImpl()
    : candidate_window_view_(NULL),
      infolist_window_(NULL) {
  IBusBridge::Get()->SetCandidateWindowHandler(this);
  // Create the mode indicator controller.
  mode_indicator_controller_.reset(
      new ModeIndicatorController(InputMethodManager::Get()));
}

CandidateWindowControllerImpl::~CandidateWindowControllerImpl() {
  IBusBridge::Get()->SetCandidateWindowHandler(NULL);
  if (candidate_window_view_) {
    candidate_window_view_->RemoveObserver(this);
    candidate_window_view_->GetWidget()->RemoveObserver(this);
  }
}

void CandidateWindowControllerImpl::InitCandidateWindowView() {
  if (candidate_window_view_)
    return;

  aura::Window* active_window = ash::wm::GetActiveWindow();
  candidate_window_view_ =
      new ash::ime::CandidateWindowView(ash::Shell::GetContainer(
          active_window ?
          active_window->GetRootWindow() : ash::Shell::GetTargetRootWindow(),
          ash::internal::kShellWindowId_InputMethodContainer));
  candidate_window_view_->AddObserver(this);
  candidate_window_view_->SetCursorBounds(cursor_bounds_, composition_head_);
  views::Widget* widget = candidate_window_view_->InitWidget();
  widget->AddObserver(this);
  widget->Show();
  FOR_EACH_OBSERVER(CandidateWindowController::Observer, observers_,
                    CandidateWindowOpened());
}

void CandidateWindowControllerImpl::Hide() {
  if (candidate_window_view_)
    candidate_window_view_->GetWidget()->Close();
  if (infolist_window_)
    infolist_window_->HideImmediately();
}

void CandidateWindowControllerImpl::SetCursorBounds(
    const gfx::Rect& cursor_bounds,
    const gfx::Rect& composition_head) {
  // A workaround for http://crosbug.com/6460. We should ignore very short Y
  // move to prevent the window from shaking up and down.
  const int kKeepPositionThreshold = 2;  // px
  gfx::Rect last_bounds;
  if (candidate_window_view_)
    last_bounds = candidate_window_view_->GetAnchorRect();

  const int delta_y = abs(last_bounds.y() - cursor_bounds.y());
  if ((last_bounds.x() == cursor_bounds.x()) &&
      (delta_y <= kKeepPositionThreshold)) {
    DVLOG(1) << "Ignored set_cursor_bounds signal to prevent window shake";
    return;
  }

  cursor_bounds_ = cursor_bounds;
  composition_head_ = composition_head;

  // Remember the cursor bounds.
  if (candidate_window_view_)
    candidate_window_view_->SetCursorBounds(cursor_bounds, composition_head);

  // Mode indicator controller also needs the cursor bounds.
  mode_indicator_controller_->SetCursorBounds(cursor_bounds);
}

void CandidateWindowControllerImpl::FocusStateChanged(bool is_focused) {
  mode_indicator_controller_->FocusStateChanged(is_focused);
}

void CandidateWindowControllerImpl::UpdateLookupTable(
    const ui::CandidateWindow& candidate_window,
    bool visible) {
  // If it's not visible, hide the lookup table and return.
  if (!visible) {
    if (candidate_window_view_)
      candidate_window_view_->HideLookupTable();
    if (infolist_window_)
      infolist_window_->HideImmediately();
    // TODO(nona): Introduce unittests for crbug.com/170036.
    latest_infolist_entries_.clear();
    return;
  }

  if (!candidate_window_view_)
    InitCandidateWindowView();
  candidate_window_view_->UpdateCandidates(candidate_window);
  candidate_window_view_->ShowLookupTable();

  bool has_highlighted = false;
  std::vector<ui::InfolistEntry> infolist_entries;
  candidate_window.GetInfolistEntries(&infolist_entries, &has_highlighted);

  // If there is no change, just return.
  if (latest_infolist_entries_ == infolist_entries)
    return;

  latest_infolist_entries_ = infolist_entries;

  if (infolist_entries.empty()) {
    if (infolist_window_)
      infolist_window_->HideImmediately();
    return;
  }

  // Highlight moves out of the infolist entries.
  if (!has_highlighted) {
    if (infolist_window_)
      infolist_window_->HideWithDelay();
    return;
  }

  if (infolist_window_) {
    infolist_window_->Relayout(infolist_entries);
  } else {
    infolist_window_ = new ash::ime::InfolistWindow(
        candidate_window_view_, infolist_entries);
    infolist_window_->InitWidget();
    infolist_window_->GetWidget()->AddObserver(this);
  }
  infolist_window_->ShowWithDelay();
}

void CandidateWindowControllerImpl::UpdatePreeditText(
    const std::string& utf8_text, unsigned int cursor, bool visible) {
  // If it's not visible, hide the preedit text and return.
  if (!visible || utf8_text.empty()) {
    if (candidate_window_view_)
      candidate_window_view_->HidePreeditText();
    return;
  }
  if (!candidate_window_view_)
    InitCandidateWindowView();
  candidate_window_view_->UpdatePreeditText(utf8_text);
  candidate_window_view_->ShowPreeditText();
}

void CandidateWindowControllerImpl::OnCandidateCommitted(int index) {
  FOR_EACH_OBSERVER(CandidateWindowController::Observer, observers_,
                    CandidateClicked(index));
}

void CandidateWindowControllerImpl::OnWidgetClosing(views::Widget* widget) {
  if (infolist_window_ && widget == infolist_window_->GetWidget()) {
    widget->RemoveObserver(this);
    infolist_window_ = NULL;
  } else if (candidate_window_view_ &&
             widget == candidate_window_view_->GetWidget()) {
    widget->RemoveObserver(this);
    candidate_window_view_->RemoveObserver(this);
    candidate_window_view_ = NULL;
    FOR_EACH_OBSERVER(CandidateWindowController::Observer, observers_,
                      CandidateWindowClosed());
  }
}

void CandidateWindowControllerImpl::AddObserver(
    CandidateWindowController::Observer* observer) {
  observers_.AddObserver(observer);
}

void CandidateWindowControllerImpl::RemoveObserver(
    CandidateWindowController::Observer* observer) {
  observers_.RemoveObserver(observer);
}

}  // namespace input_method
}  // namespace chromeos
