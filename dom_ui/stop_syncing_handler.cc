// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/dom_ui/stop_syncing_handler.h"

#include "app/l10n_util.h"
#include "base/basictypes.h"
#include "base/values.h"
#include "base/callback.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "chrome/browser/sync/profile_sync_service.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/profile_manager.h"

StopSyncingHandler::StopSyncingHandler() {
}

StopSyncingHandler::~StopSyncingHandler() {
}

void StopSyncingHandler::GetLocalizedValues(
    DictionaryValue* localized_strings) {
  DCHECK(localized_strings);
  localized_strings->SetString(L"stop_syncing_explanation",
      l10n_util::GetStringF(IDS_SYNC_STOP_SYNCING_EXPLANATION_LABEL,
          l10n_util::GetString(IDS_PRODUCT_NAME)));
  localized_strings->SetString(L"stop_syncing_title",
      l10n_util::GetString(IDS_SYNC_STOP_SYNCING_DIALOG_TITLE));
  localized_strings->SetString(L"stop_syncing_confirm",
      l10n_util::GetString(IDS_SYNC_STOP_SYNCING_CONFIRM_BUTTON_LABEL));
}

void StopSyncingHandler::RegisterMessages() {
  DCHECK(dom_ui_);
  dom_ui_->RegisterMessageCallback("stopSyncing",
      NewCallback(this, &StopSyncingHandler::StopSyncing));
}

void StopSyncingHandler::StopSyncing(const Value* value){
  DCHECK(dom_ui_);
  ProfileSyncService* service = dom_ui_->GetProfile()->GetProfileSyncService();
  if(service != NULL && ProfileSyncService::IsSyncEnabled()) {
    service->DisableForUser();
    ProfileSyncService::SyncEvent(ProfileSyncService::STOP_FROM_OPTIONS);
  }
}
