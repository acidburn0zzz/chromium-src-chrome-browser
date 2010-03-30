// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_EXTENSIONS_CRX_INSTALLER_H_
#define CHROME_BROWSER_EXTENSIONS_CRX_INSTALLER_H_

#include <string>

#include "base/file_path.h"
#include "base/ref_counted.h"
#include "base/task.h"
#include "chrome/browser/extensions/extension_install_ui.h"
#include "chrome/browser/extensions/extensions_service.h"
#include "chrome/browser/extensions/sandboxed_extension_unpacker.h"
#include "chrome/common/extensions/extension.h"

class SkBitmap;

// This class installs a crx file into a profile.
//
// Installing a CRX is a multi-step process, including unpacking the crx,
// validating it, prompting the user, and installing. Since many of these
// steps must occur on the file thread, this class contains a copy of all data
// necessary to do its job. (This also minimizes external dependencies for
// easier testing).
//
// Lifetime management:
//
// This class is ref-counted by each call it makes to itself on another thread,
// and by UtilityProcessHost.
//
// Additionally, we hold a reference to our own client so that it lives at least
// long enough to receive the result of unpacking.
//
// IMPORTANT: Callers should keep a reference to a CrxInstaller while they are
// working with it, eg:
//
// scoped_refptr<CrxInstaller> installer(new CrxInstaller(...));
// installer->set_foo();
// installer->set_bar();
// installer->InstallCrx(...);
class CrxInstaller
    : public SandboxedExtensionUnpackerClient,
      public ExtensionInstallUI::Delegate {
 public:
  // Constructor.  Extensions will be unpacked to |install_directory|.
  // Extension objects will be sent to |frontend|, and any UI will be shown
  // via |client|. For silent install, pass NULL for |client|.
  CrxInstaller(const FilePath& install_directory,
               ExtensionsService* frontend,
               ExtensionInstallUI* client);

  // Install the crx in |source_file|. Note that this will most likely
  // complete asynchronously.
  void InstallCrx(const FilePath& source_file);

  // Install the user script in |source_file|. Note that this will most likely
  // complete asynchronously.
  void InstallUserScript(const FilePath& source_file,
                         const GURL& original_url);

  // ExtensionInstallUI::Delegate
  virtual void InstallUIProceed(bool create_app_shortcut);
  virtual void InstallUIAbort();

  const GURL& original_url() const { return original_url_; }
  void set_original_url(const GURL& val) { original_url_ = val; }

  Extension::Location install_source() const { return install_source_; }
  void set_install_source(Extension::Location source) {
    install_source_ = source;
  }

  const std::string& expected_id() const { return expected_id_; }
  void set_expected_id(const std::string& val) { expected_id_ = val; }

  bool delete_source() const { return delete_source_; }
  void set_delete_source(bool val) { delete_source_ = val; }

  bool allow_privilege_increase() const { return allow_privilege_increase_; }
  void set_allow_privilege_increase(bool val) {
    allow_privilege_increase_ = val;
  }

  bool force_web_origin_to_download_url() const {
    return force_web_origin_to_download_url_;
  }
  void set_force_web_origin_to_download_url(bool val) {
    force_web_origin_to_download_url_ = val;
  }

 private:
  ~CrxInstaller();

  // Converts the source user script to an extension.
  void ConvertUserScriptOnFileThread();

  // SandboxedExtensionUnpackerClient
  virtual void OnUnpackFailure(const std::string& error_message);
  virtual void OnUnpackSuccess(const FilePath& temp_dir,
                               const FilePath& extension_dir,
                               Extension* extension);

  // Runs on the UI thread. Confirms with the user (via ExtensionInstallUI) that
  // it is OK to install this extension.
  void ConfirmInstall();

  // Runs on File thread. Install the unpacked extension into the profile and
  // notify the frontend.
  void CompleteInstall();

  // Result reporting.
  void ReportFailureFromFileThread(const std::string& error);
  void ReportFailureFromUIThread(const std::string& error);
  void ReportOverinstallFromFileThread();
  void ReportOverinstallFromUIThread();
  void ReportSuccessFromFileThread();
  void ReportSuccessFromUIThread();

  // The file we're installing.
  FilePath source_file_;

  // The URL the file was downloaded from.
  GURL original_url_;

  // The directory extensions are installed to.
  FilePath install_directory_;

  // The location the installation came from (bundled with Chromium, registry,
  // manual install, etc). This metadata is saved with the installation if
  // successful. Defaults to INTERNAL.
  Extension::Location install_source_;

  // For updates and external installs we have an ID we're expecting the
  // extension to contain.
  std::string expected_id_;

  // Whether manual extension installation is enabled. We can't just check this
  // before trying to install because themes are special-cased to always be
  // allowed.
  bool extensions_enabled_;

  // Whether we're supposed to delete the source file on destruction. Defaults
  // to false.
  bool delete_source_;

  // Whether privileges should be allowed to silently increaes from any
  // previously installed version of the extension. This is used for things
  // like external extensions, where extensions come with third-party software
  // or are distributed by the network administrator. There is no UI shown
  // for these extensions, so there shouldn't be UI for privilege increase,
  // either. Defaults to false.
  bool allow_privilege_increase_;

  // If true and the installed extension uses web content, the web origin will
  // be forced to the origin of |original_url_|. Defaults to false.
  bool force_web_origin_to_download_url_;

  // Whether to create an app shortcut after successful installation. This is
  // set based on the user's selection in the UI and can only ever be true for
  // apps.
  bool create_app_shortcut_;

  // The extension we're installing. We own this and either pass it off to
  // ExtensionsService on success, or delete it on failure.
  scoped_ptr<Extension> extension_;

  // If non-empty, contains the current version of the extension we're
  // installing (for upgrades).
  std::string current_version_;

  // The icon we will display in the installation UI, if any.
  scoped_ptr<SkBitmap> install_icon_;

  // The temp directory extension resources were unpacked to. We own this and
  // must delete it when we are done with it.
  FilePath temp_dir_;

  // The frontend we will report results back to.
  scoped_refptr<ExtensionsService> frontend_;

  // The client we will work with to do the installation. This can be NULL, in
  // which case the install is silent.
  // NOTE: we may be deleted on the file thread. To ensure the UI is deleted on
  // the main thread we don't use a scoped_ptr here.
  ExtensionInstallUI* client_;

  // The root of the unpacked extension directory. This is a subdirectory of
  // temp_dir_, so we don't have to delete it explicitly.
  FilePath unpacked_extension_root_;

  DISALLOW_COPY_AND_ASSIGN(CrxInstaller);
};

#endif  // CHROME_BROWSER_EXTENSIONS_CRX_INSTALLER_H_
