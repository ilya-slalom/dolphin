// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/MipGen.h"

using namespace VideoCommon;

TEST(MipGen, HalvesDownToOne)
{
  // 2x2 solid white.
  std::vector<u8> img(2 * 2 * 4, 255);
  const auto mips = GenerateBoxMips(2, 2, img.data());
  ASSERT_EQ(mips.size(), 2u);  // 2x2 -> 1x1
  EXPECT_EQ(mips[0].width, 2u);
  EXPECT_EQ(mips[1].width, 1u);
  EXPECT_EQ(mips[1].height, 1u);
  // Average of four white texels is white.
  EXPECT_EQ(mips[1].rgba8[0], 255);
}

TEST(MipGen, AveragesColors)
{
  // 2x1 image: black and white side by side.
  std::vector<u8> img = {0, 0, 0, 255, 255, 255, 255, 255};
  const auto mips = GenerateBoxMips(2, 1, img.data());
  ASSERT_EQ(mips.size(), 2u);  // 2x1 -> 1x1
  // Averaged red channel ~127-128.
  EXPECT_NEAR(mips[1].rgba8[0], 127, 1);
}

TEST(MipGen, NonSquareChain)
{
  std::vector<u8> img(4 * 2 * 4, 128);
  const auto mips = GenerateBoxMips(4, 2, img.data());
  // 4x2 -> 2x1 -> 1x1
  ASSERT_EQ(mips.size(), 3u);
  EXPECT_EQ(mips[1].width, 2u);
  EXPECT_EQ(mips[1].height, 1u);
  EXPECT_EQ(mips[2].width, 1u);
}
