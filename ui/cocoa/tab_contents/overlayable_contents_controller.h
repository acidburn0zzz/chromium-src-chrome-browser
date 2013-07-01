// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_COCOA_TAB_CONTENTS_OVERLAYABLE_CONTENTS_CONTROLLER_H_
#define CHROME_BROWSER_UI_COCOA_TAB_CONTENTS_OVERLAYABLE_CONTENTS_CONTROLLER_H_

#import <Cocoa/Cocoa.h>

#include "base/mac/scoped_nsobject.h"
#include "base/memory/scoped_ptr.h"
#include "chrome/common/instant_types.h"

class Browser;
class InstantOverlayControllerMac;

namespace content {
class WebContents;
}

// OverlayableContentsController manages the display of up to two tab contents
// views. It is primarily for use with Instant results. This class supports the
// notion of an "active" view vs. an "overlay" tab contents view.
//
// The "active" view is a container view that can be retrieved using
// |-activeContainer|. Its contents are meant to be managed by an external
// class.
//
// The "overlay" can be set using |-showOverlay:| and |-hideOverlay|. When an
// overlay is set, the active view is hidden (but stays in the view hierarchy).
// When the overlay is removed, the active view is reshown.
@interface OverlayableContentsController : NSViewController {
 @private
  // Container view for the "active" contents.
  base::scoped_nsobject<NSView> activeContainer_;

  // The overlay WebContents. Will be NULL if no overlay is currently showing.
  content::WebContents* overlayContents_;  // weak

  // C++ bridge to the Instant model change interface.
  scoped_ptr<InstantOverlayControllerMac> instantOverlayController_;

  // The desired height of the overlay and units.
  CGFloat overlayHeight_;
  InstantSizeUnits overlayHeightUnits_;
}

@property(readonly, nonatomic) NSView* activeContainer;

// Initialization.
- (id)initWithBrowser:(Browser*)browser;

// Sets the current overlay and installs its WebContentsView into the view
// hierarchy. Hides the active view. If |overlay| is NULL then closes the
// current overlay and shows the active view.
- (void)setOverlay:(content::WebContents*)overlay
            height:(CGFloat)height
       heightUnits:(InstantSizeUnits)heightUnits
    drawDropShadow:(BOOL)drawDropShadow;

// Called when a tab with |contents| is activated, so that we can check to see
// if it's the overlay being activated (and adjust internal state accordingly).
- (void)onActivateTabWithContents:(content::WebContents*)contents;

- (InstantOverlayControllerMac*)instantOverlayController;

- (BOOL)isShowingOverlay;

@end

#endif  // CHROME_BROWSER_UI_COCOA_TAB_CONTENTS_OVERLAYABLE_CONTENTS_CONTROLLER_H_
