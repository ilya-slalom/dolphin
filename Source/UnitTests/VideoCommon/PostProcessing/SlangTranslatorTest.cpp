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
  const auto result = TranslateSlangPass(MakeShader(""), {}, {}, /*flip_clip_y=*/false);
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
  const auto result = TranslateSlangPass(shader, {"BLOOM_APPROX"}, {"MASK"}, /*flip_clip_y=*/false);
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
  const auto result = TranslateSlangPass(shader, {}, {}, /*flip_clip_y=*/false);
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
  const auto result = TranslateSlangPass(shader, {}, {}, /*flip_clip_y=*/false);
  ASSERT_TRUE(result.ok) << result.error;
  // Only "Source" -- the "tex" parameter is not a sampler binding.
  EXPECT_EQ(result.sampler_names.size(), 1u);
  EXPECT_EQ(result.sampler_names[0], "Source");
}

TEST(SlangTranslator, PacksUniformsPerStd140)
{
  // A float followed by a vec4: std140 aligns the vec4 to 16 bytes, so the float occupies
  // bytes [0,4) and 12 bytes of padding precede the vec4 at [16,32).
  const std::vector<UboMember> members = {
      {"a", UboMemberType::Float},
      {"b", UboMemberType::Vec4},
  };
  const auto resolver = [](const std::string& name, float* out, int count) -> bool {
    if (name == "a")
    {
      out[0] = 1.0f;
      return true;
    }
    if (name == "b")
    {
      for (int i = 0; i < count; ++i)
        out[i] = static_cast<float>(10 + i);
      return true;
    }
    return false;
  };
  const std::vector<u8> data = PackSlangUniforms(members, resolver);
  ASSERT_GE(data.size(), 32u);
  const float* f = reinterpret_cast<const float*>(data.data());
  EXPECT_FLOAT_EQ(f[0], 1.0f);   // a at offset 0
  EXPECT_FLOAT_EQ(f[4], 10.0f);  // b.x at offset 16 (aligned)
  EXPECT_FLOAT_EQ(f[5], 11.0f);
  EXPECT_FLOAT_EQ(f[6], 12.0f);
  EXPECT_FLOAT_EQ(f[7], 13.0f);
}

TEST(SlangTranslator, PacksMat4AtAlignedOffset)
{
  // mat4 aligns to 16 and occupies 64 bytes; a leading float pads to offset 16.
  const std::vector<UboMember> members = {
      {"FrameCount", UboMemberType::Float},
      {"MVP", UboMemberType::Mat4},
  };
  const auto resolver = [](const std::string& name, float* out, int count) -> bool {
    if (name == "MVP")
    {
      for (int i = 0; i < count; ++i)
        out[i] = static_cast<float>(i);
      return true;
    }
    return false;  // FrameCount zero-filled
  };
  const std::vector<u8> data = PackSlangUniforms(members, resolver);
  ASSERT_GE(data.size(), 80u);  // 16 (float+pad) + 64 (mat4)
  const float* f = reinterpret_cast<const float*>(data.data());
  EXPECT_FLOAT_EQ(f[4], 0.0f);   // MVP[0] at offset 16
  EXPECT_FLOAT_EQ(f[19], 15.0f);  // MVP[15]
}

// Helper: a shader referencing Source plus `extra` additional distinct samplers.
static SlangShaderSource MakeShaderWithSamplers(int extra)
{
  SlangShaderSource shader = MakeShader("");
  for (int i = 0; i < extra; ++i)
  {
    const std::string name = "LUT" + std::to_string(i);
    shader.fragment_source +=
        "layout(binding = " + std::to_string(i + 2) + ") uniform sampler2D " + name + ";\n";
  }
  return shader;
}

TEST(SlangTranslator, AcceptsNineSamplers)
{
  // Full crt-royale's mask-apply pass needs 9 samplers (Source + 8). This is within the raised
  // 16-sampler ceiling (Dolphin utility descriptor set), so it must translate successfully.
  const auto result = TranslateSlangPass(MakeShaderWithSamplers(8), {}, {}, /*flip_clip_y=*/false);
  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.sampler_names.size(), 9u);
}

TEST(SlangTranslator, RejectsMoreThanSixteenSamplers)
{
  // Source + 16 = 17 distinct samplers -> over the 16 limit.
  const auto result = TranslateSlangPass(MakeShaderWithSamplers(16), {}, {}, /*flip_clip_y=*/false);
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.error.empty());
}

TEST(SlangTranslator, ClipYFlipMatchesFramebufferShaderGen)
{
  // Same rule as FramebufferShaderGen::GenerateScreenQuadVertexShader.
  EXPECT_TRUE(SlangNeedsClipYFlip(APIType::Vulkan));
  EXPECT_TRUE(SlangNeedsClipYFlip(APIType::OpenGL));
  EXPECT_FALSE(SlangNeedsClipYFlip(APIType::D3D));
  EXPECT_FALSE(SlangNeedsClipYFlip(APIType::Metal));
}

TEST(SlangTranslator, EmitsClipYFlipWhenRequested)
{
  const auto result = TranslateSlangPass(MakeShader(""), {}, {}, /*flip_clip_y=*/true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_NE(result.vertex_glsl.find("Position.y = -Position.y;"), std::string::npos);
  // The flip is now decided at translate time, not by a backend shader macro.
  EXPECT_EQ(result.vertex_glsl.find("API_VULKAN"), std::string::npos);
}

TEST(SlangTranslator, OmitsClipYFlipWhenNotRequested)
{
  const auto result = TranslateSlangPass(MakeShader(""), {}, {}, /*flip_clip_y=*/false);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.vertex_glsl.find("Position.y = -Position.y;"), std::string::npos);
  EXPECT_EQ(result.vertex_glsl.find("API_VULKAN"), std::string::npos);
}
