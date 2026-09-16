// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/Config/GraphicsSettings.h"
#include "VideoCommon/PostProcessing/ChainDebugDump.h"

TEST(ChainDebugDump, DisabledByDefault)
{
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());
}

TEST(ChainDebugDump, BudgetIsSpentAfterOneFrame)
{
  // Resolution 1: Reset the process-global budget at the start of this test
  VideoCommon::ResetChainImageDumpBudgetForTest();

  // Initialize Config system for this test
  Config::Init();

  Config::SetCurrent(Config::GFX_LIBRASHADER_DUMP_CHAIN_IMAGES, true);
  EXPECT_TRUE(VideoCommon::ShouldDumpChainImages());
  VideoCommon::NoteChainImagesDumped();
  // A single frame is enough evidence, and a readback per frame would be unusable. The flag
  // stays set so a config reload is not needed, but the budget is gone.
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());

  // Clean up
  Config::Shutdown();
}
