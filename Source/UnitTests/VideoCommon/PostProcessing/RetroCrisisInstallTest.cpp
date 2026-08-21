// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/RetroCrisisInstall.h"

using namespace VideoCommon;

TEST(RetroCrisisInstall, ProfileOfExtractsTopFolder)
{
  EXPECT_EQ(RetroCrisisProfileOf("retro crisis/1080p Curved/nes.slangp"), "1080p Curved");
  EXPECT_EQ(RetroCrisisProfileOf("retro crisis/4K Flat/snes.slangp"), "4K Flat");
  EXPECT_EQ(RetroCrisisProfileOf("readme.txt"), "");
}

TEST(RetroCrisisInstall, ClosureFollowsReferenceChain)
{
  // 1080p Curved -> 1080p Flat -> 4K Flat
  const std::map<std::string, std::set<std::string>> refs = {
      {"1080p Curved", {"1080p Flat"}},
      {"1080p Flat", {"4K Flat"}},
      {"4K Flat", {}},
      {"1440p Flat", {"4K Flat"}},
  };
  EXPECT_EQ(ComputeRetroCrisisClosure("1080p Curved", refs),
            (std::set<std::string>{"1080p Curved", "1080p Flat", "4K Flat"}));
  EXPECT_EQ(ComputeRetroCrisisClosure("4K Flat", refs),
            (std::set<std::string>{"4K Flat"}));
}
