// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Common/MathUtil.h"
#include "VideoCommon/PostProcessing/ChainOutputPolicy.h"

using VideoCommon::ChooseDynamicRendering;
using VideoCommon::ShouldRenderChainDirectly;

// The chain may render straight into the backbuffer only when the draw rect IS the backbuffer:
// then librashader's OutputSize equals the drawn extent either way, so skipping the intermediate
// target cannot change a pixel.
TEST(ChainOutputPolicy, FullBackbufferRectRendersDirectly)
{
  EXPECT_TRUE(ShouldRenderChainDirectly(MathUtil::Rectangle<int>(0, 0, 1920, 1080), 1920, 1080));
}

TEST(ChainOutputPolicy, PillarboxedRectUsesIntermediateTarget)
{
  // Same height, narrower and offset: OutputSize would be wrong if rendered into the backbuffer.
  EXPECT_FALSE(
      ShouldRenderChainDirectly(MathUtil::Rectangle<int>(240, 0, 1680, 1080), 1920, 1080));
}

TEST(ChainOutputPolicy, LetterboxedRectUsesIntermediateTarget)
{
  EXPECT_FALSE(ShouldRenderChainDirectly(MathUtil::Rectangle<int>(0, 60, 1920, 1020), 1920, 1080));
}

TEST(ChainOutputPolicy, StereoHalfRectUsesIntermediateTarget)
{
  // Side-by-side stereo: full height, half width, at the origin.
  EXPECT_FALSE(ShouldRenderChainDirectly(MathUtil::Rectangle<int>(0, 0, 960, 1080), 1920, 1080));
}

TEST(ChainOutputPolicy, RectLargerThanBackbufferUsesIntermediateTarget)
{
  EXPECT_FALSE(ShouldRenderChainDirectly(MathUtil::Rectangle<int>(0, 0, 1920, 1080), 1280, 720));
}

// Dynamic rendering needs both the enabled device feature and a resolvable vkCmdBeginRendering;
// librashader looks the core name up itself and would silently fall back otherwise, so we only
// ask for it when we know it will be taken.
TEST(DynamicRenderingPolicy, RequiresFeatureAndEntryPoint)
{
  EXPECT_TRUE(ChooseDynamicRendering(true, true));
  EXPECT_FALSE(ChooseDynamicRendering(true, false));
  EXPECT_FALSE(ChooseDynamicRendering(false, true));
  EXPECT_FALSE(ChooseDynamicRendering(false, false));
}
