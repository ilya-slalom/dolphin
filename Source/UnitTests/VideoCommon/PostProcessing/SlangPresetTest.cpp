// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/SlangPreset.h"

using namespace VideoCommon;

TEST(SlangPreset, ParsesPassCountAndPaths)
{
  const std::string text =
      "shaders = \"2\"\n"
      "shader0 = \"a.slang\"\n"
      "alias0 = \"FIRST\"\n"
      "filter_linear0 = \"true\"\n"
      "scale_type0 = \"source\"\n"
      "scale0 = \"1.0\"\n"
      "shader1 = \"../b.slang\"\n"
      "scale_type_x1 = \"viewport\"\n"
      "scale_x1 = \"0.5\"\n"
      "scale_type_y1 = \"absolute\"\n"
      "scale_y1 = \"240\"\n";
  std::string error;
  const auto cfg = ParseSlangPreset(text, "/root/preset", &error);
  ASSERT_TRUE(cfg.has_value()) << error;
  ASSERT_EQ(cfg->passes.size(), 2u);
  EXPECT_EQ(cfg->passes[0].shader_path, "/root/preset/a.slang");
  EXPECT_EQ(cfg->passes[0].alias, "FIRST");
  EXPECT_TRUE(cfg->passes[0].filter_linear);
  EXPECT_EQ(cfg->passes[0].scale_type_x, ScaleType::Source);
  EXPECT_EQ(cfg->passes[0].scale_type_y, ScaleType::Source);
  // "../b.slang" resolves against the preset dir's parent.
  EXPECT_EQ(cfg->passes[1].shader_path, "/root/b.slang");
  EXPECT_EQ(cfg->passes[1].scale_type_x, ScaleType::Viewport);
  EXPECT_FLOAT_EQ(cfg->passes[1].scale_x, 0.5f);
  EXPECT_EQ(cfg->passes[1].scale_type_y, ScaleType::Absolute);
  EXPECT_FLOAT_EQ(cfg->passes[1].scale_y, 240.0f);
}

TEST(SlangPreset, ParsesTextures)
{
  const std::string text =
      "shaders = \"1\"\n"
      "shader0 = \"a.slang\"\n"
      "textures = \"MASK;GRID\"\n"
      "MASK = \"masks/m.png\"\n"
      "MASK_wrap_mode = \"repeat\"\n"
      "MASK_linear = \"true\"\n"
      "MASK_mipmap = \"true\"\n"
      "GRID = \"g.png\"\n";
  std::string error;
  const auto cfg = ParseSlangPreset(text, "/root", &error);
  ASSERT_TRUE(cfg.has_value()) << error;
  ASSERT_EQ(cfg->luts.size(), 2u);
  EXPECT_EQ(cfg->luts[0].name, "MASK");
  EXPECT_EQ(cfg->luts[0].path, "/root/masks/m.png");
  EXPECT_EQ(cfg->luts[0].wrap_mode, SlangWrapMode::Repeat);
  EXPECT_TRUE(cfg->luts[0].linear);
  EXPECT_TRUE(cfg->luts[0].mipmap);
  EXPECT_EQ(cfg->luts[1].name, "GRID");
}

TEST(SlangPreset, RejectsMissingShaderCount)
{
  std::string error;
  const auto cfg = ParseSlangPreset("shader0 = \"a.slang\"\n", "/root", &error);
  EXPECT_FALSE(cfg.has_value());
  EXPECT_FALSE(error.empty());
}

TEST(SlangPreset, CollectsNonStructuralKeysAsParameterOverrides)
{
  const std::string text =
      "shaders = 1\n"
      "shader0 = stock.slang\n"
      "filter_linear0 = true\n"
      "scale0 = 2.0\n"
      "scale_x0 = 1.5\n"
      "textures = LUT\n"
      "LUT = lut.png\n"
      "LUT_linear = true\n"
      "masksize = 2.000000\n"
      "mask_zoom = -1.500000\n"
      "some_label = notanumber\n";  // non-numeric -> ignored

  std::string error;
  const auto config = ParseSlangPreset(text, "/base", &error);
  ASSERT_TRUE(config.has_value()) << error;

  EXPECT_EQ(config->parameter_overrides.size(), 2u);
  EXPECT_FLOAT_EQ(config->parameter_overrides.at("masksize"), 2.0f);
  EXPECT_FLOAT_EQ(config->parameter_overrides.at("mask_zoom"), -1.5f);
  EXPECT_EQ(config->parameter_overrides.count("shaders"), 0u);
  EXPECT_EQ(config->parameter_overrides.count("shader0"), 0u);
  EXPECT_EQ(config->parameter_overrides.count("filter_linear0"), 0u);
  EXPECT_EQ(config->parameter_overrides.count("scale0"), 0u);
  EXPECT_EQ(config->parameter_overrides.count("scale_x0"), 0u);
  EXPECT_EQ(config->parameter_overrides.count("textures"), 0u);
  EXPECT_EQ(config->parameter_overrides.count("LUT"), 0u);
  EXPECT_EQ(config->parameter_overrides.count("LUT_linear"), 0u);
  EXPECT_EQ(config->parameter_overrides.count("some_label"), 0u);
}
