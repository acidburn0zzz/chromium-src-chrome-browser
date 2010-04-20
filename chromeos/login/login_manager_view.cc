// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/login_manager_view.h"

#include <signal.h>
#include <sys/types.h>

#include <algorithm>
#include <vector>

#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/keyboard_codes.h"
#include "base/logging.h"
#include "base/process_util.h"
#include "base/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_thread.h"
#include "chrome/browser/chromeos/browser_notification_observers.h"
#include "chrome/browser/chromeos/cros/cros_library.h"
#include "chrome/browser/chromeos/cros/network_library.h"
#include "chrome/browser/chromeos/login/authentication_notification_details.h"
#include "chrome/browser/chromeos/login/login_utils.h"
#include "chrome/browser/chromeos/login/rounded_rect_painter.h"
#include "chrome/browser/chromeos/login/screen_observer.h"
#include "chrome/browser/chromeos/login/user_manager.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/profile_manager.h"
#include "chrome/common/notification_service.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"
#include "views/controls/button/native_button.h"
#include "views/controls/label.h"
#include "views/widget/widget.h"
#include "views/window/non_client_view.h"
#include "views/window/window.h"
#include "views/window/window_gtk.h"

using views::Background;
using views::Label;
using views::Textfield;
using views::View;
using views::Widget;

namespace {

const int kTitleY = 100;
const int kPanelSpacing = 36;
const int kVersionPad = 4;
const int kTextfieldWidth = 286;
const int kRowPad = 10;
const int kLabelPad = 2;
const int kLanguageMenuOffsetTop = 25;
const int kLanguageMenuOffsetRight = 25;
const int kLanguagesMenuWidth = 200;
const int kLanguagesMenuHeight = 30;
const SkColor kErrorColor = 0xFF8F384F;
const SkColor kLabelColor = 0xFF808080;
const SkColor kVersionColor = 0xFFA0A0A0;
const char *kDefaultDomain = "@gmail.com";

// Set to true to run on linux and test login.
const bool kStubOutLogin = false;

}  // namespace

namespace chromeos {

LoginManagerView::LoginManagerView(ScreenObserver* observer)
    : username_field_(NULL),
      password_field_(NULL),
      os_version_label_(NULL),
      title_label_(NULL),
      error_label_(NULL),
      sign_in_button_(NULL),
      create_account_link_(NULL),
      accel_focus_user_(views::Accelerator(base::VKEY_U, false, false, true)),
      accel_focus_pass_(views::Accelerator(base::VKEY_P, false, false, true)),
      observer_(observer),
      error_id_(-1),
      ALLOW_THIS_IN_INITIALIZER_LIST(focus_grabber_factory_(this)),
      focus_delayed_(false),
      language_switch_model_(observer,
                             ScreenObserver::LANGUAGE_CHANGED_ON_LOGIN) {
  // Create login observer to record time of login when successful.
  LogLoginSuccessObserver::Get();
  if (kStubOutLogin)
    authenticator_ = new StubAuthenticator(this);
  else
    authenticator_ = LoginUtils::Get()->CreateAuthenticator(this);
}

LoginManagerView::~LoginManagerView() {
}

void LoginManagerView::Init() {
  // Use rounded rect background.
  views::Painter* painter = CreateWizardPainter(
      &BorderDefinition::kScreenBorder);
  set_background(
      views::Background::CreateBackgroundPainter(true, painter));

  // Set up fonts.
  gfx::Font title_font =
      gfx::Font::CreateFont(L"Droid Sans", 10).DeriveFont(0, gfx::Font::BOLD);
  gfx::Font label_font = gfx::Font::CreateFont(L"Droid Sans", 8);
  gfx::Font button_font = label_font;
  gfx::Font field_font = label_font;
  gfx::Font version_font = gfx::Font::CreateFont(L"Droid Sans", 6);

  title_label_ = new views::Label();
  title_label_->SetHorizontalAlignment(views::Label::ALIGN_LEFT);
  title_label_->SetFont(title_font);
  AddChildView(title_label_);

  username_field_ = new views::Textfield;
  username_field_->SetFont(field_font);
  AddChildView(username_field_);

  password_field_ = new views::Textfield(views::Textfield::STYLE_PASSWORD);
  password_field_->SetFont(field_font);
  AddChildView(password_field_);

  sign_in_button_ = new views::NativeButton(this, std::wstring());
  sign_in_button_->set_font(button_font);
  AddChildView(sign_in_button_);

  create_account_link_ = new views::Link(std::wstring());
  create_account_link_->SetController(this);
  create_account_link_->SetFont(label_font);
  AddChildView(create_account_link_);

  os_version_label_ = new views::Label();
  os_version_label_->SetHorizontalAlignment(views::Label::ALIGN_LEFT);
  os_version_label_->SetColor(kVersionColor);
  os_version_label_->SetFont(version_font);
  AddChildView(os_version_label_);

  error_label_ = new views::Label();
  error_label_->SetHorizontalAlignment(views::Label::ALIGN_LEFT);
  error_label_->SetColor(kErrorColor);
  error_label_->SetFont(label_font);
  AddChildView(error_label_);

  language_switch_model_.InitLanguageMenu();
  languages_menubutton_ = new views::MenuButton(
      NULL, std::wstring(), &language_switch_model_, true);
  AddChildView(languages_menubutton_);

  AddAccelerator(accel_focus_user_);
  AddAccelerator(accel_focus_pass_);

  UpdateLocalizedStrings();

  // Restore previously logged in user.
  std::vector<UserManager::User> users = UserManager::Get()->GetUsers();
  if (users.size() > 0) {
    username_field_->SetText(UTF8ToUTF16(users[0].email()));
  }
  RequestFocus();

  // Controller to handle events from textfields
  username_field_->SetController(this);
  password_field_->SetController(this);
  if (CrosLibrary::Get()->EnsureLoaded()) {
    loader_.GetVersion(
        &consumer_, NewCallback(this, &LoginManagerView::OnOSVersion));
  } else if (!kStubOutLogin) {
    error_label_->SetText(
        ASCIIToWide(CrosLibrary::Get()->load_error_string()));
    username_field_->SetReadOnly(true);
    password_field_->SetReadOnly(true);
  }
}

bool LoginManagerView::AcceleratorPressed(
    const views::Accelerator& accelerator) {
  if (accelerator == accel_focus_user_) {
    username_field_->RequestFocus();
    return true;
  }

  if (accelerator == accel_focus_pass_) {
    password_field_->RequestFocus();
    return true;
  }

  return false;
}

void LoginManagerView::UpdateLocalizedStrings() {
  title_label_->SetText(l10n_util::GetString(IDS_LOGIN_TITLE));
  username_field_->set_text_to_display_when_empty(
      l10n_util::GetStringUTF16(IDS_LOGIN_USERNAME));
  password_field_->set_text_to_display_when_empty(
      l10n_util::GetStringUTF16(IDS_LOGIN_PASSWORD));
  sign_in_button_->SetLabel(l10n_util::GetString(IDS_LOGIN_BUTTON));
  create_account_link_->SetText(
      l10n_util::GetString(IDS_CREATE_ACCOUNT_BUTTON));
  ShowError(error_id_);
  languages_menubutton_->SetText(language_switch_model_.GetCurrentLocaleName());
}

void LoginManagerView::RequestFocus() {
  MessageLoop::current()->PostTask(FROM_HERE,
      focus_grabber_factory_.NewRunnableMethod(
          &LoginManagerView::FocusFirstField));
}

void LoginManagerView::ViewHierarchyChanged(bool is_add,
                                            View *parent,
                                            View *child) {
  if (is_add && child == this) {
    MessageLoop::current()->PostTask(FROM_HERE,
        focus_grabber_factory_.NewRunnableMethod(
            &LoginManagerView::FocusFirstField));
  }
}

void LoginManagerView::NativeViewHierarchyChanged(bool attached,
                                                  gfx::NativeView native_view,
                                                  views::RootView* root_view) {
  if (focus_delayed_ && attached) {
    focus_delayed_ = false;
    MessageLoop::current()->PostTask(FROM_HERE,
        focus_grabber_factory_.NewRunnableMethod(
            &LoginManagerView::FocusFirstField));
  }
}

void LoginManagerView::FocusFirstField() {
  if (GetFocusManager()) {
    if (username_field_->text().empty())
      username_field_->RequestFocus();
    else
      password_field_->RequestFocus();
  } else {
    // We are invisible - delay until it is no longer the case.
    focus_delayed_ = true;
  }
}

// Sets the bounds of the view, using x and y as the origin.
// The width is determined by the min of width and the preferred size
// of the view, unless force_width is true in which case it is always used.
// The height is gotten from the preferred size and returned.
static int setViewBounds(
    views::View* view, int x, int y, int width, bool force_width) {
  gfx::Size pref_size = view->GetPreferredSize();
  if (!force_width) {
    if (pref_size.width() < width) {
      width = pref_size.width();
    }
  }
  int height = pref_size.height();
  view->SetBounds(x, y, width, height);
  return height;
}

void LoginManagerView::Layout() {
  // Center the text fields, and align the rest of the views with them.
  int x = (width() - kTextfieldWidth) / 2;
  int y = kTitleY;
  int max_width = width() - (x + kVersionPad);

  y += (setViewBounds(title_label_, x, y, max_width, false) + kRowPad);
  y += (setViewBounds(username_field_, x, y, kTextfieldWidth, true) + kRowPad);
  y += (setViewBounds(password_field_, x, y, kTextfieldWidth, true) + kRowPad);
  y += (setViewBounds(sign_in_button_, x, y, kTextfieldWidth, false) + kRowPad);
  y += setViewBounds(create_account_link_, x, y, kTextfieldWidth, false);
  y += kRowPad;

  int padding = BorderDefinition::kScreenBorder.shadow +
                BorderDefinition::kScreenBorder.corner_radius / 2;

  y += setViewBounds(
      error_label_,
      padding,
      y,
      width() - 2 * padding,
      true);

  setViewBounds(
      os_version_label_,
      padding,
      height() - (os_version_label_->GetPreferredSize().height() + padding),
      width() - 2 * padding,
      true);

  x = width() - kLanguagesMenuWidth - kLanguageMenuOffsetRight;
  y = kLanguageMenuOffsetTop;
  languages_menubutton_->SetBounds(x, y,
                                   kLanguagesMenuWidth, kLanguagesMenuHeight);
  SchedulePaint();
}

gfx::Size LoginManagerView::GetPreferredSize() {
  return gfx::Size(width(), height());
}

views::View* LoginManagerView::GetContentsView() {
  return this;
}

void LoginManagerView::SetUsername(const std::string& username) {
  username_field_->SetText(UTF8ToUTF16(username));
}

void LoginManagerView::SetPassword(const std::string& password) {
  password_field_->SetText(UTF8ToUTF16(password));
}

void LoginManagerView::Login() {
  // Disallow 0 size username.
  if (username_field_->text().empty()) {
    // Return true so that processing ends
    return;
  }
  std::string username = UTF16ToUTF8(username_field_->text());
  // todo(cmasone) Need to sanitize memory used to store password.
  std::string password = UTF16ToUTF8(password_field_->text());

  if (username.find('@') == std::string::npos) {
    username += kDefaultDomain;
    username_field_->SetText(UTF8ToUTF16(username));
  }

  Profile* profile = g_browser_process->profile_manager()->GetWizardProfile();
  ChromeThread::PostTask(
      ChromeThread::FILE, FROM_HERE,
      NewRunnableMethod(authenticator_.get(),
                        &Authenticator::Authenticate,
                        profile, username, password));
}

// Sign in button causes a login attempt.
void LoginManagerView::ButtonPressed(
    views::Button* sender, const views::Event& event) {
  DCHECK(sender == sign_in_button_);
  Login();
}

void LoginManagerView::LinkActivated(views::Link* source, int event_flags) {
  DCHECK(source == create_account_link_);
  observer_->OnExit(ScreenObserver::LOGIN_CREATE_ACCOUNT);
}

void LoginManagerView::OnLoginFailure(const std::string& error) {
  LOG(INFO) << "LoginManagerView: OnLoginFailure() " << error;
  NetworkLibrary* network = CrosLibrary::Get()->GetNetworkLibrary();

  // Send notification of failure
  AuthenticationNotificationDetails details(false);
  NotificationService::current()->Notify(
      NotificationType::LOGIN_AUTHENTICATION, Source<LoginManagerView>(this),
      Details<AuthenticationNotificationDetails>(&details));

  // Check networking after trying to login in case user is
  // cached locally or the local admin account.
  if (!network || !CrosLibrary::Get()->EnsureLoaded()) {
    ShowError(IDS_LOGIN_ERROR_NO_NETWORK_LIBRARY);
  } else if (!network->Connected()) {
    ShowError(IDS_LOGIN_ERROR_OFFLINE_FAILED_NETWORK_NOT_CONNECTED);
  } else {
    ShowError(IDS_LOGIN_ERROR_AUTHENTICATING);
    // TODO(someone): get |error| onto the UI somehow?
  }
  SetPassword(std::string());
  password_field_->RequestFocus();
}

void LoginManagerView::OnLoginSuccess(const std::string& username,
                                      const std::string& credentials) {
  // TODO(cmasone): something sensible if errors occur.
  if (observer_) {
    observer_->OnExit(ScreenObserver::LOGIN_SIGN_IN_SELECTED);
  }

  LoginUtils::Get()->CompleteLogin(username, credentials);
}

void LoginManagerView::ShowError(int error_id) {
  error_id_ = error_id;
  error_label_->SetText((error_id_ == -1)
                        ? std::wstring()
                        : l10n_util::GetString(error_id_));
}

bool LoginManagerView::HandleKeystroke(views::Textfield* s,
    const views::Textfield::Keystroke& keystroke) {
  if (!kStubOutLogin && !CrosLibrary::Get()->EnsureLoaded())
    return false;

  if (keystroke.GetKeyboardCode() == base::VKEY_TAB) {
    if (username_field_->text().length() != 0) {
      std::string username = UTF16ToUTF8(username_field_->text());

      if (username.find('@') == std::string::npos) {
        username += kDefaultDomain;
        username_field_->SetText(UTF8ToUTF16(username));
      }
      return false;
    }
  } else if (keystroke.GetKeyboardCode() == base::VKEY_RETURN) {
    Login();
    // Return true so that processing ends
    return true;
  } else if (error_id_ != -1) {
    // Clear all previous error messages.
    ShowError(-1);
    return false;
  }
  // Return false so that processing does not end
  return false;
}

void LoginManagerView::OnOSVersion(
    VersionLoader::Handle handle,
    std::string version) {
  os_version_label_->SetText(ASCIIToWide(version));
}

}  // namespace chromeos
