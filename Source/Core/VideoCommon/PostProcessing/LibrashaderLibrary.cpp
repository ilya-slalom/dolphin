// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/LibrashaderLibrary.h"

#include "Common/FileUtil.h"

namespace VideoCommon
{
std::string LibrashaderLibraryPath()
{
  // Check ANDROID first: it is Linux, so it would otherwise fall through to the generic #else arm.
#if defined(ANDROID)
  return "librashader.so";
#elif defined(__APPLE__)
  return File::GetBundleDirectory() + "/Contents/Frameworks/librashader.dylib";
#elif defined(_WIN32)
  return File::GetExeDirectory() + "\\librashader.dll";
#else
  return "librashader.so";
#endif
}
}  // namespace VideoCommon
