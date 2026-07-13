// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/MultipassPostProcessing.h"

#include <array>
#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/Config/GraphicsSettings.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/PostProcessing/LutTexture.h"
#include "VideoCommon/PostProcessing/PassSizing.h"
#include "VideoCommon/PostProcessing/SlangPreset.h"
#include "VideoCommon/PostProcessing/SlangSamplers.h"
#include "VideoCommon/PostProcessing/SlangShader.h"
#include "VideoCommon/PostProcessing/SlangTranslator.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon
{
namespace
{
constexpr AbstractTextureFormat INTERMEDIATE_FORMAT = AbstractTextureFormat::RGBA16F;

std::string DirectoryOf(const std::string& path)
{
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Identity MVP: the shader multiplies it by the synthesized fullscreen-triangle Position.
std::array<float, 16> IdentityMvp()
{
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
}  // namespace

MultipassPostProcessing::MultipassPostProcessing() = default;
MultipassPostProcessing::~MultipassPostProcessing() = default;

std::string PresetNameFromPath(const std::string& full_path,
                               const std::vector<std::string>& roots)
{
  std::string path = full_path;
  std::replace(path.begin(), path.end(), '\\', '/');

  std::string best;  // longest matching root's relative remainder
  for (const std::string& raw_root : roots)
  {
    std::string root = raw_root;
    std::replace(root.begin(), root.end(), '\\', '/');
    if (!root.empty() && root.back() != '/')
      root += '/';
    if (path.size() > root.size() && path.compare(0, root.size(), root) == 0)
    {
      const std::string remainder = path.substr(root.size());
      if (best.empty() || remainder.size() < best.size())
        best = remainder;
    }
  }

  std::string relative = best.empty() ? path : best;

  // Strip a leading slash and the ".slangp" extension.
  if (best.empty())
  {
    // No root matched: fall back to the bare filename.
    const auto slash = relative.find_last_of('/');
    if (slash != std::string::npos)
      relative = relative.substr(slash + 1);
  }
  constexpr std::string_view kExt = ".slangp";
  if (relative.size() >= kExt.size() &&
      relative.compare(relative.size() - kExt.size(), kExt.size(), kExt) == 0)
  {
    relative = relative.substr(0, relative.size() - kExt.size());
  }
  return relative;
}

std::vector<std::string> MultipassPostProcessing::GetPresetList()
{
  const std::string user_dir = File::GetUserPath(D_SHADERS_IDX);
  const std::string sys_dir = File::GetSysDirectory() + SHADERS_DIR DIR_SEP;
  const std::array<std::string_view, 2> dirs = {user_dir, sys_dir};
  const std::array<std::string_view, 1> exts = {".slangp"};
  const std::vector<std::string> paths = Common::DoFileSearch(dirs, exts, /*recursive=*/true);

  const std::vector<std::string> roots = {user_dir, sys_dir};
  std::vector<std::string> result;
  result.reserve(paths.size());
  for (const std::string& path : paths)
    result.push_back(PresetNameFromPath(path, roots));
  return result;
}

bool MultipassPostProcessing::Initialize(AbstractTextureFormat format)
{
  m_framebuffer_format = format;
  LoadPreset(Config::Get(Config::GFX_ENHANCE_POST_SHADER));
  if (!m_passthrough)
    RecompilePipeline();
  // A post-processing preset failing to load must never block game boot: on any failure
  // LoadPreset falls back to pass-through (and has already surfaced a panic). Always report
  // success so Presenter::Initialize continues.
  return true;
}

void MultipassPostProcessing::RecompileShader()
{
  ClearChain();
  LoadPreset(Config::Get(Config::GFX_ENHANCE_POST_SHADER));
  if (!m_passthrough)
    RecompilePipeline();
}

void MultipassPostProcessing::ClearChain()
{
  m_passes.clear();
  m_luts.clear();
  m_passthrough = true;
}

void MultipassPostProcessing::LoadPreset(const std::string& preset_spec)
{
  ClearChain();

  // The config value may be a single preset name or a ';'-separated chain of presets whose pass
  // graphs are concatenated (each preset's first pass samples the previous preset's output as
  // "Source"). A plain name has no ';', so this is backwards compatible.
  std::vector<std::string> names;
  size_t start = 0;
  while (start <= preset_spec.size())
  {
    const auto sep = preset_spec.find(';', start);
    const auto end = sep == std::string::npos ? preset_spec.size() : sep;
    std::string name = preset_spec.substr(start, end - start);
    // Trim surrounding whitespace.
    const auto first = name.find_first_not_of(" \t");
    const auto last = name.find_last_not_of(" \t");
    if (first != std::string::npos)
      names.push_back(name.substr(first, last - first + 1));
    if (sep == std::string::npos)
      break;
    start = end + 1;
  }

  for (const std::string& name : names)
    AppendPreset(name);

  m_passthrough = m_passes.empty();
}

void MultipassPostProcessing::AppendPreset(const std::string& preset_name)
{
  if (preset_name.empty())
    return;

  // Resolve preset path: user Shaders dir first, then Sys.
  std::string path = File::GetUserPath(D_SHADERS_IDX) + preset_name + ".slangp";
  if (!File::Exists(path))
    path = File::GetSysDirectory() + SHADERS_DIR DIR_SEP + preset_name + ".slangp";
  if (!File::Exists(path))
    return;

  std::string text;
  if (!File::ReadFileToString(path, text))
    return;

  const std::string base_dir = DirectoryOf(path);
  std::string error;
  const auto config = ParseSlangPreset(text, base_dir, &error);
  if (!config)
  {
    ERROR_LOG_FMT(VIDEO, "Post-processing: failed to load preset {}: {}", preset_name, error);
    return;
  }

  // Load LUTs.
  std::vector<std::string> lut_names;
  for (const SlangLutConfig& lut_config : config->luts)
  {
    Lut lut;
    lut.name = lut_config.name;
    lut.texture = LoadLutTexture(lut_config);
    lut.sampler =
        MakeSlangSamplerState(lut_config.wrap_mode, lut_config.linear, lut_config.mipmap);
    lut_names.push_back(lut.name);
    m_luts.push_back(std::move(lut));
  }

  // On any failure, roll back just this preset's passes/LUTs so a bad preset later in a chain
  // doesn't discard earlier ones that loaded fine.
  const size_t passes_before = m_passes.size();
  const size_t luts_before = m_luts.size() - config->luts.size();
  const auto rollback = [&] {
    m_passes.resize(passes_before);
    m_luts.resize(luts_before);
  };

  // Build passes, accumulating known aliases as we go.
  std::vector<std::string> known_aliases;
  for (const SlangPassConfig& pass_config : config->passes)
  {
    std::string shader_text;
    if (!File::ReadFileToString(pass_config.shader_path, shader_text))
    {
      ERROR_LOG_FMT(VIDEO, "Post-processing: failed to read slang shader {}; skipping preset {}",
                    pass_config.shader_path, preset_name);
      rollback();
      return;
    }

    // Expand #includes before stage-splitting: some crt-royale passes keep their
    // #pragma stage bodies in an included header.
    const SlangFileReader reader = [](const std::string& p, std::string* out) {
      return File::ReadFileToString(p, *out);
    };
    shader_text = ExpandSlangIncludes(shader_text, DirectoryOf(pass_config.shader_path), reader);

    const auto parsed = ParseSlangShader(shader_text, &error);
    if (!parsed)
    {
      ERROR_LOG_FMT(VIDEO, "Post-processing: failed to parse {}: {}; skipping preset {}",
                    pass_config.shader_path, error, preset_name);
      rollback();
      return;
    }

    TranslatedPass translated = TranslateSlangPass(*parsed, known_aliases, lut_names);
    if (!translated.ok)
    {
      ERROR_LOG_FMT(VIDEO, "Post-processing: cannot translate {}: {}; skipping preset {}",
                    pass_config.shader_path, translated.error, preset_name);
      rollback();
      return;
    }

    CompiledPassShaders shaders =
        CompileTranslatedPass(translated, DirectoryOf(pass_config.shader_path));
    if (!shaders.vertex || !shaders.pixel)
    {
      ERROR_LOG_FMT(VIDEO, "Post-processing: failed to compile {}; skipping preset {}",
                    pass_config.shader_path, preset_name);
      rollback();
      return;
    }

    Pass pass;
    pass.config = pass_config;
    pass.alias = pass_config.alias;
    pass.sampler_names = std::move(translated.sampler_names);
    pass.ubo_members = std::move(translated.ubo_members);
    pass.parameters = parsed->parameters;
    pass.input_sampler =
        MakeSlangSamplerState(pass_config.wrap_mode, pass_config.filter_linear,
                              pass_config.mipmap_input);
    pass.vertex_shader = std::move(shaders.vertex);
    pass.pixel_shader = std::move(shaders.pixel);
    m_passes.push_back(std::move(pass));

    if (!pass_config.alias.empty())
      known_aliases.push_back(pass_config.alias);
  }
}

void MultipassPostProcessing::RecompilePipeline()
{
  if (m_passthrough || m_framebuffer_format == AbstractTextureFormat::Undefined)
    return;

  const u32 viewport_width = std::max<u32>(1, m_target_width);
  const u32 viewport_height = std::max<u32>(1, m_target_height);
  const u32 source_width = std::max<u32>(1, m_source_width);
  const u32 source_height = std::max<u32>(1, m_source_height);

  // Size every pass's RT from the game's real source resolution (seed) chained through the
  // preset's scale rules. Seeding from the source -- not the viewport -- is what makes
  // SourceSize/scanline geometry correct.
  std::vector<SlangPassConfig> configs;
  configs.reserve(m_passes.size());
  for (const Pass& pass : m_passes)
    configs.push_back(pass.config);
  const std::vector<PassSize> sizes =
      ComputePassChainSizes(configs, source_width, source_height, viewport_width, viewport_height);

  const size_t pass_count = m_passes.size();
  for (size_t i = 0; i < pass_count; ++i)
  {
    Pass& pass = m_passes[i];
    const bool is_final = i == pass_count - 1;
    const AbstractTextureFormat output_format =
        is_final ? m_framebuffer_format : INTERMEDIATE_FORMAT;

    if (!is_final)
    {
      const u32 out_w = sizes[i].width;
      const u32 out_h = sizes[i].height;

      const TextureConfig texture_config(out_w, out_h, 1, 1, 1, INTERMEDIATE_FORMAT,
                                         AbstractTextureFlag_RenderTarget,
                                         AbstractTextureType::Texture_2DArray);
      pass.output_texture =
          g_gfx->CreateTexture(texture_config, "slang pass " + std::to_string(i));
      pass.output_framebuffer =
          pass.output_texture ? g_gfx->CreateFramebuffer(pass.output_texture.get(), nullptr)
                              : nullptr;
    }
    else
    {
      pass.output_texture.reset();
      pass.output_framebuffer.reset();
    }

    AbstractPipelineConfig pipeline_config = {};
    pipeline_config.vertex_shader = pass.vertex_shader.get();
    pipeline_config.pixel_shader = pass.pixel_shader.get();
    pipeline_config.rasterization_state =
        RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
    pipeline_config.depth_state = RenderState::GetNoDepthTestingDepthState();
    pipeline_config.blending_state = RenderState::GetNoBlendingBlendState();
    pipeline_config.framebuffer_state = RenderState::GetColorFramebufferState(output_format);
    pipeline_config.usage = AbstractPipelineUsage::Utility;
    pass.pipeline = g_gfx->CreatePipeline(pipeline_config);
  }
}

void MultipassPostProcessing::BuildPassthroughPipeline()
{
  AbstractFramebuffer* const framebuffer = g_gfx->GetCurrentFramebuffer();
  if (framebuffer == nullptr)
    return;
  const AbstractTextureFormat format = framebuffer->GetColorFormat();
  if (m_passthrough_pipeline && m_passthrough_format == format)
    return;

  // Fullscreen-triangle vertex shader + a plain textured copy. Uses Dolphin's per-backend
  // shader macros (defined by the backend header CreateShaderFromSource prepends), so it works
  // for any backbuffer format -- unlike ScaleTexture, which only supports RGBA8 targets.
  // Vulkan needs Y inverted (matching the old post-processor's vertex shader).
  const std::string flip_y = g_backend_info.api_type == APIType::Vulkan ?
                                 "  gl_Position.y = -gl_Position.y;\n" :
                                 "";
  const std::string vertex_source =
      "VARYING_LOCATION(0) out float2 v_tex0;\n"
      "void main() {\n"
      "  v_tex0 = float2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
      "  gl_Position = float4(v_tex0 * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n" +
      flip_y +
      "}\n";
  const char* const pixel_source =
      "SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n"
      "VARYING_LOCATION(0) in float2 v_tex0;\n"
      "FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n"
      "void main() {\n"
      "  ocol0 = texture(samp0, float3(v_tex0, 0.0));\n"
      "}\n";

  m_passthrough_vertex = g_gfx->CreateShaderFromSource(ShaderStage::Vertex, vertex_source, nullptr,
                                                       "slang passthrough vertex");
  m_passthrough_pixel = g_gfx->CreateShaderFromSource(ShaderStage::Pixel, pixel_source, nullptr,
                                                      "slang passthrough pixel");
  m_passthrough_pipeline.reset();
  if (!m_passthrough_vertex || !m_passthrough_pixel)
    return;

  AbstractPipelineConfig config = {};
  config.vertex_shader = m_passthrough_vertex.get();
  config.pixel_shader = m_passthrough_pixel.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetColorFramebufferState(format);
  config.usage = AbstractPipelineUsage::Utility;
  m_passthrough_pipeline = g_gfx->CreatePipeline(config);
  m_passthrough_format = format;
}

void MultipassPostProcessing::BlitFromTexture(const MathUtil::Rectangle<int>& dst,
                                              const MathUtil::Rectangle<int>& src,
                                              const AbstractTexture* src_tex, int src_layer,
                                              u32 native_width, u32 native_height)
{
  if (m_passthrough || m_passes.empty())
  {
    AbstractFramebuffer* const framebuffer = g_gfx->GetCurrentFramebuffer();
    BuildPassthroughPipeline();
    if (!m_passthrough_pipeline)
      return;

    g_gfx->SetTexture(0, src_tex);
    g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
    g_gfx->SetViewportAndScissor(g_gfx->ConvertFramebufferRectangle(dst, framebuffer));
    g_gfx->SetPipeline(m_passthrough_pipeline.get());
    g_gfx->Draw(0, 3);
    return;
  }

  // The shader's SourceSize and the pass-chain sizing use the game's NATIVE resolution (before
  // internal-resolution upscaling) so scanline/mask geometry is identical at any internal
  // resolution; sampling still reads the high-res src_tex. Fall back to the src rect if the
  // caller didn't supply a native size.
  const u32 target_width = static_cast<u32>(dst.GetWidth());
  const u32 target_height = static_cast<u32>(dst.GetHeight());
  const u32 source_width = native_width != 0 ? native_width : static_cast<u32>(src.GetWidth());
  const u32 source_height = native_height != 0 ? native_height : static_cast<u32>(src.GetHeight());
  const AbstractTextureFormat current_format =
      g_gfx->GetCurrentFramebuffer()->GetColorFormat();
  if (current_format != m_framebuffer_format || target_width != m_target_width ||
      target_height != m_target_height || source_width != m_source_width ||
      source_height != m_source_height)
  {
    m_framebuffer_format = current_format;
    m_target_width = target_width;
    m_target_height = target_height;
    m_source_width = source_width;
    m_source_height = source_height;
    RecompilePipeline();
  }

  ++m_frame_count;

  AbstractFramebuffer* const entry_framebuffer = g_gfx->GetCurrentFramebuffer();
  const AbstractTexture* const original_tex = src_tex;
  const AbstractTexture* prev_output = src_tex;
  // SourceSize/OriginalSize use the native resolution (m_source_width/height), so scanline and
  // mask geometry are resolution-independent even though we sample the upscaled src_tex.
  const MathUtil::Rectangle<int> native_rect(0, 0, static_cast<int>(m_source_width),
                                             static_cast<int>(m_source_height));
  MathUtil::Rectangle<int> prev_rect = native_rect;

  const size_t pass_count = m_passes.size();
  for (size_t i = 0; i < pass_count; ++i)
  {
    Pass& pass = m_passes[i];
    if (!pass.pipeline)
      continue;

    const bool is_final = i == pass_count - 1;

    // Resolve and bind this pass's input samplers.
    for (u32 binding = 0; binding < pass.sampler_names.size(); ++binding)
    {
      const std::string& name = pass.sampler_names[binding];
      const AbstractTexture* texture = prev_output;
      SamplerState sampler = pass.input_sampler;

      if (name == "Original")
      {
        texture = original_tex;
      }
      else if (name == "Source")
      {
        texture = prev_output;
      }
      else
      {
        // Alias of an earlier pass?
        bool resolved = false;
        for (size_t j = 0; j < i; ++j)
        {
          if (!m_passes[j].alias.empty() && m_passes[j].alias == name &&
              m_passes[j].output_texture)
          {
            texture = m_passes[j].output_texture.get();
            resolved = true;
            break;
          }
        }
        // LUT?
        if (!resolved)
        {
          for (const Lut& lut : m_luts)
          {
            if (lut.name == name && lut.texture)
            {
              texture = lut.texture.get();
              sampler = lut.sampler;
              resolved = true;
              break;
            }
          }
        }
      }

      if (texture == nullptr)
        texture = original_tex;

      // Ensure any render-target input has finished rendering (Vulkan layout transition).
      if (texture != original_tex && texture != src_tex)
        const_cast<AbstractTexture*>(texture)->FinishedRendering();

      g_gfx->SetTexture(binding, texture);
      g_gfx->SetSamplerState(binding, sampler);
    }

    AbstractFramebuffer* target =
        is_final ? entry_framebuffer : pass.output_framebuffer.get();
    if (target == nullptr)
      continue;

    const MathUtil::Rectangle<int> target_rect =
        is_final ? dst : pass.output_texture->GetRect();

    // Fill + upload the merged PSBlock, packed to match the shader's declared std140 layout.
    // Resolve each member by name: MVP, the *Size semantics, FrameCount, per-input <Name>Size,
    // and #pragma parameter defaults.
    const auto size_vec = [](u32 w, u32 h, float* out) {
      out[0] = static_cast<float>(w);
      out[1] = static_cast<float>(h);
      out[2] = w != 0 ? 1.0f / w : 0.0f;
      out[3] = h != 0 ? 1.0f / h : 0.0f;
    };
    // Resolve a texture size by input name (Source/Original/alias/LUT), for "<Name>Size".
    const auto input_size = [&](const std::string& name, float* out) -> bool {
      if (name == "Source")
      {
        size_vec(static_cast<u32>(prev_rect.GetWidth()), static_cast<u32>(prev_rect.GetHeight()),
                 out);
        return true;
      }
      if (name == "Original")
      {
        size_vec(static_cast<u32>(native_rect.GetWidth()),
                 static_cast<u32>(native_rect.GetHeight()), out);
        return true;
      }
      for (size_t j = 0; j < i; ++j)
      {
        if (m_passes[j].alias == name && m_passes[j].output_texture)
        {
          size_vec(m_passes[j].output_texture->GetWidth(),
                   m_passes[j].output_texture->GetHeight(), out);
          return true;
        }
      }
      for (const Lut& lut : m_luts)
      {
        if (lut.name == name && lut.texture)
        {
          size_vec(lut.texture->GetWidth(), lut.texture->GetHeight(), out);
          return true;
        }
      }
      return false;
    };

    const auto resolver = [&](const std::string& name, float* out, int count) -> bool {
      if (name == "MVP")
      {
        const std::array<float, 16> mvp = IdentityMvp();
        std::copy(mvp.begin(), mvp.end(), out);
        return true;
      }
      if (name == "SourceSize")
      {
        size_vec(static_cast<u32>(prev_rect.GetWidth()), static_cast<u32>(prev_rect.GetHeight()),
                 out);
        return true;
      }
      if (name == "OriginalSize")
      {
        size_vec(static_cast<u32>(native_rect.GetWidth()),
                 static_cast<u32>(native_rect.GetHeight()), out);
        return true;
      }
      if (name == "OutputSize" || name == "FinalViewportSize")
      {
        size_vec(static_cast<u32>(target_rect.GetWidth()),
                 static_cast<u32>(target_rect.GetHeight()), out);
        return true;
      }
      if (name == "FrameCount")
      {
        out[0] = static_cast<float>(m_frame_count);
        return true;
      }
      // "<InputName>Size" -> that input's texture size.
      constexpr std::string_view kSize = "Size";
      if (name.size() > kSize.size() &&
          name.compare(name.size() - kSize.size(), kSize.size(), kSize) == 0)
      {
        if (input_size(name.substr(0, name.size() - kSize.size()), out))
          return true;
      }
      // #pragma parameter default.
      for (const SlangParameter& param : pass.parameters)
      {
        if (param.id == name)
        {
          out[0] = param.default_value;
          return true;
        }
      }
      (void)count;
      return false;  // zero-fill unknown members
    };

    const std::vector<u8> ubo_data = PackSlangUniforms(pass.ubo_members, resolver);
    if (!ubo_data.empty())
      g_vertex_manager->UploadUtilityUniforms(ubo_data.data(), static_cast<u32>(ubo_data.size()));

    g_gfx->SetFramebuffer(target);
    g_gfx->SetViewportAndScissor(g_gfx->ConvertFramebufferRectangle(target_rect, target));
    g_gfx->SetPipeline(pass.pipeline.get());
    g_gfx->Draw(0, 3);

    if (!is_final)
    {
      prev_output = pass.output_texture.get();
      prev_rect = pass.output_texture->GetRect();
    }
  }

  // Ensure the final target is the framebuffer we entered with.
  g_gfx->SetFramebuffer(entry_framebuffer);
  (void)src_layer;
}
}  // namespace VideoCommon
