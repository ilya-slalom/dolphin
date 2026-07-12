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

TEST(SlangTranslator, DeduplicatesSamplersReusingBindingInBranches)
{
  // crt-royale declares the same layout(binding = N) in mutually-exclusive #ifdef/#else
  // branches. Counting textual sampler2D names over-counts; counting distinct binding
  // numbers gives the true count. Only one branch's names are live per compile, but the
  // translator (no preprocessor) sees both -- it must not treat them as extra samplers.
  SlangShaderSource shader;
  shader.vertex_source = "void main() {}\n";
  shader.fragment_source =
      "layout(binding = 2) uniform sampler2D Source;\n"
      "#ifdef USE_LARGE\n"
      "layout(binding = 3) uniform sampler2D mask_grille_large;\n"
      "layout(binding = 4) uniform sampler2D mask_slot_large;\n"
      "layout(binding = 5) uniform sampler2D mask_shadow_large;\n"
      "#else\n"
      "layout(binding = 3) uniform sampler2D mask_grille_small;\n"
      "layout(binding = 4) uniform sampler2D mask_slot_small;\n"
      "layout(binding = 5) uniform sampler2D mask_shadow_small;\n"
      "#endif\n"
      "layout(location = 0) out vec4 FragColor;\n"
      "void main() { FragColor = vec4(1); }\n";
  const auto result = TranslateSlangPass(shader, {}, {});
  // 4 distinct bindings (2,3,4,5) -> under the 8 limit despite 7 textual declarations.
  EXPECT_TRUE(result.ok) << result.error;
}

TEST(SlangTranslator, IgnoresSampler2DFunctionParameters)
{
  // crt-royale helpers take `const sampler2D tex` parameters; these are not uniform
  // declarations and must not be counted as bindable samplers.
  SlangShaderSource shader;
  shader.vertex_source = "void main() {}\n";
  shader.fragment_source =
      "float4 helper(const sampler2D tex, vec2 uv) { return texture(tex, uv); }\n"
      "layout(binding = 2) uniform sampler2D Source;\n"
      "layout(location = 0) out vec4 FragColor;\n"
      "void main() { FragColor = helper(Source, vec2(0.5)); }\n";
  const auto result = TranslateSlangPass(shader, {}, {});
  ASSERT_TRUE(result.ok) << result.error;
  // Only "Source" -- the "tex" parameter is not a sampler binding.
  EXPECT_EQ(result.sampler_names.size(), 1u);
  EXPECT_EQ(result.sampler_names[0], "Source");
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
