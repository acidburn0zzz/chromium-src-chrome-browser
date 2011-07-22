// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_NTP_NEW_TAB_SYNC_SETUP_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_NTP_NEW_TAB_SYNC_SETUP_HANDLER_H_

#include "chrome/browser/ui/webui/sync_setup_handler.h"

// The handler for Javascript messages related to the sync setup UI in the new
// tab page.
class NewTabSyncSetupHandler : public SyncSetupHandler {
 protected:
  virtual void ShowSetupUI();
};

#endif  // CHROME_BROWSER_UI_WEBUI_NTP_NEW_TAB_SYNC_SETUP_HANDLER_H_
