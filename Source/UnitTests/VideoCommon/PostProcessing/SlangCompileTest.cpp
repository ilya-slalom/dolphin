// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// End-to-end translator oracle: run a real RetroArch-slang-shaped shader through the parser +
// translator, then compile the result with glslang (the exact SPIRV::Compile* path the Vulkan
// backend uses). This catches ABI mismatches (uniform blocks, vertex attributes, macro
// collisions) that a text-only test cannot.

#include <sstream>
#include <string>

#include <cstdio>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

#include "VideoCommon/PostProcessing/SlangPreset.h"
#include "VideoCommon/PostProcessing/SlangShader.h"
#include "VideoCommon/PostProcessing/SlangTranslator.h"
#include "VideoCommon/Spirv.h"

using namespace VideoCommon;

namespace
{
// The Vulkan backend prepends this header (mirrors VideoBackends/Vulkan/ShaderCompiler.cpp's
// SHADER_HEADER) before calling SPIRV::Compile*; g_gfx->CreateShaderFromSource does this at
// runtime. The oracle must replicate it to faithfully compile what the backend would.
constexpr const char* VULKAN_SHADER_HEADER = R"(
  #version 450 core
  #extension GL_ARB_shading_language_include : enable
  #define ATTRIBUTE_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
  #define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(set = 1, binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(set = 1, binding = (x + 16))
  #define SSBO_BINDING(x) layout(std430, set = 2, binding = x)
  #define INPUT_ATTACHMENT_BINDING(x, y, z) layout(set = x, binding = y, input_attachment_index = z)
  #define VARYING_LOCATION(x) layout(location = x)
  #define FORCE_EARLY_Z layout(early_fragment_tests) in
  #define FB_FETCH_VALUE subpassLoad(in_ocol0)
  #define API_VULKAN 1
  #define float2 vec2
  #define float3 vec3
  #define float4 vec4
  #define uint2 uvec2
  #define uint3 uvec3
  #define uint4 uvec4
  #define int2 ivec2
  #define int3 ivec3
  #define int4 ivec4
  #define frac fract
  #define lerp mix
  #define gl_VertexID gl_VertexIndex
  #define gl_InstanceID gl_InstanceIndex
)";

// Compiles both stages of a translated pass with glslang; returns true only if both succeed.
bool CompilesOnVulkan(const TranslatedPass& pass, std::string* which_failed)
{
  const auto lang = glslang::EShTargetSpv_1_0;
  const std::string vs_src = std::string(VULKAN_SHADER_HEADER) + "\n" + pass.vertex_glsl;
  const std::string fs_src = std::string(VULKAN_SHADER_HEADER) + "\n" + pass.fragment_glsl;
  const auto vs = SPIRV::CompileVertexShader(vs_src, APIType::Vulkan, lang, nullptr);
  if (!vs)
  {
    *which_failed = "vertex";
    return false;
  }
  const auto fs = SPIRV::CompileFragmentShader(fs_src, APIType::Vulkan, lang, nullptr);
  if (!fs)
  {
    *which_failed = "fragment";
    return false;
  }
  return true;
}
}  // namespace

// The canonical RetroArch "stock" passthrough shader: dual uniform blocks (push_constant Push
// {} params + std140 UBO {} global), vertex attributes Position/TexCoord, and global.MVP.
TEST(SlangCompile, StockShaderCompilesOnVulkan)
{
  const std::string text =
      "#version 450\n"
      "layout(push_constant) uniform Push\n"
      "{\n"
      "    vec4 SourceSize;\n"
      "    vec4 OriginalSize;\n"
      "    vec4 OutputSize;\n"
      "    uint FrameCount;\n"
      "} params;\n"
      "layout(std140, set = 0, binding = 0) uniform UBO\n"
      "{\n"
      "    mat4 MVP;\n"
      "} global;\n"
      "#pragma stage vertex\n"
      "layout(location = 0) in vec4 Position;\n"
      "layout(location = 1) in vec2 TexCoord;\n"
      "layout(location = 0) out vec2 vTexCoord;\n"
      "void main()\n"
      "{\n"
      "   gl_Position = global.MVP * Position;\n"
      "   vTexCoord = TexCoord;\n"
      "}\n"
      "#pragma stage fragment\n"
      "layout(location = 0) in vec2 vTexCoord;\n"
      "layout(location = 0) out vec4 FragColor;\n"
      "layout(set = 0, binding = 2) uniform sampler2D Source;\n"
      "void main()\n"
      "{\n"
      "    FragColor = vec4(texture(Source, vTexCoord).rgb, 1.0);\n"
      "}\n";

  std::string error;
  const auto parsed = ParseSlangShader(text, &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  const auto translated = TranslateSlangPass(*parsed, {}, {});
  ASSERT_TRUE(translated.ok) << translated.error;

  std::string which;
  EXPECT_TRUE(CompilesOnVulkan(translated, &which))
      << which << " stage failed to compile:\nVS:\n"
      << translated.vertex_glsl << "\nFS:\n"
      << translated.fragment_glsl;
}

// The HLSL-compat macros RetroArch shaders pull in (lerp/frac/mul/float2...) collide with
// Dolphin's own backend-prepended macros; the translation must compile regardless.
TEST(SlangCompile, CompatMacrosCompileOnVulkan)
{
  const std::string text =
      "#version 450\n"
      "layout(push_constant) uniform Push { vec4 SourceSize; } params;\n"
      "layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;\n"
      "#define lerp(a,b,c) mix(a,b,c)\n"
      "#define frac(x) (fract(x))\n"
      "#define mul(a,b) (b*a)\n"
      "#define float4 vec4\n"
      "#pragma stage vertex\n"
      "layout(location = 0) in vec4 Position;\n"
      "void main() { gl_Position = global.MVP * Position; }\n"
      "#pragma stage fragment\n"
      "layout(location = 0) out vec4 FragColor;\n"
      "layout(set = 0, binding = 2) uniform sampler2D Source;\n"
      "void main() { FragColor = lerp(float4(0.0), texture(Source, vec2(0.5)), frac(0.5)); }\n";

  std::string error;
  const auto parsed = ParseSlangShader(text, &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  const auto translated = TranslateSlangPass(*parsed, {}, {});
  ASSERT_TRUE(translated.ok) << translated.error;

  std::string which;
  EXPECT_TRUE(CompilesOnVulkan(translated, &which))
      << which << " stage failed:\nVS:\n"
      << translated.vertex_glsl << "\nFS:\n"
      << translated.fragment_glsl;
}

namespace
{
std::string ReadFile(const std::string& path)
{
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
std::string DirName(const std::string& p)
{
  const auto s = p.find_last_of('/');
  return s == std::string::npos ? "" : p.substr(0, s);
}
}  // namespace

// Compiles every pass of a REAL preset through the full pipeline (parse preset -> resolve pass
// paths -> expand #includes -> translate -> glslang). Guarded by SLANG_PRESET env var so it only
// runs when pointed at a real preset tree (e.g. the crt-royale pack pulled from the buildbot):
//   SLANG_PRESET=/path/to/crt/crt-royale.slangp ./Tests --gtest_filter=SlangCompile.RealPreset
TEST(SlangCompile, RealPresetCompilesAllPasses)
{
  const char* preset_path = std::getenv("SLANG_PRESET");
  if (preset_path == nullptr)
    GTEST_SKIP() << "set SLANG_PRESET=/path/to/foo.slangp to run";

  const std::string text = ReadFile(preset_path);
  ASSERT_FALSE(text.empty()) << "cannot read " << preset_path;
  std::string error;
  const auto preset = ParseSlangPreset(text, DirName(preset_path), &error);
  ASSERT_TRUE(preset.has_value()) << error;

  const SlangFileReader reader = [](const std::string& p, std::string* out) {
    const std::string c = ReadFile(p);
    if (c.empty())
      return false;
    out->assign(c);
    return true;
  };

  std::vector<std::string> lut_names;
  for (const auto& lut : preset->luts)
    lut_names.push_back(lut.name);

  int ok = 0;
  std::vector<std::string> known_aliases;
  for (size_t i = 0; i < preset->passes.size(); ++i)
  {
    const auto& pass = preset->passes[i];
    std::string src = ReadFile(pass.shader_path);
    ASSERT_FALSE(src.empty()) << "pass " << i << " unreadable: " << pass.shader_path;
    src = ExpandSlangIncludes(src, DirName(pass.shader_path), reader);
    const auto parsed = ParseSlangShader(src, &error);
    ASSERT_TRUE(parsed.has_value()) << "pass " << i << " parse: " << error;
    const auto translated = TranslateSlangPass(*parsed, known_aliases, lut_names);
    ASSERT_TRUE(translated.ok) << "pass " << i << " translate: " << translated.error;

    std::string which;
    const bool compiled = CompilesOnVulkan(translated, &which);
    EXPECT_TRUE(compiled) << "pass " << i << " (" << pass.shader_path << ") " << which
                          << " stage failed to compile";
    if (compiled)
      ++ok;
    if (!pass.alias.empty())
      known_aliases.push_back(pass.alias);
  }
  std::printf("RealPreset: %d/%zu passes compiled\n", ok, preset->passes.size());
}
