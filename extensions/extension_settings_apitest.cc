// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/command_line.h"
#include "chrome/browser/extensions/extension_apitest.h"
#include "chrome/common/chrome_switches.h"
#include "net/base/mock_host_resolver.h"

class ExtensionSettingsApiTest : public ExtensionApiTest {
};

IN_PROC_BROWSER_TEST_F(ExtensionSettingsApiTest, Settings) {
  CommandLine::ForCurrentProcess()->AppendSwitch(
      switches::kEnableExperimentalExtensionApis);
  ASSERT_TRUE(RunExtensionTest("settings")) << message_;
}
