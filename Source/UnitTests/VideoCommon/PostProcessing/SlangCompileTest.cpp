// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// End-to-end translator oracle: run a real RetroArch-slang-shaped shader through the parser +
// translator, then compile the result with glslang (the exact SPIRV::Compile* path the Vulkan
// backend uses). This catches ABI mismatches (uniform blocks, vertex attributes, macro
// collisions) that a text-only test cannot.

#include <array>
#include <sstream>
#include <string>

#include <cstdio>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

#include <spirv_hlsl.hpp>
#include <spirv_msl.hpp>

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

// D3D backend header, copied verbatim from VideoBackends/D3DCommon/Shader.cpp SHADER_HEADER.
constexpr const char* D3D_SHADER_HEADER = R"(
  // Target GLSL 4.5.
  #version 450 core

  #extension GL_ARB_shading_language_include : enable

  #define ATTRIBUTE_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
  #define UBO_BINDING(packing, x) layout(packing, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(binding = x)
  #define SSBO_BINDING(x) layout(binding = (x + 2))
  #define VARYING_LOCATION(x) layout(location = x)
  #define FORCE_EARLY_Z layout(early_fragment_tests) in

  // hlsl to glsl function translation
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

  #define API_D3D 1
)";

// Metal backend header, copied verbatim from VideoBackends/Metal/MTLUtil.mm SHADER_HEADER.
constexpr const char* METAL_SHADER_HEADER = R"(
// Target GLSL 4.5.
#version 450 core
// Always available on Metal
#extension GL_EXT_shader_8bit_storage : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require

#define ATTRIBUTE_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
#define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - 1))
#define SAMPLER_BINDING(x) layout(set = 1, binding = x)
#define TEXEL_BUFFER_BINDING(x) layout(set = 1, binding = (x + 8))
#define SSBO_BINDING(x) layout(std430, set = 2, binding = x)
#define INPUT_ATTACHMENT_BINDING(x, y, z) layout(set = x, binding = y, input_attachment_index = z)
#define VARYING_LOCATION(x) layout(location = x)
#define FORCE_EARLY_Z layout(early_fragment_tests) in

// Metal framebuffer fetch helpers.
#define FB_FETCH_VALUE subpassLoad(in_ocol0)

// hlsl to glsl function translation
#define API_METAL 1
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

// These were changed in Vulkan
#define gl_VertexID gl_VertexIndex
#define gl_InstanceID gl_InstanceIndex
)";

// OpenGL backend header — hand-maintained mirror of the modern-desktop variant assembled at
// ProgramShaderCache.cpp runtime. The real OGL backend leaves VARYING_LOCATION empty
// (ProgramShaderCache.cpp:790, with a TODO to define it if using bSupportsExplicitLayoutInShader)
// and matches varyings by name, because it hands GLSL straight to the driver. This test must
// define VARYING_LOCATION anyway: SPIRV::Compile* always targets SPIR-V (Spirv.cpp:179), and
// SPIR-V requires explicit locations on user varyings. Therefore this row does NOT prove
// name-based varying matching — that coverage is not available through this API and is a known
// gap. What it DOES prove: the translated GLSL compiles under APIType::OpenGL, i.e. without
// EShMsgVulkanRules and without the Vulkan header's gl_VertexID/gl_InstanceID aliases, so no
// Vulkan-only builtin or construct survives translation. That is real, distinct coverage. Note
// the real OGL backend never calls SPIRV::Compile* at all (no such call exists anywhere under
// Source/Core/VideoBackends/OGL/), so this row is a translate-and-compile proxy rather than
// driver acceptance.
constexpr const char* OGL_SHADER_HEADER = R"(
  #version 450 core

  #extension GL_ARB_explicit_attrib_location : enable
  #define ATTRIBUTE_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
  #define UBO_BINDING(packing, x) layout(packing, binding = x)
  #define SAMPLER_BINDING(x) layout(binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(binding = x)
  #define SSBO_BINDING(x) layout(std430, binding = x)
  #define IMAGE_BINDING(format, x) layout(format, binding = x)

  #define VARYING_LOCATION(x) layout(location = x)

  #define API_OPENGL 1
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
)";

struct BackendShaderHeader
{
  const char* name;
  APIType api_type;
  const char* header;
  glslang::EShTargetLanguageVersion spv_version;
};

constexpr BackendShaderHeader BACKEND_HEADERS[] = {
    {"Vulkan", APIType::Vulkan, VULKAN_SHADER_HEADER, glslang::EShTargetSpv_1_0},
    {"D3D", APIType::D3D, D3D_SHADER_HEADER, glslang::EShTargetSpv_1_0},
    {"Metal", APIType::Metal, METAL_SHADER_HEADER, glslang::EShTargetSpv_1_5},
    {"OpenGL", APIType::OpenGL, OGL_SHADER_HEADER, glslang::EShTargetSpv_1_0},
};

// Compiles both stages of a translated pass with one backend's GLSL preamble; returns true only if
// both succeed, and on failure *which_failed names the backend and stage.
bool CompilesOnBackend(const TranslatedPass& pass, const BackendShaderHeader& backend,
                       std::string* which_failed)
{
  const std::string vs_src = std::string(backend.header) + "\n" + pass.vertex_glsl;
  const std::string fs_src = std::string(backend.header) + "\n" + pass.fragment_glsl;
  const auto vs = SPIRV::CompileVertexShader(vs_src, backend.api_type, backend.spv_version, nullptr);
  if (!vs)
  {
    *which_failed = std::string(backend.name) + " vertex";
    return false;
  }
  const auto fs = SPIRV::CompileFragmentShader(fs_src, backend.api_type, backend.spv_version, nullptr);
  if (!fs)
  {
    *which_failed = std::string(backend.name) + " fragment";
    return false;
  }
  return true;
}

// Cross-compiles fragment SPIR-V exactly the way D3DCommon/Shader.cpp GetHLSLFromSPIRV does for
// feature level 11 (shader_model = 50).
std::string HlslFromSpirv(const SPIRV::CodeVector& spv)
{
  spirv_cross::CompilerHLSL::Options options;
  options.shader_model = 50;
  spirv_cross::CompilerHLSL compiler(spv);
  compiler.set_hlsl_options(options);
  return compiler.compile();
}

// ... and the way Metal/MTLUtil.mm does on macOS (MSL 2.3, framebuffer-fetch subpasses).
std::string MslFromSpirv(const SPIRV::CodeVector& spv)
{
  spirv_cross::CompilerMSL::Options options;
  options.platform = spirv_cross::CompilerMSL::Options::macOS;
  options.set_msl_version(2, 3);
  options.use_framebuffer_fetch_subpasses = true;
  spirv_cross::CompilerMSL compiler(spv);
  compiler.set_msl_options(options);
  return compiler.compile();
}

const BackendShaderHeader& BackendNamed(const char* name)
{
  for (const BackendShaderHeader& backend : BACKEND_HEADERS)
  {
    if (std::string_view(backend.name) == name)
      return backend;
  }
  ADD_FAILURE() << "no backend named " << name;
  return BACKEND_HEADERS[0];
}
}  // namespace

// The canonical RetroArch "stock" passthrough shader: dual uniform blocks (push_constant Push
// {} params + std140 UBO {} global), vertex attributes Position/TexCoord, and global.MVP.
TEST(SlangCompile, StockShaderCompilesOnAllBackends)
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

  for (const BackendShaderHeader& backend : BACKEND_HEADERS)
  {
    SCOPED_TRACE(backend.name);
    const auto translated =
        TranslateSlangPass(*parsed, {}, {}, SlangNeedsClipYFlip(backend.api_type));
    ASSERT_TRUE(translated.ok) << translated.error;

    std::string which;
    EXPECT_TRUE(CompilesOnBackend(translated, backend, &which))
        << which << " stage failed to compile:\nVS:\n"
        << translated.vertex_glsl << "\nFS:\n"
        << translated.fragment_glsl;
  }
}

// The HLSL-compat macros RetroArch shaders pull in (lerp/frac/mul/float2...) collide with
// Dolphin's own backend-prepended macros; the translation must compile regardless.
TEST(SlangCompile, CompatMacrosCompileOnAllBackends)
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

  for (const BackendShaderHeader& backend : BACKEND_HEADERS)
  {
    SCOPED_TRACE(backend.name);
    const auto translated =
        TranslateSlangPass(*parsed, {}, {}, SlangNeedsClipYFlip(backend.api_type));
    ASSERT_TRUE(translated.ok) << translated.error;

    std::string which;
    EXPECT_TRUE(CompilesOnBackend(translated, backend, &which))
        << which << " stage failed:\nVS:\n"
        << translated.vertex_glsl << "\nFS:\n"
        << translated.fragment_glsl;
  }
}

namespace
{
// A pass shaped like the ones that actually break: the sampler is handed to a user function
// (crt-royale's `tex2D_linearize(sampler2D tex, vec2 coords)`) and reached through a macro, so no
// name-based rewrite of the call sites could ever see it. Also exercises every sampling entry
// point the real libretro pack uses on a 2D sampler.
constexpr const char* SAMPLER_SHADER = R"(#version 450
layout(push_constant) uniform Push { vec4 SourceSize; } params;
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() { gl_Position = global.MVP * Position; vTexCoord = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
#define SAMPLE(t, c) texture(t, c)
vec4 tex2D_linearize(sampler2D tex, vec2 coords)
{
  return SAMPLE(tex, coords) + texelFetch(tex, ivec2(coords), 0) +
         textureLod(tex, coords, 1.0) + textureOffset(tex, coords, ivec2(1, 0)) +
         textureLodOffset(tex, coords, 1.0, ivec2(1, 0)) +
         texelFetchOffset(tex, ivec2(coords), 0, ivec2(1, 0)) +
         textureGrad(tex, coords, vec2(0.0), vec2(0.0)) + textureGather(tex, coords) +
         textureGather(tex, coords, 1) + texture(tex, coords, 0.5) +
         vec4(vec2(textureSize(tex, 0)), 0.0, 0.0);
}
void main() { FragColor = tex2D_linearize(Source, vTexCoord); }
)";

TranslatedPass TranslateForBackend(const char* shader_text, const BackendShaderHeader& backend)
{
  std::string error;
  const auto parsed = ParseSlangShader(shader_text, &error);
  EXPECT_TRUE(parsed.has_value()) << error;
  if (!parsed.has_value())
    return {};
  return TranslateSlangPass(*parsed, {}, {}, SlangNeedsClipYFlip(backend.api_type));
}
}  // namespace

// H1. Every texture the slang chain binds is allocated as SLANG_INPUT_TEXTURE_TYPE, which is
// Texture_2DArray: MultipassPostProcessing's pass outputs and feedback buffers, the history
// textures that clone the XFB's config, the XFB itself, and the LUTs. A `sampler2D` declared
// against one of those samples the texture unit's *2D* binding, which nothing ever sets -- on
// desktop OpenGL that is object 0, i.e. black, returned with GL_NO_ERROR (measured on GL 4.6 /
// NVIDIA 596.49). glslang accepts `sampler2D` happily, so this can only be caught by asserting on
// the generated declaration.
TEST(SlangCompile, SamplersAreDeclaredAsArrays)
{
  const std::string expected =
      "uniform " + std::string(SlangSamplerGlslType(SLANG_INPUT_TEXTURE_TYPE)) + " Source;";
  for (const BackendShaderHeader& backend : BACKEND_HEADERS)
  {
    SCOPED_TRACE(backend.name);
    const auto translated = TranslateForBackend(SAMPLER_SHADER, backend);
    ASSERT_TRUE(translated.ok) << translated.error;
    EXPECT_NE(translated.fragment_glsl.find(expected), std::string::npos)
        << "expected `" << expected << "` in:\n"
        << translated.fragment_glsl;
    EXPECT_EQ(translated.fragment_glsl.find("uniform sampler2D Source;"), std::string::npos)
        << "sampler2D declared against a Texture_2DArray binding";
  }
}

// The real oracle: assert what reaches the driver, not what the translator wrote. This is the
// assertion the previous oracle was missing -- it stopped at SPIRV::Compile*, and `sampler2D` is
// valid GLSL, so all four rows passed a chain that rendered black.
TEST(SlangCompile, CrossCompiledSamplersAreArrayTextures)
{
  {
    const BackendShaderHeader& d3d = BackendNamed("D3D");
    const auto translated = TranslateForBackend(SAMPLER_SHADER, d3d);
    ASSERT_TRUE(translated.ok) << translated.error;
    const auto spv = SPIRV::CompileFragmentShader(std::string(d3d.header) + "\n" +
                                                      translated.fragment_glsl,
                                                  d3d.api_type, d3d.spv_version, nullptr);
    ASSERT_TRUE(spv.has_value()) << translated.fragment_glsl;
    const std::string hlsl = HlslFromSpirv(*spv);
    EXPECT_NE(hlsl.find("Texture2DArray<float4> Source"), std::string::npos) << hlsl;
    EXPECT_EQ(hlsl.find("Texture2D<float4> Source"), std::string::npos) << hlsl;
  }
  {
    const BackendShaderHeader& metal = BackendNamed("Metal");
    const auto translated = TranslateForBackend(SAMPLER_SHADER, metal);
    ASSERT_TRUE(translated.ok) << translated.error;
    const auto spv = SPIRV::CompileFragmentShader(std::string(metal.header) + "\n" +
                                                      translated.fragment_glsl,
                                                  metal.api_type, metal.spv_version, nullptr);
    ASSERT_TRUE(spv.has_value()) << translated.fragment_glsl;
    const std::string msl = MslFromSpirv(*spv);
    EXPECT_NE(msl.find("texture2d_array<float> Source"), std::string::npos) << msl;
    EXPECT_EQ(msl.find("texture2d<float> Source"), std::string::npos) << msl;
  }
}

// The array declaration is only useful if the 2-coordinate call sites still compile. They are
// reached through a macro and through a user function's sampler parameter, so the shim overloads
// -- not a call-site rewrite -- are what has to carry them.
TEST(SlangCompile, TwoCoordinateCallSitesCompileOnAllBackends)
{
  for (const BackendShaderHeader& backend : BACKEND_HEADERS)
  {
    SCOPED_TRACE(backend.name);
    const auto translated = TranslateForBackend(SAMPLER_SHADER, backend);
    ASSERT_TRUE(translated.ok) << translated.error;
    std::string which;
    EXPECT_TRUE(CompilesOnBackend(translated, backend, &which))
        << which << " stage failed:\nVS:\n"
        << translated.vertex_glsl << "\nFS:\n"
        << translated.fragment_glsl;
  }
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
  const auto s = p.find_last_of("/\\");
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

  std::array<int, std::size(BACKEND_HEADERS)> ok{};
  std::vector<std::string> known_aliases;
  for (size_t i = 0; i < preset->passes.size(); ++i)
  {
    const auto& pass = preset->passes[i];
    std::string src = ReadFile(pass.shader_path);
    ASSERT_FALSE(src.empty()) << "pass " << i << " unreadable: " << pass.shader_path;
    src = ExpandSlangIncludes(src, DirName(pass.shader_path), reader);
    const auto parsed = ParseSlangShader(src, &error);
    ASSERT_TRUE(parsed.has_value()) << "pass " << i << " parse: " << error;

    for (size_t b = 0; b < std::size(BACKEND_HEADERS); ++b)
    {
      const BackendShaderHeader& backend = BACKEND_HEADERS[b];
      SCOPED_TRACE(backend.name);
      const auto translated =
          TranslateSlangPass(*parsed, known_aliases, lut_names, SlangNeedsClipYFlip(backend.api_type));
      ASSERT_TRUE(translated.ok) << "pass " << i << " translate: " << translated.error;

      std::string which;
      const bool compiled = CompilesOnBackend(translated, backend, &which);
      EXPECT_TRUE(compiled) << "pass " << i << " (" << pass.shader_path << ") " << which
                            << " stage failed to compile";
      if (compiled)
        ++ok[b];
    }
    if (!pass.alias.empty())
      known_aliases.push_back(pass.alias);
  }
  for (size_t b = 0; b < std::size(BACKEND_HEADERS); ++b)
  {
    std::printf("RealPreset[%s]: %d/%zu passes compiled\n", BACKEND_HEADERS[b].name, ok[b],
                preset->passes.size());
  }
}
