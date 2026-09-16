// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace VideoCommon
{
// Where the librashader dynamic library is packaged for this platform. librashader_ld.h loads by
// bare name, which cannot find a library inside a macOS .app bundle, so we hand it an absolute
// path -- the same treatment VulkanLoader gives libMoltenVK. On platforms where the loader's
// default search already works (Android, Linux) this returns the bare file name.
std::string LibrashaderLibraryPath();
}  // namespace VideoCommon
