// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/LibrashaderPostProcessing.h"

#include <string>
#include <utility>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/Config/GraphicsSettings.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/PostProcessing/ChainDebugDump.h"
#include "VideoCommon/PostProcessing/ChainOutputPolicy.h"
#include "VideoCommon/PostProcessing/LibrashaderLoader.h"
#include "VideoCommon/PostProcessing/LibrashaderRuntime.h"
#include "VideoCommon/PostProcessing/SlangTranslator.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon
{
std::string ResolvePresetPath(const std::string& preset_spec)
{
  // librashader accepts a single preset, so only the first entry of a ';'-separated chain is used.
  const auto separator = preset_spec.find(';');
  std::string dropped =
      separator == std::string::npos ? std::string() : preset_spec.substr(separator + 1);
  // A tail of nothing but separators and whitespace ("shader;", "shader; ") drops nothing a user
  // would want to hear about, so it is not worth a warning.
  if (dropped.find_first_not_of(" \t;") == std::string::npos)
    dropped.clear();

  std::string name = preset_spec.substr(0, separator);
  const auto first = name.find_first_not_of(" \t");
  if (first == std::string::npos)
  {
    // Nothing before the separator (";shader"): the entries after it are dropped like any other
    // tail, except that here they were the whole request. Warn anyway -- this is the mis-typed
    // input most in need of an explanation, and it used to fail silently.
    if (!dropped.empty())
    {
      WARN_LOG_FMT(VIDEO,
                   "Librashader: preset '{}' has an empty first entry, so nothing is loaded; only "
                   "one preset is supported, so '{}' is ignored",
                   preset_spec, dropped);
    }
    return {};
  }
  const auto last = name.find_last_not_of(" \t");
  name = name.substr(first, last - first + 1);

  if (!dropped.empty())
  {
    WARN_LOG_FMT(VIDEO, "Librashader: only one preset is supported; using '{}' and ignoring '{}'",
                 name, dropped);
  }

  std::string path = File::GetUserPath(D_SHADERS_IDX) + "shaders_slang" DIR_SEP + name + ".slangp";
  if (!File::Exists(path))
    path = File::GetUserPath(D_SHADERS_IDX) + name + ".slangp";
  if (!File::Exists(path))
    path = File::GetSysDirectory() + SHADERS_DIR DIR_SEP "shaders_slang" DIR_SEP + name + ".slangp";
  if (!File::Exists(path))
    path = File::GetSysDirectory() + SHADERS_DIR DIR_SEP + name + ".slangp";
  if (!File::Exists(path))
    return {};
  return path;
}

LibrashaderPostProcessing::LibrashaderPostProcessing(std::unique_ptr<LibrashaderRuntime> runtime)
    : m_runtime(std::move(runtime))
{
}

LibrashaderPostProcessing::~LibrashaderPostProcessing()
{
  // Free the chain here rather than relying on every runtime's destructor to remember. The runtime
  // object is still fully alive in this body, so the virtual call dispatches normally; DestroyChain
  // is required to be idempotent for exactly this reason.
  m_runtime->DestroyChain();
}

bool LibrashaderPostProcessing::Initialize(AbstractTextureFormat format)
{
  m_format = format;
  m_available = m_runtime->IsSupported();
  if (!m_available)
    return false;

  RecompileShader();
  return true;
}

void LibrashaderPostProcessing::RecompileShader()
{
  // Free any previous chain before rebuilding.
  m_runtime->DestroyChain();
  m_frame_count = 0;

  if (!m_available)
    return;

  const std::string preset_name = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
  const std::string path = ResolvePresetPath(preset_name);
  if (path.empty())
  {
    if (!preset_name.empty())
    {
      WARN_LOG_FMT(VIDEO, "Librashader: preset '{}' not found; falling back to passthrough",
                   preset_name);
    }
    return;
  }

  libra_shader_preset_t preset = nullptr;
  const std::string error =
      Librashader::DescribeAndFreeError(Librashader::Common().preset_create(path.c_str(), &preset));
  if (!error.empty())
  {
    ERROR_LOG_FMT(VIDEO, "Librashader: preset_create('{}') failed: {}", path, error);
    return;
  }

  // CreateChain consumes `preset` on every path, so there is nothing to free here.
  if (m_runtime->CreateChain(preset))
    INFO_LOG_FMT(VIDEO, "Librashader: filter chain created from '{}'", path);
}

void LibrashaderPostProcessing::RecompilePipeline()
{
  // librashader owns its internal pipelines and rebuilds them as part of the filter chain, so there
  // is nothing backend-pipeline-specific to rebuild here. The passthrough pipeline is (re)built
  // lazily in BlitFromTexture when the framebuffer format changes.
}

void LibrashaderPostProcessing::BuildPassthroughPipeline()
{
  AbstractFramebuffer* const framebuffer = g_gfx->GetCurrentFramebuffer();
  if (framebuffer == nullptr)
    return;
  const AbstractTextureFormat format = framebuffer->GetColorFormat();
  if (m_passthrough_pipeline && m_passthrough_format == format)
    return;

  // Fullscreen-triangle copy, identical to MultipassPostProcessing's passthrough -- including the
  // conditional flip, which is only correct on the backends whose clip space is Y-down. Hardcoding
  // it went unnoticed while Vulkan was the only backend here; on D3D it inverts the whole frame.
  // This draw targets the presented framebuffer, so it asks SlangNeedsPresentClipYFlip and not
  // SlangNeedsClipYFlip: on OpenGL the latter is the answer for a texture target only.
  const std::string flip_y = SlangNeedsPresentClipYFlip(g_backend_info.api_type) ?
                                 "  gl_Position.y = -gl_Position.y;\n" :
                                 "";
  const std::string vertex_source =
      "VARYING_LOCATION(0) out float2 v_tex0;\n"
      "void main() {\n"
      "  v_tex0 = float2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
      "  gl_Position = float4(v_tex0 * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n" +
      flip_y + "}\n";
  const char* const pixel_source = "SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n"
                                   "VARYING_LOCATION(0) in float2 v_tex0;\n"
                                   "FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n"
                                   "void main() {\n"
                                   "  ocol0 = texture(samp0, float3(v_tex0, 0.0));\n"
                                   "}\n";

  m_passthrough_vertex = g_gfx->CreateShaderFromSource(ShaderStage::Vertex, vertex_source, nullptr,
                                                       "librashader passthrough vertex");
  m_passthrough_pixel = g_gfx->CreateShaderFromSource(ShaderStage::Pixel, pixel_source, nullptr,
                                                      "librashader passthrough pixel");
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

void LibrashaderPostProcessing::BuildDownscalePipeline(const SlangSourceDownscalePlan& plan,
                                                       AbstractTextureFormat format)
{
  const u32 factor = plan.box_filter ? plan.factor : 0;
  if (m_downscale_pipeline && m_downscale_is_box == plan.box_filter &&
      m_downscale_factor == factor && m_downscale_format == format)
  {
    return;
  }

  // Fullscreen triangle. On the Y-down-clip-space backends the flip is what makes v_tex0 align
  // with gl_FragCoord's top-left origin, so the bilinear path (v_tex0) and the box path (texelFetch
  // on gl_FragCoord) share one orientation and both preserve the source's orientation into the
  // native texture. On D3D and Metal clip space already agrees with gl_FragCoord, so adding it
  // there inverts the native source instead.
  const std::string flip_y =
      SlangNeedsClipYFlip(g_backend_info.api_type) ? "  gl_Position.y = -gl_Position.y;\n" : "";
  const std::string vertex_source =
      "VARYING_LOCATION(0) out float2 v_tex0;\n"
      "void main() {\n"
      "  v_tex0 = float2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
      "  gl_Position = float4(v_tex0 * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n" +
      flip_y + "}\n";

  std::string pixel_source;
  if (plan.box_filter)
  {
    // Box average over the whole factor x factor footprint: real SSAA, cheap because the taps run
    // over the small native target. The factor is baked as a literal so the loop bounds are
    // compile-time constant (this is why the shader is rebuilt when the factor changes).
    const std::string n = std::to_string(plan.factor);
    const std::string n2 = std::to_string(plan.factor * plan.factor);
    pixel_source = "SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n"
                   "FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n"
                   "void main() {\n"
                   "  int2 base = int2(gl_FragCoord.xy) * " +
                   n +
                   ";\n"
                   "  float4 sum = float4(0.0, 0.0, 0.0, 0.0);\n"
                   "  for (int y = 0; y < " +
                   n +
                   "; ++y)\n"
                   "    for (int x = 0; x < " +
                   n +
                   "; ++x)\n"
                   "      sum += texelFetch(samp0, int3(base + int2(x, y), 0), 0);\n"
                   "  ocol0 = sum * (1.0 / " +
                   n2 +
                   ".0);\n"
                   "}\n";
  }
  else
  {
    // Fractional or mismatched factor: a single bilinear tap. Not SSAA, but correct and
    // orientation-preserving; the exact-integer case above upgrades this to box averaging.
    pixel_source = "SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n"
                   "VARYING_LOCATION(0) in float2 v_tex0;\n"
                   "FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n"
                   "void main() {\n"
                   "  ocol0 = texture(samp0, float3(v_tex0, 0.0));\n"
                   "}\n";
  }

  m_downscale_vertex = g_gfx->CreateShaderFromSource(ShaderStage::Vertex, vertex_source, nullptr,
                                                     "librashader downscale vertex");
  m_downscale_pixel = g_gfx->CreateShaderFromSource(ShaderStage::Pixel, pixel_source, nullptr,
                                                    "librashader downscale pixel");
  m_downscale_pipeline.reset();
  if (!m_downscale_vertex || !m_downscale_pixel)
    return;

  AbstractPipelineConfig config = {};
  config.vertex_shader = m_downscale_vertex.get();
  config.pixel_shader = m_downscale_pixel.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetColorFramebufferState(format);
  config.usage = AbstractPipelineUsage::Utility;
  m_downscale_pipeline = g_gfx->CreatePipeline(config);

  m_downscale_is_box = plan.box_filter;
  m_downscale_factor = factor;
  m_downscale_format = format;
}

AbstractTexture*
LibrashaderPostProcessing::DownscaleToNativeSource(const SlangSourceDownscalePlan& plan,
                                                   const AbstractTexture* src_tex, u32 native_width,
                                                   u32 native_height)
{
  const AbstractTextureFormat format = src_tex->GetFormat();

  // (Re)allocate the native-res render target whenever its size or format changes.
  if (!m_native_source || m_native_source_width != native_width ||
      m_native_source_height != native_height || m_native_source_format != format)
  {
    m_native_source_fb.reset();
    m_native_source.reset();
    const TextureConfig config(native_width, native_height, 1, 1, 1, format,
                               AbstractTextureFlag_RenderTarget,
                               AbstractTextureType::Texture_2DArray);
    m_native_source = g_gfx->CreateTexture(config, "librashader native source");
    if (m_native_source)
      m_native_source_fb = g_gfx->CreateFramebuffer(m_native_source.get(), nullptr);
    m_native_source_width = native_width;
    m_native_source_height = native_height;
    m_native_source_format = format;
  }
  if (!m_native_source || !m_native_source_fb)
    return nullptr;

  BuildDownscalePipeline(plan, format);
  if (!m_downscale_pipeline)
    return nullptr;

  // We overwrite every native texel, so discard the prior contents. Box averaging reads exact
  // texels (point sampler); the bilinear fallback needs a linear sampler.
  g_gfx->SetAndDiscardFramebuffer(m_native_source_fb.get());
  g_gfx->SetTexture(0, src_tex);
  g_gfx->SetSamplerState(0, plan.box_filter ? RenderState::GetPointSamplerState() :
                                              RenderState::GetLinearSamplerState());
  g_gfx->SetViewportAndScissor(
      g_gfx->ConvertFramebufferRectangle(m_native_source->GetRect(), m_native_source_fb.get()));
  g_gfx->SetPipeline(m_downscale_pipeline.get());
  g_gfx->Draw(0, 3);

  // The result is handed to librashader as a shader-read source, but nothing is done about that
  // here: the caller's g_gfx->SetFramebuffer(framebuffer) restore ends the render pass this draw
  // opened, and RunFrame() transitions whichever source it is given, so the shader-read transition
  // is deferred to it rather than issued twice.
  return m_native_source.get();
}

AbstractFramebuffer* LibrashaderPostProcessing::EnsureOutputTarget(u32 width, u32 height,
                                                                   AbstractTextureFormat format)
{
  if (!m_output_target || m_output_target_width != width || m_output_target_height != height ||
      m_output_target_format != format)
  {
    m_output_target_fb.reset();
    m_output_target.reset();
    const TextureConfig config(width, height, 1, 1, 1, format, AbstractTextureFlag_RenderTarget,
                               AbstractTextureType::Texture_2DArray);
    m_output_target = g_gfx->CreateTexture(config, "librashader chain output");
    if (m_output_target)
      m_output_target_fb = g_gfx->CreateFramebuffer(m_output_target.get(), nullptr);
    m_output_target_width = width;
    m_output_target_height = height;
    m_output_target_format = format;
  }
  return m_output_target_fb.get();
}

void LibrashaderPostProcessing::BlitFromTexture(const MathUtil::Rectangle<int>& dst,
                                                const MathUtil::Rectangle<int>& src,
                                                const AbstractTexture* src_tex, int src_layer,
                                                u32 native_width, u32 native_height)
{
  AbstractFramebuffer* const framebuffer = g_gfx->GetCurrentFramebuffer();
  if (framebuffer == nullptr)
    return;

  // Drive the real librashader filter chain when it was created successfully. If there is no chain
  // (no preset, preset failed, or a required device extension is missing) we skip straight to the
  // passthrough copy so the screen never blanks.
  if (m_runtime->HasChain())
  {
    // librashader derives SourceSize/OriginalSize from the input image's dimensions, and CRT
    // presets (crt-royale, RetroCrisis) scale their scanline and phosphor-mask geometry by
    // SourceSize. The XFB source is at the internal (upscaled) resolution, so feeding it directly
    // reports SourceSize = internal res: the mask/scanline period shrinks with the IR multiplier
    // (moire, invisible scanlines) and every pass runs against the oversized frame. Worse,
    // librashader's single bilinear tap subsamples that upscaled frame, aliasing high-frequency
    // content into the NTSC/scanline bands. We instead materialize a REAL native-resolution source
    // by box-averaging the whole footprint (SSAA) for integer upscales, bilinear for fractional --
    // so the chain computes geometry against native pixels while keeping supersampled detail. No-op
    // at 1x or when no native size is supplied.
    const SlangSourceDownscalePlan plan = PlanSlangSourceDownscale(
        src_tex->GetWidth(), src_tex->GetHeight(), native_width, native_height);
    const AbstractTexture* source = src_tex;
    if (plan.downscale)
    {
      if (const AbstractTexture* native =
              DownscaleToNativeSource(plan, src_tex, native_width, native_height))
      {
        source = native;
      }
      // The downscale draw left its own framebuffer bound; restore the caller's so that a
      // fall-through to the passthrough copy (on chain error) targets the screen, not the native
      // RT. This restore is also what ends the render pass the downscale opened, which is why
      // DownscaleToNativeSource does not end one itself -- do not remove or move it out of this
      // branch without putting that back, or the chain's barriers get recorded inside that pass.
      g_gfx->SetFramebuffer(framebuffer);
    }

    // UAT instrument for finding 3: dump chain input and output from one frame, once per run.
    const bool dump_images = ShouldDumpChainImages();
    if (dump_images)
      DumpChainImage(source, "chain-input");

    // librashader derives OutputSize, FinalViewportSize, and every scale_type=viewport framebuffer
    // size from the OUTPUT IMAGE's dimensions, then merely scissors rendering to the viewport rect.
    // Handing it the full backbuffer plus a pillarboxed sub-rect would size the whole chain
    // (phosphor mask, scanline geometry, NTSC subcarrier) for the backbuffer width while the pixels
    // land in the narrower draw rect -- a fixed fractional mismatch that beats against the panel
    // pixel grid as vertical moire, independent of internal resolution. We instead render the chain
    // into a draw-rect-sized target at viewport origin (0,0) so OutputSize == the drawn extent,
    // then blit that 1:1 into the backbuffer at the draw rect. This mirrors how ARMSX2 drives
    // librashader.
    if (ShouldRenderChainDirectly(dst, framebuffer->GetWidth(), framebuffer->GetHeight(),
                                  framebuffer->GetColorAttachment() != nullptr))
    {
      // The draw rect is the entire backbuffer, so OutputSize is identical whether the chain
      // targets the intermediate texture or the backbuffer itself: render straight into the
      // backbuffer and skip the extra full-frame write + read of the 1:1 blit. The chain's final
      // pass covers every backbuffer pixel, which also makes the clear BindBackbuffer deferred
      // redundant.
      m_runtime->DiscardPendingTargetClear();
      if (m_runtime->RunFrame(source, framebuffer, m_frame_count++))
      {
        if (dump_images)
        {
          // UAT instrument: the output is the backbuffer, which cannot be read back reliably. A
          // windowed run (where ShouldRenderChainDirectly returns false) produces both images.
          INFO_LOG_FMT(VIDEO, "Librashader: chain-output dump skipped (direct-to-backbuffer path); "
                              "run windowed to force the intermediate texture path");
          NoteChainImagesDumped();
        }
        return;
      }
      // On error, fall through to the passthrough copy; it covers the full rect, so the discarded
      // clear is not missed.
    }
    else
    {
      // GetColorFormat() rather than the attachment's own format: they agree whenever there is an
      // attachment, and OpenGL's window framebuffer has none but still reports the format it
      // presents (RGBA8), which is the format the 1:1 blit below has to match.
      AbstractFramebuffer* const chain_fb =
          EnsureOutputTarget(static_cast<u32>(dst.GetWidth()), static_cast<u32>(dst.GetHeight()),
                             framebuffer->GetColorFormat());
      if (chain_fb != nullptr && m_runtime->RunFrame(source, chain_fb, m_frame_count++))
      {
        AbstractTexture* const chain_output = chain_fb->GetColorAttachment();
        if (dump_images)
        {
          // UAT instrument: dump the chain output before presenting it to the backbuffer.
          DumpChainImage(chain_output, "chain-output");
          NoteChainImagesDumped();
        }

        // Present the chain output 1:1 into the backbuffer draw rect. Point sampling keeps the copy
        // exact (target and rect are equal size). SetTexture takes the chain output out of the
        // render-target state RunFrame left it in.
        BuildPassthroughPipeline();
        if (m_passthrough_pipeline)
        {
          g_gfx->SetFramebuffer(framebuffer);
          g_gfx->SetTexture(0, chain_output);
          g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
          g_gfx->SetViewportAndScissor(g_gfx->ConvertFramebufferRectangle(dst, framebuffer));
          g_gfx->SetPipeline(m_passthrough_pipeline.get());
          g_gfx->Draw(0, 3);
        }
        return;
      }
      // On error, fall through to the passthrough copy for this frame so the screen never blanks.
    }
  }

  BuildPassthroughPipeline();
  if (!m_passthrough_pipeline)
    return;

  g_gfx->SetTexture(0, src_tex);
  g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
  g_gfx->SetViewportAndScissor(g_gfx->ConvertFramebufferRectangle(dst, framebuffer));
  g_gfx->SetPipeline(m_passthrough_pipeline.get());
  g_gfx->Draw(0, 3);
}
}  // namespace VideoCommon
