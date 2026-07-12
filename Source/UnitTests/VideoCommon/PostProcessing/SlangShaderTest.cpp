// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/SlangShader.h"

using namespace VideoCommon;

TEST(SlangShader, SplitsStagesAndSharesPrologue)
{
  const std::string text =
      "#pragma name FIRST_PASS\n"
      "#pragma format R16G16B16A16_SFLOAT\n"
      "layout(std140) uniform UBO { vec4 SourceSize; };\n"  // common prologue
      "#pragma stage vertex\n"
      "void main() { gl_Position = vec4(0); }\n"
      "#pragma stage fragment\n"
      "layout(location = 0) out vec4 FragColor;\n"
      "void main() { FragColor = vec4(1); }\n";
  std::string error;
  const auto shader = ParseSlangShader(text, &error);
  ASSERT_TRUE(shader.has_value()) << error;
  EXPECT_EQ(shader->name, "FIRST_PASS");
  EXPECT_EQ(shader->format, "R16G16B16A16_SFLOAT");
  // Common prologue appears in both stages.
  EXPECT_NE(shader->vertex_source.find("uniform UBO"), std::string::npos);
  EXPECT_NE(shader->fragment_source.find("uniform UBO"), std::string::npos);
  // Stage-specific bodies land in the right stage only.
  EXPECT_NE(shader->vertex_source.find("gl_Position"), std::string::npos);
  EXPECT_EQ(shader->vertex_source.find("FragColor"), std::string::npos);
  EXPECT_NE(shader->fragment_source.find("FragColor"), std::string::npos);
}

TEST(SlangShader, ParsesParameters)
{
  const std::string text =
      "#pragma parameter BRIGHTNESS \"Brightness\" 1.0 0.0 2.0 0.05\n"
      "#pragma stage vertex\n"
      "void main() {}\n"
      "#pragma stage fragment\n"
      "void main() {}\n";
  std::string error;
  const auto shader = ParseSlangShader(text, &error);
  ASSERT_TRUE(shader.has_value()) << error;
  ASSERT_EQ(shader->parameters.size(), 1u);
  EXPECT_EQ(shader->parameters[0].id, "BRIGHTNESS");
  EXPECT_EQ(shader->parameters[0].label, "Brightness");
  EXPECT_FLOAT_EQ(shader->parameters[0].default_value, 1.0f);
  EXPECT_FLOAT_EQ(shader->parameters[0].max_value, 2.0f);
}

TEST(SlangShader, RejectsMissingStages)
{
  std::string error;
  const auto shader = ParseSlangShader("void main() {}\n", &error);
  EXPECT_FALSE(shader.has_value());
  EXPECT_FALSE(error.empty());
}
