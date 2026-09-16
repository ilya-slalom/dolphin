// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/LibrashaderPostProcessing.h"

#include <string>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/Config/GraphicsSettings.h"

// VulkanContext.h transitively includes VulkanLoader.h, which includes <vulkan/vulkan.h> with
// VK_NO_PROTOTYPES and declares Dolphin's function-pointer globals (including ::vkGetInstanceProcAddr).
// It MUST be included before librashader_ld.h so that when librashader.h re-includes
// <vulkan/vulkan.h> the header guard suppresses conflicting prototype declarations.
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/PostProcessing/ChainOutputPolicy.h"
#include "VideoCommon/PostProcessing/LibrashaderLibrary.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"

// Override librashader_ld.h's bare-name load with the absolute packaged path. Android and Linux
// keep the header's default, which their loaders resolve correctly.
#if defined(_WIN32)
#include <windows.h>
#include "Common/StringUtil.h"
#define _LIBRASHADER_LOAD LoadLibraryW(UTF8ToWString(VideoCommon::LibrashaderLibraryPath()).c_str())
#elif defined(__APPLE__) && !defined(ANDROID)
#include <dlfcn.h>
#define _LIBRASHADER_LOAD dlopen(VideoCommon::LibrashaderLibraryPath().c_str(), RTLD_LAZY)
#endif

// librashader_ld.h defines many static-inline no-op stubs plus librashader_load_instance(); it is
// included in exactly this one translation unit. LIBRA_RUNTIME_VULKAN selects the Vulkan runtime
// entry points.
#define LIBRA_RUNTIME_VULKAN
#include <librashader_ld.h>

namespace Vulkan
{
// Loads the librashader shared library exactly once. The header guarantees the returned instance is
// always safe to call: unresolved symbols point at no-op stubs, so a missing/incompatible .so
// degrades to passthrough rather than crashing. instance_loaded is the authoritative "did it load"
// flag and is only meaningful right after loading, which is why we cache the instance here.
static const libra_instance_t& GetLibrashaderInstance()
{
  static const libra_instance_t s_instance = librashader_load_instance();
  return s_instance;
}

// Logs a librashader error (if any) and frees it. Returns true if an error was present.
static bool CheckError(const libra_instance_t& lib, libra_error_t error, const char* context)
{
  if (error == nullptr)
    return false;

  ERROR_LOG_FMT(VIDEO, "Librashader: {} failed (errno {})", context,
                static_cast<int>(lib.error_errno(error)));
  lib.error_free(&error);
  return true;
}

// Resolves a post-processing preset name to an absolute .slangp path using the identical search
// order as MultipassPostProcessing::AppendPreset(), so both engines consume the same preset file.
// librashader accepts a single preset, so only the first entry of a ';'-separated chain is used.
static std::string ResolvePresetPath(const std::string& preset_spec)
{
  std::string name = preset_spec.substr(0, preset_spec.find(';'));
  const auto first = name.find_first_not_of(" \t");
  if (first == std::string::npos)
    return {};
  const auto last = name.find_last_not_of(" \t");
  name = name.substr(first, last - first + 1);

  std::string path =
      File::GetUserPath(D_SHADERS_IDX) + "shaders_slang" DIR_SEP + name + ".slangp";
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

LibrashaderPostProcessing::LibrashaderPostProcessing() = default;

LibrashaderPostProcessing::~LibrashaderPostProcessing()
{
  if (m_chain != nullptr)
    GetLibrashaderInstance().vk_filter_chain_free(&m_chain);
}

bool LibrashaderPostProcessing::IsAvailable()
{
  return GetLibrashaderInstance().instance_loaded;
}

bool LibrashaderPostProcessing::Initialize(AbstractTextureFormat format)
{
  m_format = format;
  m_available = IsAvailable();
  if (!m_available)
    return false;

  RecompileShader();
  return true;
}

void LibrashaderPostProcessing::RecompileShader()
{
  const libra_instance_t& lib = GetLibrashaderInstance();

  // Free any previous chain before rebuilding.
  if (m_chain != nullptr)
  {
    lib.vk_filter_chain_free(&m_chain);
    m_chain = nullptr;
  }
  m_frame_count = 0;

  if (!m_available)
    return;

  const std::string preset_spec = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
  const std::string path = ResolvePresetPath(preset_spec);
  if (path.empty())
  {
    WARN_LOG_FMT(VIDEO, "Librashader: preset '{}' not found; falling back to passthrough",
                 preset_spec);
    return;
  }

  libra_shader_preset_t preset = nullptr;
  if (CheckError(lib, lib.preset_create(path.c_str(), &preset), "preset_create"))
    return;

  // libra_device_vk_t = { physical_device, instance, device, queue, entry }. `entry` is the
  // vkGetInstanceProcAddr loader; Dolphin's global (declared in VulkanLoader.h) already holds the
  // resolved pointer, so it is passed by value.
  libra_device_vk_t device = {};
  device.physical_device = g_vulkan_context->GetPhysicalDevice();
  device.instance = g_vulkan_context->GetVulkanInstance();
  device.device = g_vulkan_context->GetDevice();
  device.queue = g_vulkan_context->GetGraphicsQueue();
  device.entry = ::vkGetInstanceProcAddr;

  // Chain options. frames_in_flight 0 selects librashader's default of three, which is >= Dolphin's
  // NUM_FRAMES_IN_FLIGHT (2): a pass's per-frame objects are never recycled while a submit that still
  // references them can be in flight. Dynamic rendering (when the device has it and librashader can
  // resolve vkCmdBeginRendering) avoids one VkFramebuffer per pass per frame; in render-pass mode
  // librashader creates those objects every frame.
  filter_chain_vk_opt_t options = {};
  options.version = LIBRASHADER_CURRENT_VERSION;
  options.frames_in_flight = 0;
  options.force_no_mipmaps = false;
  options.use_dynamic_rendering = VideoCommon::ChooseDynamicRendering(
      g_vulkan_context->SupportsDynamicRendering() &&
          Config::Get(Config::GFX_LIBRASHADER_DYNAMIC_RENDERING),
      vkGetDeviceProcAddr(g_vulkan_context->GetDevice(), "vkCmdBeginRendering") != nullptr);
  options.disable_cache = false;

  // vk_filter_chain_create invalidates the preset handle regardless of success or failure (see the
  // header: "the shader preset is immediately invalidated"), so it must not be freed on either path
  // -- doing so would be a double-free. On error, leave m_chain null (passthrough).
  if (CheckError(lib, lib.vk_filter_chain_create(&preset, device, &options, &m_chain),
                 "vk_filter_chain_create"))
  {
    m_chain = nullptr;
    return;
  }

  INFO_LOG_FMT(VIDEO, "Librashader: filter chain created from '{}' (dynamic rendering {})", path,
               options.use_dynamic_rendering ? "on" : "off");
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

  // Fullscreen-triangle copy, identical to MultipassPostProcessing's passthrough. Vulkan needs Y
  // inverted to match the presenter's expected orientation.
  const std::string vertex_source =
      "VARYING_LOCATION(0) out float2 v_tex0;\n"
      "void main() {\n"
      "  v_tex0 = float2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
      "  gl_Position = float4(v_tex0 * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
      "  gl_Position.y = -gl_Position.y;\n"
      "}\n";
  const char* const pixel_source =
      "SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n"
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

void LibrashaderPostProcessing::BuildDownscalePipeline(
    const VideoCommon::SlangSourceDownscalePlan& plan, AbstractTextureFormat format)
{
  const u32 factor = plan.box_filter ? plan.factor : 0;
  if (m_downscale_pipeline && m_downscale_is_box == plan.box_filter &&
      m_downscale_factor == factor && m_downscale_format == format)
  {
    return;
  }

  // Fullscreen triangle. The Y flip makes v_tex0 align with gl_FragCoord's top-left origin, so the
  // bilinear path (v_tex0) and the box path (texelFetch on gl_FragCoord) share one orientation and
  // both preserve the source's orientation into the native texture.
  const char* const vertex_source =
      "VARYING_LOCATION(0) out float2 v_tex0;\n"
      "void main() {\n"
      "  v_tex0 = float2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
      "  gl_Position = float4(v_tex0 * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
      "  gl_Position.y = -gl_Position.y;\n"
      "}\n";

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
    // Fractional or mismatched factor: a single bilinear tap. Not SSAA, but correct and orientation-
    // preserving; the exact-integer case above upgrades this to box averaging.
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

const AbstractTexture* LibrashaderPostProcessing::DownscaleToNativeSource(
    const VideoCommon::SlangSourceDownscalePlan& plan, const AbstractTexture* src_tex,
    u32 native_width, u32 native_height)
{
  const AbstractTextureFormat format = src_tex->GetFormat();

  // (Re)allocate the native-res render target whenever its size or format changes.
  if (!m_native_source || m_native_source_width != native_width ||
      m_native_source_height != native_height || m_native_source_format != format)
  {
    m_native_source_fb.reset();
    m_native_source.reset();
    const TextureConfig config(native_width, native_height, 1, 1, 1, format,
                               AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2DArray);
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
  g_gfx->SetSamplerState(0, plan.box_filter ? RenderState::GetPointSamplerState()
                                            : RenderState::GetLinearSamplerState());
  g_gfx->SetViewportAndScissor(
      g_gfx->ConvertFramebufferRectangle(m_native_source->GetRect(), m_native_source_fb.get()));
  g_gfx->SetPipeline(m_downscale_pipeline.get());
  g_gfx->Draw(0, 3);

  // Hand the result to librashader as a shader-read source. End the render pass first so neither
  // the layout transition nor the following filter-chain frame is recorded inside our draw's pass.
  auto* native = static_cast<VKTexture*>(m_native_source.get());
  StateTracker::GetInstance()->EndRenderPass();
  native->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return native;
}

AbstractTexture* LibrashaderPostProcessing::EnsureOutputTarget(u32 width, u32 height,
                                                               AbstractTextureFormat format)
{
  if (!m_output_target || m_output_target_width != width || m_output_target_height != height ||
      m_output_target_format != format)
  {
    m_output_target.reset();
    const TextureConfig config(width, height, 1, 1, 1, format, AbstractTextureFlag_RenderTarget,
                               AbstractTextureType::Texture_2DArray);
    m_output_target = g_gfx->CreateTexture(config, "librashader chain output");
    m_output_target_width = width;
    m_output_target_height = height;
    m_output_target_format = format;
  }
  return m_output_target.get();
}

void LibrashaderPostProcessing::BlitFromTexture(const MathUtil::Rectangle<int>& dst,
                                                const MathUtil::Rectangle<int>& src,
                                                const AbstractTexture* src_tex, int src_layer,
                                                u32 native_width, u32 native_height)
{
  AbstractFramebuffer* const framebuffer = g_gfx->GetCurrentFramebuffer();
  if (framebuffer == nullptr)
    return;

  // Drive the real librashader filter chain when it was created successfully. If the chain is null
  // (no preset, preset failed, or a required device extension is missing) we skip straight to the
  // Task-4 passthrough copy so the screen never blanks. A framebuffer without a color attachment
  // likewise falls through to passthrough rather than dereferencing a null attachment.
  if (m_chain != nullptr && framebuffer->GetColorAttachment() != nullptr)
  {
    const libra_instance_t& lib = GetLibrashaderInstance();

    const auto* in_tex = static_cast<const VKTexture*>(src_tex);

    // librashader derives SourceSize/OriginalSize from the input image's dimensions, and CRT presets
    // (crt-royale, RetroCrisis) scale their scanline and phosphor-mask geometry by SourceSize. The
    // XFB source is at the internal (upscaled) resolution, so feeding it directly reports
    // SourceSize = internal res: the mask/scanline period shrinks with the IR multiplier (moire,
    // invisible scanlines) and every pass runs against the oversized frame. Worse, librashader's
    // single bilinear tap subsamples that upscaled frame, aliasing high-frequency content into the
    // NTSC/scanline bands. We instead materialize a REAL native-resolution source by box-averaging
    // the whole footprint (SSAA) for integer upscales, bilinear for fractional -- so the chain
    // computes geometry against native pixels while keeping supersampled detail. No-op at 1x or when
    // no native size is supplied.
    const VideoCommon::SlangSourceDownscalePlan plan = VideoCommon::PlanSlangSourceDownscale(
        in_tex->GetWidth(), in_tex->GetHeight(), native_width, native_height);
    const VKTexture* source = in_tex;
    if (plan.downscale)
    {
      if (const AbstractTexture* native =
              DownscaleToNativeSource(plan, src_tex, native_width, native_height))
      {
        source = static_cast<const VKTexture*>(native);
      }
      // The downscale draw left its own framebuffer bound; restore the caller's so that a
      // fall-through to the passthrough copy (on chain error) targets the screen, not the native RT.
      g_gfx->SetFramebuffer(framebuffer);
    }

    libra_image_vk_t in{source->GetImage(), source->GetVkFormat(), source->GetWidth(),
                        source->GetHeight()};

    // librashader derives OutputSize, FinalViewportSize, and every scale_type=viewport framebuffer
    // size from the OUTPUT IMAGE's dimensions, then merely scissors rendering to the viewport rect.
    // Handing it the full backbuffer plus a pillarboxed sub-rect would size the whole chain
    // (phosphor mask, scanline geometry, NTSC subcarrier) for the backbuffer width while the pixels
    // land in the narrower draw rect -- a fixed fractional mismatch that beats against the panel
    // pixel grid as vertical moire, independent of internal resolution. We instead render the chain
    // into a draw-rect-sized target at viewport origin (0,0) so OutputSize == the drawn extent, then
    // blit that 1:1 into the backbuffer at the draw rect. This mirrors how ARMSX2 drives librashader.
    // Records the whole chain with `target` as its output image (viewport = the full target).
    // libra_vk_filter_chain_frame() must NOT be recorded inside a render pass, and the header
    // requires the input in SHADER_READ_ONLY_OPTIMAL and the output in COLOR_ATTACHMENT_OPTIMAL.
    // (`source` is const; VKTexture::TransitionToLayout is const-qualified. When a downscale ran,
    // `source` is the native texture already left in SHADER_READ_ONLY_OPTIMAL; re-issuing the
    // transition is a no-op there.) librashader creates no barrier after its final pass, so on
    // success the target is left in COLOR_ATTACHMENT_OPTIMAL and Dolphin's tracking is reconciled.
    const auto run_chain = [&](VKTexture* target) -> bool {
      libra_image_vk_t out{target->GetImage(), target->GetVkFormat(), target->GetWidth(),
                           target->GetHeight()};
      libra_viewport_t vp{0.0f, 0.0f, target->GetWidth(), target->GetHeight()};

      StateTracker::GetInstance()->EndRenderPass();
      VkCommandBuffer cmd = g_command_buffer_mgr->GetCurrentCommandBuffer();
      source->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      target->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

      libra_error_t err = lib.vk_filter_chain_frame(&m_chain, cmd, m_frame_count++, in, out, &vp,
                                                    nullptr, nullptr);
      target->OverrideImageLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      return !CheckError(lib, err, "vk_filter_chain_frame");
    };

    auto* out_tex = static_cast<VKTexture*>(framebuffer->GetColorAttachment());
    if (VideoCommon::ShouldRenderChainDirectly(dst, framebuffer->GetWidth(),
                                               framebuffer->GetHeight()))
    {
      // The draw rect is the entire backbuffer, so OutputSize is identical whether the chain
      // targets the intermediate texture or the backbuffer itself: render straight into the
      // backbuffer and skip the extra full-frame write + read of the 1:1 blit. The chain's final
      // pass covers every backbuffer pixel, which also makes the clear BindBackbuffer deferred
      // redundant.
      StateTracker::GetInstance()->DiscardPendingClear();
      if (run_chain(out_tex))
        return;
      // On error, fall through to the passthrough copy; it covers the full rect, so the discarded
      // clear is not missed.
    }
    else
    {
      AbstractTexture* const chain_output =
          EnsureOutputTarget(static_cast<u32>(dst.GetWidth()),
                             static_cast<u32>(dst.GetHeight()), out_tex->GetFormat());
      if (chain_output != nullptr && run_chain(static_cast<VKTexture*>(chain_output)))
      {
        // Present the chain output 1:1 into the backbuffer draw rect. Point sampling keeps the copy
        // exact (target and rect are equal size).
        BuildPassthroughPipeline();
        if (m_passthrough_pipeline)
        {
          auto* chain_out_tex = static_cast<VKTexture*>(chain_output);
          chain_out_tex->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
}  // namespace Vulkan
