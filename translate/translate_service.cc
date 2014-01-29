// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/translate/translate_service.h"

#include "chrome/browser/browser_process.h"
#include "chrome/browser/translate/translate_manager.h"
#include "components/translate/core/browser/translate_download_manager.h"

// static
void TranslateService::Initialize() {
  // Create the TranslateManager singleton.
  TranslateManager::GetInstance();
  TranslateDownloadManager* download_manager =
      TranslateDownloadManager::GetInstance();
  download_manager->set_request_context(
      g_browser_process->system_request_context());
  download_manager->set_application_locale(
      g_browser_process->GetApplicationLocale());
}

// static
void TranslateService::Shutdown(bool cleanup_pending_fetcher) {
  if (cleanup_pending_fetcher)
    TranslateManager::GetInstance()->CleanupPendingUlrFetcher();
  TranslateDownloadManager::GetInstance()->set_request_context(NULL);
}