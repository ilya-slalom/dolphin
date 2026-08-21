// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/ShaderPackSource.h"

using namespace VideoCommon;

TEST(ShaderPackSource, LibretroAndSatpixieRegistered)
{
  const ShaderPackSource* libretro = FindShaderPackSource("libretro");
  ASSERT_NE(libretro, nullptr);
  EXPECT_EQ(libretro->install_subdir, "shaders_slang");
  EXPECT_TRUE(libretro->extract_subpath.empty());
  EXPECT_TRUE(libretro->depends_on.empty());

  const ShaderPackSource* satpixie = FindShaderPackSource("satpixie");
  ASSERT_NE(satpixie, nullptr);
  EXPECT_EQ(satpixie->install_subdir, "shaders_slang");
  EXPECT_EQ(satpixie->extract_subpath, "satpixie-crt-shader/RetroArch/shaders/shaders_slang/");
}

TEST(ShaderPackSource, UnknownIdReturnsNull)
{
  EXPECT_EQ(FindShaderPackSource("nope"), nullptr);
}
