// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/app_list/app_list_positioner.h"

#include <algorithm>

#include "base/logging.h"
#include "ui/gfx/point.h"
#include "ui/gfx/rect.h"

AppListPositioner::AppListPositioner(const gfx::Display& display,
                                     const gfx::Size& window_size,
                                     int min_distance_from_edge)
    : display_(display),
      window_size_(window_size),
      min_distance_from_edge_(min_distance_from_edge) {}

gfx::Point AppListPositioner::GetAnchorPointForScreenCorner(
    ScreenCorner corner) const {
  const gfx::Rect& screen_rect = display_.bounds();
  gfx::Point anchor;
  switch (corner) {
    case SCREEN_CORNER_TOP_LEFT:
      anchor = screen_rect.origin();
      break;
    case SCREEN_CORNER_TOP_RIGHT:
      anchor = screen_rect.top_right();
      break;
    case SCREEN_CORNER_BOTTOM_LEFT:
      anchor = screen_rect.bottom_left();
      break;
    case SCREEN_CORNER_BOTTOM_RIGHT:
      anchor = screen_rect.bottom_right();
      break;
    default:
      NOTREACHED();
      anchor = gfx::Point();
  }
  return ClampAnchorPoint(gfx::Rect(), anchor);
}

gfx::Point AppListPositioner::GetAnchorPointForShelfCorner(
    ScreenEdge shelf_edge,
    const gfx::Rect shelf_rect) const {
  const gfx::Rect& screen_rect = display_.bounds();
  gfx::Point anchor;
  switch (shelf_edge) {
    case SCREEN_EDGE_LEFT:
      anchor = gfx::Point(shelf_rect.right(), screen_rect.y());
      break;
    case SCREEN_EDGE_RIGHT:
      anchor = gfx::Point(shelf_rect.x(), screen_rect.y());
      break;
    case SCREEN_EDGE_TOP:
      anchor = gfx::Point(screen_rect.x(), shelf_rect.bottom());
      break;
    case SCREEN_EDGE_BOTTOM:
      anchor = gfx::Point(screen_rect.x(), shelf_rect.y());
      break;
    default:
      NOTREACHED();
      anchor = gfx::Point();
  }
  return ClampAnchorPoint(shelf_rect, anchor);
}

gfx::Point AppListPositioner::GetAnchorPointForShelfCursor(
    ScreenEdge shelf_edge,
    const gfx::Rect shelf_rect,
    const gfx::Point& cursor) const {
  gfx::Point anchor;
  switch (shelf_edge) {
    case SCREEN_EDGE_LEFT:
      anchor = gfx::Point(shelf_rect.right(), cursor.y());
      break;
    case SCREEN_EDGE_RIGHT:
      anchor = gfx::Point(shelf_rect.x(), cursor.y());
      break;
    case SCREEN_EDGE_TOP:
      anchor = gfx::Point(cursor.x(), shelf_rect.bottom());
      break;
    case SCREEN_EDGE_BOTTOM:
      anchor = gfx::Point(cursor.x(), shelf_rect.y());
      break;
    default:
      NOTREACHED();
      anchor = gfx::Point();
  }
  return ClampAnchorPoint(shelf_rect, anchor);
}

AppListPositioner::ScreenEdge AppListPositioner::GetShelfEdge(
    const gfx::Rect& shelf_rect) const {
  const gfx::Rect& screen_rect = display_.bounds();

  // If we can't find the shelf, return SCREEN_EDGE_UNKNOWN. If the display
  // size is the same as the work area, and does not contain the shelf, either
  // the shelf is hidden or on another monitor.
  if (display_.work_area() == screen_rect &&
      !display_.work_area().Contains(shelf_rect)) {
    return SCREEN_EDGE_UNKNOWN;
  }

  // Note: On Windows 8 the work area won't include split windows on the left or
  // right, and neither will |shelf_rect|.
  if (shelf_rect.width() == display_.work_area().width()) {
    // Shelf is horizontal.
    if (shelf_rect.bottom() == screen_rect.bottom())
      return SCREEN_EDGE_BOTTOM;
    else if (shelf_rect.y() == screen_rect.y())
      return SCREEN_EDGE_TOP;
  } else if (shelf_rect.height() == display_.work_area().height()) {
    // Shelf is vertical.
    if (shelf_rect.x() == screen_rect.x())
      return SCREEN_EDGE_LEFT;
    else if (shelf_rect.right() == screen_rect.right())
      return SCREEN_EDGE_RIGHT;
  }

  return SCREEN_EDGE_UNKNOWN;
}

int AppListPositioner::GetCursorDistanceFromShelf(
    ScreenEdge shelf_edge,
    const gfx::Rect& shelf_rect,
    const gfx::Point& cursor) const {
  switch (shelf_edge) {
    case SCREEN_EDGE_UNKNOWN:
      return 0;
    case SCREEN_EDGE_LEFT:
      return std::max(0, cursor.x() - shelf_rect.right());
    case SCREEN_EDGE_RIGHT:
      return std::max(0, shelf_rect.x() - cursor.x());
    case SCREEN_EDGE_TOP:
      return std::max(0, cursor.y() - shelf_rect.bottom());
    case SCREEN_EDGE_BOTTOM:
      return std::max(0, shelf_rect.y() - cursor.y());
    default:
      NOTREACHED();
      return 0;
  }
}

gfx::Point AppListPositioner::ClampAnchorPoint(const gfx::Rect shelf_rect,
                                               gfx::Point anchor) const {
  // Always subtract the shelf area since work_area() will not subtract it
  // if the shelf is set to auto-hide, and the window should never overlap
  // the shelf.
  gfx::Rect bounds_rect(display_.work_area());
  bounds_rect.Subtract(shelf_rect);

  // Anchor the center of the window in a region that prevents the window
  // showing outside of the work area.
  bounds_rect.Inset(window_size_.width() / 2 + min_distance_from_edge_,
                    window_size_.height() / 2 + min_distance_from_edge_);

  anchor.SetToMax(bounds_rect.origin());
  anchor.SetToMin(bounds_rect.bottom_right());
  return anchor;
}