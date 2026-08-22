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
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"

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

  // vk_filter_chain_create invalidates the preset handle regardless of success or failure (see the
  // header: "the shader preset is immediately invalidated"), so it must not be freed on either path
  // -- doing so would be a double-free. On error, leave m_chain null (passthrough).
  if (CheckError(lib, lib.vk_filter_chain_create(&preset, device, nullptr, &m_chain),
                 "vk_filter_chain_create"))
  {
    m_chain = nullptr;
    return;
  }

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

    // librashader derives SourceSize/OriginalSize from the dimensions we declare here, and CRT
    // presets (crt-royale, RetroCrisis) scale their scanline and phosphor-mask geometry by
    // SourceSize. Dolphin's XFB source texture is at the internal (upscaled) resolution, so
    // declaring its full extent would report SourceSize = internal res -> the mask/scanline period
    // shrinks with the internal-resolution multiplier (moire, invisible scanlines) and every pass
    // runs against the oversized source (slowdown at high IR). We instead declare the NATIVE
    // resolution while still binding the full-resolution image: librashader samples the bound image
    // with normalized coordinates, so it keeps the supersampled detail but computes geometry against
    // native pixels -- identical to MultipassPostProcessing's native-SourceSize handling. Fall back
    // to the texture extent for any path that does not supply a native size.
    const uint32_t source_width = native_width != 0 ? native_width : in_tex->GetWidth();
    const uint32_t source_height = native_height != 0 ? native_height : in_tex->GetHeight();
    libra_image_vk_t in{in_tex->GetImage(), in_tex->GetVkFormat(), source_width, source_height};

    auto* out_tex = static_cast<VKTexture*>(framebuffer->GetColorAttachment());
    libra_image_vk_t out{out_tex->GetImage(), out_tex->GetVkFormat(), out_tex->GetWidth(),
                         out_tex->GetHeight()};

    libra_viewport_t vp{static_cast<float>(dst.left), static_cast<float>(dst.top),
                        static_cast<uint32_t>(dst.GetWidth()),
                        static_cast<uint32_t>(dst.GetHeight())};

    // libra_vk_filter_chain_frame() must NOT be recorded inside a render pass.
    StateTracker::GetInstance()->EndRenderPass();

    // The header requires the input in SHADER_READ_ONLY_OPTIMAL and the output in
    // COLOR_ATTACHMENT_OPTIMAL before the call. No barrier is created for the final pass, so the
    // output remains in COLOR_ATTACHMENT_OPTIMAL afterward and the caller owns any later
    // transition. (in_tex is const; VKTexture::TransitionToLayout is const-qualified.)
    VkCommandBuffer cmd = g_command_buffer_mgr->GetCurrentCommandBuffer();
    in_tex->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    out_tex->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    libra_error_t err =
        lib.vk_filter_chain_frame(&m_chain, cmd, m_frame_count++, in, out, &vp, nullptr, nullptr);
    if (!CheckError(lib, err, "vk_filter_chain_frame"))
    {
      // librashader left the output in COLOR_ATTACHMENT_OPTIMAL; reconcile Dolphin's tracking.
      out_tex->OverrideImageLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      return;
    }
    // On error, fall through to the passthrough copy for this frame so the screen never blanks.
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
