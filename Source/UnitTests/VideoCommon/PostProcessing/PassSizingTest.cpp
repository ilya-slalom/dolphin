// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/PassSizing.h"
#include "VideoCommon/PostProcessing/SlangPreset.h"

using namespace VideoCommon;

TEST(PassSizing, Source)
{
  EXPECT_EQ(ComputePassAxisSize(ScaleType::Source, 1.0f, 640, 1920), 640u);
  EXPECT_EQ(ComputePassAxisSize(ScaleType::Source, 0.5f, 640, 1920), 320u);
}
TEST(PassSizing, Viewport)
{
  EXPECT_EQ(ComputePassAxisSize(ScaleType::Viewport, 1.0f, 640, 1920), 1920u);
  EXPECT_EQ(ComputePassAxisSize(ScaleType::Viewport, 0.0625f, 640, 1920), 120u);
}
TEST(PassSizing, Absolute)
{
  EXPECT_EQ(ComputePassAxisSize(ScaleType::Absolute, 320.0f, 640, 1920), 320u);
}
TEST(PassSizing, ClampsToOne)
{
  EXPECT_EQ(ComputePassAxisSize(ScaleType::Absolute, 0.0f, 640, 1920), 1u);
}
