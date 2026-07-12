// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing.h"

// g_gfx is null in the unit-test environment, so ValidateShaderSource must
// exercise only the parse/structure path and report the GPU compile as deferred.
TEST(PostProcessingValidation, PlainShaderParsesWithoutGpu)
{
  const std::string code = "void main() { SetOutput(Sample()); }\n";
  const auto result = VideoCommon::PostProcessing::ValidateShaderSource(code);
  EXPECT_TRUE(result.valid);
  EXPECT_FALSE(result.gpu_compiled);
  EXPECT_TRUE(result.error_message.empty());
}

TEST(PostProcessingValidation, ConfigurationBlockParses)
{
  const std::string code =
      "[configuration]\n"
      "[OptionBool]\n"
      "GUIName = Invert\n"
      "OptionName = invert\n"
      "DefaultValue = false\n"
      "[/configuration]\n"
      "void main() { SetOutput(Sample()); }\n";
  const auto result = VideoCommon::PostProcessing::ValidateShaderSource(code);
  EXPECT_TRUE(result.valid);
}

TEST(PostProcessingValidation, EmptySourceIsInvalid)
{
  const auto result = VideoCommon::PostProcessing::ValidateShaderSource("");
  EXPECT_FALSE(result.valid);
  EXPECT_FALSE(result.error_message.empty());
}
