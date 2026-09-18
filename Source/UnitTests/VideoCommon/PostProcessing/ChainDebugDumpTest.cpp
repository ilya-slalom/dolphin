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

TEST_F(ChainDebugDumpTest, DelaySkipsEarlyFrames)
{
  Config::SetCurrent(Config::GFX_LIBRASHADER_DUMP_CHAIN_IMAGES, true);
  Config::SetCurrent(Config::GFX_LIBRASHADER_DUMP_CHAIN_DELAY_FRAMES, 2u);
  // The frames right after a boot are the console's black screen, so a dump that always lands on
  // the first one measures nothing: input and output both read back bit-exact zero.
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());
  EXPECT_TRUE(VideoCommon::ShouldDumpChainImages());
}

TEST_F(ChainDebugDumpTest, DelayCountsOnlyFramesWithTheFlagSet)
{
  Config::SetCurrent(Config::GFX_LIBRASHADER_DUMP_CHAIN_DELAY_FRAMES, 1u);
  // Frames seen with the dump off must not burn the delay down, or turning it on later would dump
  // on whatever frame came next -- which is the failure the delay exists to avoid.
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());
  Config::SetCurrent(Config::GFX_LIBRASHADER_DUMP_CHAIN_IMAGES, true);
  EXPECT_FALSE(VideoCommon::ShouldDumpChainImages());
  EXPECT_TRUE(VideoCommon::ShouldDumpChainImages());
}
