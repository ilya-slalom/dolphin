// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/ShaderChainSpec.h"

using namespace VideoCommon;

TEST(ShaderChainSpec, SplitsAndIgnoresEmptyEntries)
{
  EXPECT_EQ(SplitChainSpec(""), (std::vector<std::string>{}));
  EXPECT_EQ(SplitChainSpec("crt/a.slangp"), (std::vector<std::string>{"crt/a.slangp"}));
  EXPECT_EQ(SplitChainSpec("crt/a.slangp;misc/b.slangp"),
            (std::vector<std::string>{"crt/a.slangp", "misc/b.slangp"}));
  EXPECT_EQ(SplitChainSpec(";crt/a.slangp;;"), (std::vector<std::string>{"crt/a.slangp"}));
}

TEST(ShaderChainSpec, TrimsWhitespaceAroundEntries)
{
  // A hand-edited GFX.ini may be spaced out; MultipassPostProcessing::LoadPreset resolves the same
  // names this returns, so the two must not disagree about whether " a " is "a".
  EXPECT_EQ(SplitChainSpec("crt/a.slangp ; misc/b.slangp"),
            (std::vector<std::string>{"crt/a.slangp", "misc/b.slangp"}));
  EXPECT_EQ(SplitChainSpec("\tcrt/a.slangp\t"), (std::vector<std::string>{"crt/a.slangp"}));
  // An entry that is only whitespace is empty, not a preset named " ".
  EXPECT_EQ(SplitChainSpec("  ;  "), (std::vector<std::string>{}));
}

TEST(ShaderChainSpec, JoinsWithSemicolons)
{
  EXPECT_EQ(JoinChainSpec({}), "");
  EXPECT_EQ(JoinChainSpec({"a", "b"}), "a;b");
}

TEST(ShaderChainSpec, DescribesChainWithArrows)
{
  EXPECT_EQ(DescribeChainSpec(""), "");
  EXPECT_EQ(DescribeChainSpec("crt/a.slangp"), "crt/a.slangp");
  EXPECT_EQ(DescribeChainSpec("crt/a.slangp;misc/b.slangp"), "crt/a.slangp \xE2\x86\x92 misc/b.slangp");
}

TEST(ShaderChainSpec, AppendsToChain)
{
  EXPECT_EQ(AppendToChainSpec("", "a"), "a");
  EXPECT_EQ(AppendToChainSpec("a", "b"), "a;b");
  EXPECT_EQ(AppendToChainSpec("a;b", "c"), "a;b;c");
  // Appending nothing is a no-op rather than a trailing separator.
  EXPECT_EQ(AppendToChainSpec("a", ""), "a");
}

TEST(ShaderChainSpec, DerivesCategoryFromLeadingDirectory)
{
  EXPECT_EQ(ChainCategoryOf("crt/crt-royale.slangp"), "crt");
  EXPECT_EQ(ChainCategoryOf("crt/nested/x.slangp"), "crt");
  // A preset at the root has no category.
  EXPECT_EQ(ChainCategoryOf("plain.slangp"), "");
}

TEST(ShaderChainSpec, ListsDistinctSortedCategories)
{
  const std::vector<std::string> presets = {"misc/b.slangp", "crt/a.slangp", "crt/c.slangp",
                                            "root.slangp"};
  EXPECT_EQ(ChainCategories(presets), (std::vector<std::string>{"crt", "misc"}));
}

TEST(ShaderChainSpec, FiltersPresetsByCategory)
{
  const std::vector<std::string> presets = {"misc/b.slangp", "crt/a.slangp", "root.slangp"};
  EXPECT_EQ(PresetsInCategory(presets, "crt"), (std::vector<std::string>{"crt/a.slangp"}));
  // An empty category means "all".
  EXPECT_EQ(PresetsInCategory(presets, ""), presets);
}
