// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/PostProcessingConfig.h"

using namespace VideoCommon;

TEST(PostProcessingConfig, LegacyChainResolvesToItsFirstEntry)
{
  // Configs written before the chain feature was removed hold a ';'-separated list. The
  // librashader path already used only the first entry; now every path does, and out loud.
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset("a.slangp;b.slangp"), "a.slangp");
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset("a.slangp"), "a.slangp");
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset(""), "");
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset(";b.slangp"), "");
}

TEST(PostProcessingConfig, TrimsWhitespaceFromFirstEntry)
{
  // A hand-edited GFX.ini may be spaced out.
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset(" a.slangp "), "a.slangp");
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset("\ta.slangp\t"), "a.slangp");
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset(" a.slangp ;b.slangp"), "a.slangp");
}

TEST(PostProcessingConfig, NoWarningForEmptyTail)
{
  // A tail of nothing but separators and whitespace drops nothing a user would want to hear about.
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset("a.slangp;"), "a.slangp");
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset("a.slangp; "), "a.slangp");
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset("a.slangp;\t"), "a.slangp");
  EXPECT_EQ(VideoCommon::ResolveConfiguredPreset("a.slangp;;"), "a.slangp");
}
