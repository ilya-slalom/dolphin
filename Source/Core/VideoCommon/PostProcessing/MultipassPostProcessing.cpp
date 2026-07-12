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
#include "Common/MsgHandler.h"

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

// std140 UBO layout matching the semantic member set the translator preserves for the
// crt-royale MVP: mat4 MVP; vec4 SourceSize; vec4 OriginalSize; vec4 OutputSize; uint FrameCount.
// NOTE (per plan Task 9 Step 4): this assumes crt-royale's member order. Generalizing to any
// preset requires SPIR-V reflection of each shader's declared UBO; kept in one struct so that
// change is localized.
struct SemanticUniforms
{
  std::array<float, 16> mvp;
  std::array<float, 4> source_size;
  std::array<float, 4> original_size;
  std::array<float, 4> output_size;
  u32 frame_count;
  u32 pad0;
  u32 pad1;
  u32 pad2;
};

std::array<float, 4> SizeVec(u32 width, u32 height)
{
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  return {w, h, w != 0.0f ? 1.0f / w : 0.0f, h != 0.0f ? 1.0f / h : 0.0f};
}

// Identity MVP with the same clip-space orientation the fixed post-process vertex shader uses.
std::array<float, 16> IdentityMvp()
{
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
}  // namespace

MultipassPostProcessing::MultipassPostProcessing() = default;
MultipassPostProcessing::~MultipassPostProcessing() = default;

std::vector<std::string> MultipassPostProcessing::GetPresetList()
{
  const std::string user_dir = File::GetUserPath(D_SHADERS_IDX);
  const std::string sys_dir = File::GetSysDirectory() + SHADERS_DIR DIR_SEP;
  const std::array<std::string_view, 2> dirs = {user_dir, sys_dir};
  const std::array<std::string_view, 1> exts = {".slangp"};
  const std::vector<std::string> paths = Common::DoFileSearch(dirs, exts, /*recursive=*/true);

  std::vector<std::string> result;
  result.reserve(paths.size());
  for (const std::string& path : paths)
  {
    std::string name;
    SplitPath(path, nullptr, &name, nullptr);
    result.push_back(name);
  }
  return result;
}

bool MultipassPostProcessing::Initialize(AbstractTextureFormat format)
{
  m_framebuffer_format = format;
  const bool ok = LoadPreset(Config::Get(Config::GFX_ENHANCE_POST_SHADER));
  if (!m_passthrough)
    RecompilePipeline();
  return ok;
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

bool MultipassPostProcessing::LoadPreset(const std::string& preset_name)
{
  ClearChain();

  if (preset_name.empty())
  {
    m_passthrough = true;
    return true;
  }

  // Resolve preset path: user Shaders dir first, then Sys.
  std::string path = File::GetUserPath(D_SHADERS_IDX) + preset_name + ".slangp";
  if (!File::Exists(path))
    path = File::GetSysDirectory() + SHADERS_DIR DIR_SEP + preset_name + ".slangp";
  if (!File::Exists(path))
  {
    m_passthrough = true;
    return true;
  }

  std::string text;
  if (!File::ReadFileToString(path, text))
  {
    m_passthrough = true;
    return true;
  }

  const std::string base_dir = DirectoryOf(path);
  std::string error;
  const auto config = ParseSlangPreset(text, base_dir, &error);
  if (!config)
  {
    PanicAlertFmt("Failed to load shader preset {}: {}", preset_name, error);
    m_passthrough = true;
    return false;
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

  // Build passes, accumulating known aliases as we go.
  std::vector<std::string> known_aliases;
  for (const SlangPassConfig& pass_config : config->passes)
  {
    std::string shader_text;
    if (!File::ReadFileToString(pass_config.shader_path, shader_text))
    {
      PanicAlertFmt("Failed to read slang shader {}", pass_config.shader_path);
      ClearChain();
      return false;
    }

    const auto parsed = ParseSlangShader(shader_text, &error);
    if (!parsed)
    {
      PanicAlertFmt("Failed to parse slang shader {}: {}", pass_config.shader_path, error);
      ClearChain();
      return false;
    }

    TranslatedPass translated = TranslateSlangPass(*parsed, known_aliases, lut_names);
    if (!translated.ok)
    {
      PanicAlertFmt("Failed to translate slang shader {}: {}", pass_config.shader_path,
                    translated.error);
      ClearChain();
      return false;
    }

    CompiledPassShaders shaders =
        CompileTranslatedPass(translated, DirectoryOf(pass_config.shader_path));
    if (!shaders.vertex || !shaders.pixel)
    {
      PanicAlertFmt("Failed to compile slang shader {}", pass_config.shader_path);
      ClearChain();
      return false;
    }

    Pass pass;
    pass.config = pass_config;
    pass.alias = pass_config.alias;
    pass.sampler_names = std::move(translated.sampler_names);
    pass.input_sampler =
        MakeSlangSamplerState(pass_config.wrap_mode, pass_config.filter_linear,
                              pass_config.mipmap_input);
    pass.vertex_shader = std::move(shaders.vertex);
    pass.pixel_shader = std::move(shaders.pixel);
    m_passes.push_back(std::move(pass));

    if (!pass_config.alias.empty())
      known_aliases.push_back(pass_config.alias);
  }

  m_passthrough = m_passes.empty();
  return true;
}

void MultipassPostProcessing::RecompilePipeline()
{
  if (m_passthrough || m_framebuffer_format == AbstractTextureFormat::Undefined)
    return;

  const u32 viewport_width = std::max<u32>(1, m_target_width);
  const u32 viewport_height = std::max<u32>(1, m_target_height);

  u32 source_width = viewport_width;
  u32 source_height = viewport_height;

  const size_t pass_count = m_passes.size();
  for (size_t i = 0; i < pass_count; ++i)
  {
    Pass& pass = m_passes[i];
    const bool is_final = i == pass_count - 1;
    const AbstractTextureFormat output_format =
        is_final ? m_framebuffer_format : INTERMEDIATE_FORMAT;

    if (!is_final)
    {
      const u32 out_w = ComputePassAxisSize(pass.config.scale_type_x, pass.config.scale_x,
                                            source_width, viewport_width);
      const u32 out_h = ComputePassAxisSize(pass.config.scale_type_y, pass.config.scale_y,
                                            source_height, viewport_height);

      const TextureConfig texture_config(out_w, out_h, 1, 1, 1, INTERMEDIATE_FORMAT,
                                         AbstractTextureFlag_RenderTarget,
                                         AbstractTextureType::Texture_2DArray);
      pass.output_texture =
          g_gfx->CreateTexture(texture_config, "slang pass " + std::to_string(i));
      pass.output_framebuffer =
          pass.output_texture ? g_gfx->CreateFramebuffer(pass.output_texture.get(), nullptr)
                              : nullptr;

      source_width = out_w;
      source_height = out_h;
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

void MultipassPostProcessing::BlitFromTexture(const MathUtil::Rectangle<int>& dst,
                                              const MathUtil::Rectangle<int>& src,
                                              const AbstractTexture* src_tex, int src_layer)
{
  if (m_passthrough || m_passes.empty())
  {
    g_gfx->ScaleTexture(g_gfx->GetCurrentFramebuffer(), dst, src_tex, src);
    return;
  }

  // Track target size; reallocate render targets if the on-screen size changed.
  const u32 target_width = static_cast<u32>(dst.GetWidth());
  const u32 target_height = static_cast<u32>(dst.GetHeight());
  const AbstractTextureFormat current_format =
      g_gfx->GetCurrentFramebuffer()->GetColorFormat();
  if (current_format != m_framebuffer_format || target_width != m_target_width ||
      target_height != m_target_height)
  {
    m_framebuffer_format = current_format;
    m_target_width = target_width;
    m_target_height = target_height;
    RecompilePipeline();
  }

  ++m_frame_count;

  AbstractFramebuffer* const entry_framebuffer = g_gfx->GetCurrentFramebuffer();
  const AbstractTexture* const original_tex = src_tex;
  const AbstractTexture* prev_output = src_tex;
  MathUtil::Rectangle<int> prev_rect = src;

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

    // Fill + upload the semantic UBO.
    SemanticUniforms uniforms = {};
    uniforms.mvp = IdentityMvp();
    uniforms.source_size = SizeVec(static_cast<u32>(prev_rect.GetWidth()),
                                   static_cast<u32>(prev_rect.GetHeight()));
    uniforms.original_size = SizeVec(static_cast<u32>(src.GetWidth()),
                                     static_cast<u32>(src.GetHeight()));
    uniforms.output_size =
        SizeVec(static_cast<u32>(target_rect.GetWidth()),
                static_cast<u32>(target_rect.GetHeight()));
    uniforms.frame_count = m_frame_count;
    g_vertex_manager->UploadUtilityUniforms(&uniforms, sizeof(uniforms));

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
