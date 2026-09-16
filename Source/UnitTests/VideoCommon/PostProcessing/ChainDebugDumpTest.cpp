// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/Config/GraphicsSettings.h"
#include "VideoCommon/PostProcessing/ChainDebugDump.h"

// Config::SetCurrent needs an initialised config system, so both tests run under a fixture that
// brackets them -- the same shape CoreTimingTest uses. The dump budget is process-global, so the
// fixture re-arms it too rather than letting either test depend on running first.
class ChainDebugDumpTest : public testing::Test
{
protected:
  void SetUp() override
  {
    Config::Init();
    VideoCommon::ResetChainImageDumpBudgetForTest();
  }

  void TearDown() override
  {
    VideoCommon::ResetChainImageDumpBudgetForTest();
    Config::Shutdown();
  }
};

TEST_F(ChainDebugDumpTest, DisabledByDefault)
{
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());
}

TEST_F(ChainDebugDumpTest, BudgetIsSpentAfterOneFrame)
{
  Config::SetCurrent(Config::GFX_LIBRASHADER_DUMP_CHAIN_IMAGES, true);
  EXPECT_TRUE(VideoCommon::ShouldDumpChainImages());
  VideoCommon::NoteChainImagesDumped();
  // A single frame is enough evidence, and a readback per frame would be unusable. The flag
  // stays set so a config reload is not needed, but the budget is gone.
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());
}
