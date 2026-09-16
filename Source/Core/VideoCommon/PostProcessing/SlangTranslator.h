// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/PostProcessing/SlangShader.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoCommon.h"

class AbstractShader;

namespace VideoCommon
{
// Every texture a translated slang pass can sample is allocated with this type: the pass outputs
// and feedback buffers (MultipassPostProcessing::ConfigureChain), the Original history textures
// (which clone the XFB's config), the XFB itself (TextureCacheBase, layered for stereo), and the
// LUTs (LutTexture). Backends derive the view/target from TextureConfig::type alone, never from the
// layer count, so a pass must declare its samplers to match or the binding is silently wrong: on
// desktop OpenGL a sampler2D reads the texture unit's GL_TEXTURE_2D binding, which nothing sets for
// an array texture, and returns black with GL_NO_ERROR.
//
// Only three of those five sites can name this constant. The XFB's own type is not ours to pick --
// it is an array because stereo renders one eye per layer, a decision that predates and outranks
// post-processing -- and the history textures must copy the XFB's config verbatim, because
// ShiftHistory moves frames with CopyRectangleFromTexture. Writing SLANG_INPUT_TEXTURE_TYPE into
// either would invert the dependency and read as though the chain dictated the frame format. So the
// direction is kept honest by checking instead of imposing -- at run time only. There is nothing
// here for a static_assert to catch: the XFB's type is an argument at its allocation site
// (TextureCacheBase spells Texture_2DArray out by hand rather than leaning on the TextureConfig
// default), and the history textures copy it from the texture they are handed, so both are values
// no translation unit can see. The live checks are in MultipassPostProcessing: BlitFromTexture
// asserts the incoming type in the rebuild branch, which every preset with a translated chain
// passes through on its first frame and on every geometry change, and repeats the test per frame as
// a debug-only assert to cover a type that changes with the geometry unmoved; EnsureHistoryTextures
// asserts the type it is about to clone, and is only reached by presets that ask for
// OriginalHistoryN with N >= 1. The passthrough path returns before all three, so it is covered by
// nothing here -- its own shader spells sampler2DArray out, and that is the only thing holding it.
constexpr AbstractTextureType SLANG_INPUT_TEXTURE_TYPE = AbstractTextureType::Texture_2DArray;

// The GLSL sampler type that matches an AbstractTextureType binding.
constexpr std::string_view SlangSamplerGlslType(AbstractTextureType type)
{
  return type == AbstractTextureType::Texture_2DArray ? "sampler2DArray" : "sampler2D";
}

// The set of texture samplers a pass reads, in binding order (index 0..N-1).
// Includes "Source", "Original", each referenced alias, and each referenced LUT.
// GLSL scalar/vector/matrix category of a UBO member, used for std140 packing by the executor.
enum class UboMemberType
{
  Float,   // float / int / uint (4 bytes, 4-byte aligned)
  Vec2,    // 8 bytes, 8-byte aligned
  Vec3,    // 12 bytes, 16-byte aligned
  Vec4,    // 16 bytes, 16-byte aligned
  Mat4,    // 64 bytes, 16-byte aligned
  Unknown  // unrecognized; treated as vec4 to stay conservative
};

struct UboMember
{
  std::string name;
  UboMemberType type = UboMemberType::Unknown;
};

struct TranslatedPass
{
  std::string vertex_glsl;    // ready for g_gfx->CreateShaderFromSource(Vertex, ...)
  std::string fragment_glsl;  // ready for CreateShaderFromSource(Pixel, ...)
  std::vector<std::string> sampler_names;  // binding index -> semantic/alias/LUT name
  // The merged PSBlock members in declaration order (params block, then global block). The
  // executor packs the uniform buffer to match this std140 layout.
  std::vector<UboMember> ubo_members;
  bool ok = false;
  std::string error;  // set when ok == false (e.g. more samplers than MAX_SAMPLERS, which is 16)
};

// True when the injected fullscreen-triangle vertex shader must negate clip-space Y. NDC Y is
// flipped in Vulkan; we also flip on OpenGL so that (0,0) is the lower-left. Mirrors
// FramebufferShaderGen::GenerateScreenQuadVertexShader -- keep the two in sync.
constexpr bool SlangNeedsClipYFlip(APIType api_type)
{
  return api_type == APIType::Vulkan || api_type == APIType::OpenGL;
}
static_assert(SlangNeedsClipYFlip(APIType::Vulkan));
static_assert(SlangNeedsClipYFlip(APIType::OpenGL));
static_assert(!SlangNeedsClipYFlip(APIType::D3D));
static_assert(!SlangNeedsClipYFlip(APIType::Metal));

// known_aliases: names produced by earlier passes; lut_names: declared LUTs.
// flip_clip_y: see SlangNeedsClipYFlip. Callers pass SlangNeedsClipYFlip(g_backend_info.api_type).
TranslatedPass TranslateSlangPass(const SlangShaderSource& shader,
                                  const std::vector<std::string>& known_aliases,
                                  const std::vector<std::string>& lut_names, bool flip_clip_y);

// Packs a std140 uniform buffer matching `members` (in declaration order). For each member,
// `resolver(name, out)` fills `out` with the member's component values (1..16 floats); if it
// returns false the member is zero-filled. Applies std140 alignment rules for the supported
// member types (Float/Vec2/Vec3/Vec4/Mat4). Pure/testable.
using UniformResolver = std::function<bool(const std::string& name, float* out, int count)>;
std::vector<u8> PackSlangUniforms(const std::vector<UboMember>& members,
                                  const UniformResolver& resolver);

struct CompiledPassShaders
{
  std::unique_ptr<AbstractShader> vertex;
  std::unique_ptr<AbstractShader> pixel;
};

// Compiles a translated pass. include_dir roots the #include resolver (the directory of the
// .slang file); the Sys shaders dir is added as the system include root. Returns empty uniques
// on failure.
CompiledPassShaders CompileTranslatedPass(const TranslatedPass& pass,
                                          const std::string& include_dir);
}  // namespace VideoCommon
