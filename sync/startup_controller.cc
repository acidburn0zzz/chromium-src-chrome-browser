// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sync/startup_controller.h"

#include "base/command_line.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/histogram.h"
#include "chrome/browser/managed_mode/managed_user_signin_manager_wrapper.h"
#include "chrome/browser/signin/profile_oauth2_token_service.h"
#include "chrome/browser/sync/sync_prefs.h"
#include "chrome/common/chrome_switches.h"

namespace browser_sync {

namespace {

// The amount of time we'll wait to initialize sync if no data type triggers
// initialization via a StartSyncFlare.
const int kDeferredInitFallbackSeconds = 10;

// Enum (for UMA, primarily) defining different events that cause us to
// exit the "deferred" state of initialization and invoke start_backend.
enum DeferredInitTrigger {
  // We have received a signal from a SyncableService requesting that sync
  // starts as soon as possible.
  TRIGGER_DATA_TYPE_REQUEST,
  // No data type requested sync to start and our fallback timer expired.
  TRIGGER_FALLBACK_TIMER,
  MAX_TRIGGER_VALUE
};

}  // namespace

StartupController::StartupController(
    ProfileSyncServiceStartBehavior start_behavior,
    ProfileOAuth2TokenService* token_service,
    const browser_sync::SyncPrefs* sync_prefs,
    const ManagedUserSigninManagerWrapper* signin,
    base::Closure start_backend)
    : received_start_request_(false),
      setup_in_progress_(false),
      auto_start_enabled_(start_behavior == AUTO_START),
      sync_prefs_(sync_prefs),
      token_service_(token_service),
      signin_(signin),
      start_backend_(start_backend),
      fallback_timeout_(
          base::TimeDelta::FromSeconds(kDeferredInitFallbackSeconds)),
      weak_factory_(this) {}

StartupController::~StartupController() {}

void StartupController::Reset() {
  received_start_request_ = false;
  setup_in_progress_ = false;
  start_up_time_ = base::Time();
  start_backend_time_ = base::Time();
  // Don't let previous timers affect us post-reset.
  weak_factory_.InvalidateWeakPtrs();
}

void StartupController::set_setup_in_progress(bool in_progress) {
  setup_in_progress_ = in_progress;
}

bool StartupController::StartUp(StartUpDeferredOption deferred_option) {
  const bool first_start = start_up_time_.is_null();
  if (first_start)
    start_up_time_ = base::Time::Now();

  if (deferred_option == STARTUP_BACKEND_DEFERRED &&
      CommandLine::ForCurrentProcess()->
          HasSwitch(switches::kSyncEnableDeferredStartup)) {
    if (first_start) {
      base::MessageLoop::current()->PostDelayedTask(
          FROM_HERE,
          base::Bind(&StartupController::OnFallbackStartupTimerExpired,
                     weak_factory_.GetWeakPtr()), fallback_timeout_);
    }
    return false;
  }

  if (start_backend_time_.is_null()) {
    start_backend_time_ = base::Time::Now();
    start_backend_.Run();
  }

  return true;
}

void StartupController::OverrideFallbackTimeoutForTest(
    const base::TimeDelta& timeout) {
  fallback_timeout_ = timeout;
}

bool StartupController::TryStart() {
  if (sync_prefs_->IsManaged())
    return false;

  if (sync_prefs_->IsStartSuppressed())
    return false;

  if (signin_->GetEffectiveUsername().empty())
    return false;

  if (!token_service_)
    return false;

  if (!token_service_->RefreshTokenIsAvailable(
          signin_->GetAccountIdToUse())) {
    return false;
  }

  // TODO(tim): Seems wrong to always record this histogram here...
  // If we got here then tokens are loaded and user logged in and sync is
  // enabled. If OAuth refresh token is not available then something is wrong.
  // When PSS requests access token, OAuth2TokenService will return error and
  // PSS will show error to user asking to reauthenticate.
  UMA_HISTOGRAM_BOOLEAN("Sync.RefreshTokenAvailable", true);

  // If sync setup has completed we always start the backend. If the user is in
  // the process of setting up now, we should start the backend to download
  // account control state / encryption information). If autostart is enabled,
  // but we haven't completed sync setup, we try to start sync anyway, since
  // it's possible we crashed/shutdown after logging in but before the backend
  // finished initializing the last time.
  //
  // However, the only time we actually need to start sync _immediately_ is if
  // we haven't completed sync setup and the user is in the process of setting
  // up - either they just signed in (for the first time) on an auto-start
  // platform or they explicitly kicked off sync setup, and e.g we need to
  // fetch account details like encryption state to populate UI. Otherwise,
  // for performance reasons and maximizing parallelism at chrome startup, we
  // defer the heavy lifting for sync init until things have calmed down.
  if (sync_prefs_->HasSyncSetupCompleted()) {
    if (!received_start_request_)
      return StartUp(STARTUP_BACKEND_DEFERRED);
    else
      return StartUp(STARTUP_IMMEDIATE);
  } else if (setup_in_progress_ || auto_start_enabled_) {
    // We haven't completed sync setup. Start immediately if the user explicitly
    // kicked this off or we're supposed to automatically start syncing.
    return StartUp(STARTUP_IMMEDIATE);
  }

  return false;
}

void StartupController::OnFallbackStartupTimerExpired() {
  DCHECK(CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kSyncEnableDeferredStartup));

  if (!start_backend_time_.is_null())
    return;

  DVLOG(2) << "Sync deferred init fallback timer expired, starting backend.";
  DCHECK(!start_up_time_.is_null());
  base::TimeDelta time_deferred = base::Time::Now() - start_up_time_;
  UMA_HISTOGRAM_TIMES("Sync.Startup.TimeDeferred", time_deferred);
  UMA_HISTOGRAM_ENUMERATION("Sync.Startup.DeferredInitTrigger",
                            TRIGGER_FALLBACK_TIMER,
                            MAX_TRIGGER_VALUE);
  received_start_request_ = true;
  TryStart();
}

std::string StartupController::GetBackendInitializationStateString() const {
  if (!start_backend_time_.is_null())
    return "Started";
  else if (!start_up_time_.is_null())
    return "Deferred";
  else
    return "Not started";
}

void StartupController::OnDataTypeRequestsSyncStartup(syncer::ModelType type) {
  if (!CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kSyncEnableDeferredStartup)) {
    DVLOG(2) << "Ignoring data type request for sync startup: "
             << syncer::ModelTypeToString(type);
    return;
  }

  if (!start_backend_time_.is_null())
    return;

  DVLOG(2) << "Data type requesting sync startup: "
           << syncer::ModelTypeToString(type);
  // Measure the time spent waiting for init and the type that triggered it.
  // We could measure the time spent deferred on a per-datatype basis, but
  // for now this is probably sufficient.
  if (!start_up_time_.is_null()) {
    base::TimeDelta time_deferred = base::Time::Now() - start_up_time_;
    UMA_HISTOGRAM_TIMES("Sync.Startup.TimeDeferred", time_deferred);
    UMA_HISTOGRAM_ENUMERATION("Sync.Startup.TypeTriggeringInit",
                              ModelTypeToHistogramInt(type),
                              syncer::MODEL_TYPE_COUNT);
    UMA_HISTOGRAM_ENUMERATION("Sync.Startup.DeferredInitTrigger",
                              TRIGGER_DATA_TYPE_REQUEST,
                              MAX_TRIGGER_VALUE);
  }
  received_start_request_ = true;
  TryStart();
}

}  // namespace browser_sync