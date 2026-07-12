// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include <gtest/gtest.h>

#include "VideoCommon/PostProcessing/SlangShader.h"
#include "VideoCommon/PostProcessing/SlangTranslator.h"

using namespace VideoCommon;

static SlangShaderSource MakeShader(const std::string& frag_extra)
{
  SlangShaderSource s;
  s.vertex_source =
      "layout(std140) uniform UBO { mat4 MVP; vec4 SourceSize; };\n"
      "void main() { gl_Position = MVP * vec4(0); }\n";
  s.fragment_source =
      "layout(std140) uniform UBO { mat4 MVP; vec4 SourceSize; };\n"
      "layout(binding = 1) uniform sampler2D Source;\n"
      "layout(location = 0) out vec4 FragColor;\n" +
      frag_extra +
      "void main() { FragColor = texture(Source, vec2(0.5)); }\n";
  return s;
}

TEST(SlangTranslator, EmitsBindingMacrosAndSourceSampler)
{
  const auto result = TranslateSlangPass(MakeShader(""), {}, {});
  ASSERT_TRUE(result.ok) << result.error;
  // Source is always sampler binding 0.
  ASSERT_FALSE(result.sampler_names.empty());
  EXPECT_EQ(result.sampler_names[0], "Source");
  // The translator injects Dolphin's sampler macro rather than raw layout(binding=).
  EXPECT_NE(result.fragment_glsl.find("SAMPLER_BINDING(0)"), std::string::npos);
  // The UBO uses Dolphin's UBO_BINDING macro.
  EXPECT_NE(result.fragment_glsl.find("UBO_BINDING"), std::string::npos);
}

TEST(SlangTranslator, AssignsBindingsToAliasesAndLuts)
{
  auto shader = MakeShader(
      "layout(binding = 2) uniform sampler2D BLOOM_APPROX;\n"
      "layout(binding = 3) uniform sampler2D MASK;\n");
  const auto result = TranslateSlangPass(shader, {"BLOOM_APPROX"}, {"MASK"});
  ASSERT_TRUE(result.ok) << result.error;
  // Source at 0, then referenced aliases/LUTs get subsequent binding indices.
  EXPECT_EQ(result.sampler_names[0], "Source");
  const auto& names = result.sampler_names;
  EXPECT_NE(std::find(names.begin(), names.end(), "BLOOM_APPROX"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "MASK"), names.end());
}

TEST(SlangTranslator, RejectsTooManySamplers)
{
  // Build a shader referencing 9 distinct samplers (Source + 8) -> over the 8 limit.
  SlangShaderSource shader = MakeShader("");
  std::vector<std::string> luts;
  for (int i = 0; i < 8; ++i)
  {
    const std::string name = "LUT" + std::to_string(i);
    shader.fragment_source +=
        "layout(binding = " + std::to_string(i + 2) + ") uniform sampler2D " + name + ";\n";
    luts.push_back(name);
  }
  const auto result = TranslateSlangPass(shader, {}, luts);
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.error.empty());
}
