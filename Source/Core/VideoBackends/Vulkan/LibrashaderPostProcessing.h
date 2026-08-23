// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "VideoCommon/PostProcessing/IPostProcessor.h"
#include "VideoCommon/PostProcessing/SlangSourceDownscale.h"

// librashader's opaque Vulkan filter-chain handle is `struct _filter_chain_vk *`
// (see Externals/librashader/include/librashader.h). Forward-declare it here so this
// header does not pull in librashader_ld.h, which defines many static-inline symbols and
// is therefore included in exactly one translation unit (LibrashaderPostProcessing.cpp).
struct _filter_chain_vk;

class AbstractShader;
class AbstractPipeline;
class AbstractTexture;
class AbstractFramebuffer;

namespace Vulkan
{
// Vulkan-backend post-processing engine backed by librashader (loaded at runtime via dlopen).
// This is the Task 4 skeleton: it loads the librashader instance, resolves the selected preset
// and creates the filter chain, but BlitFromTexture only performs a passthrough copy. The real
// per-frame libra_vk_filter_chain_frame() call arrives in a later task.
class LibrashaderPostProcessing final : public VideoCommon::IPostProcessor
{
public:
  LibrashaderPostProcessing();
  ~LibrashaderPostProcessing() override;

  // Lazily loads the librashader instance once and reports whether the shared library was found
  // and its ABI is compatible. Used by VKGfx::CreatePostProcessor() to decide whether to fall
  // back to the built-in engine. Safe to call before any instance exists.
  static bool IsAvailable();

  bool Initialize(AbstractTextureFormat format) override;
  void RecompileShader() override;
  void RecompilePipeline() override;
  void BlitFromTexture(const MathUtil::Rectangle<int>& dst, const MathUtil::Rectangle<int>& src,
                       const AbstractTexture* src_tex, int src_layer, u32 native_width,
                       u32 native_height) override;

private:
  // Builds (or rebuilds, on framebuffer-format change) the fullscreen-triangle copy pipeline used
  // for passthrough rendering. Mirrors MultipassPostProcessing::BuildPassthroughPipeline().
  void BuildPassthroughPipeline();

  // Renders the internally-upscaled source down to a native-resolution texture per `plan`, so the
  // filter chain derives its geometry from native pixels and the discarded upscale detail becomes
  // supersampling (box) rather than aliasing (single bilinear tap). Returns the native-res source
  // texture, left in SHADER_READ_ONLY_OPTIMAL, or nullptr on allocation/pipeline failure.
  const AbstractTexture* DownscaleToNativeSource(const VideoCommon::SlangSourceDownscalePlan& plan,
                                                 const AbstractTexture* src_tex, u32 native_width,
                                                 u32 native_height);

  // (Re)builds the downscale pipeline. The box pixel shader bakes the factor as a literal, so it is
  // rebuilt whenever the factor, filter kind (box vs bilinear), or color format changes.
  void BuildDownscalePipeline(const VideoCommon::SlangSourceDownscalePlan& plan,
                              AbstractTextureFormat format);

  // (Re)allocates the draw-rect-sized target the filter chain renders into (at viewport origin
  // 0,0), so librashader's OutputSize/FinalViewportSize and every scale_type=viewport pass match
  // the actually-drawn extent instead of the full backbuffer. Returns nullptr on allocation
  // failure. See BlitFromTexture for why this matters (vertical-moire root cause).
  AbstractTexture* EnsureOutputTarget(u32 width, u32 height, AbstractTextureFormat format);

  // librashader Vulkan filter chain; null means "passthrough" (creation failed or no preset).
  _filter_chain_vk* m_chain = nullptr;

  AbstractTextureFormat m_format = AbstractTextureFormat::Undefined;
  size_t m_frame_count = 0;
  bool m_available = false;

  // Passthrough copy resources.
  std::unique_ptr<AbstractShader> m_passthrough_vertex;
  std::unique_ptr<AbstractShader> m_passthrough_pixel;
  std::unique_ptr<AbstractPipeline> m_passthrough_pipeline;
  AbstractTextureFormat m_passthrough_format = AbstractTextureFormat::Undefined;

  // Native-resolution source produced by downscaling the upscaled frame before the chain runs.
  std::unique_ptr<AbstractTexture> m_native_source;
  std::unique_ptr<AbstractFramebuffer> m_native_source_fb;
  u32 m_native_source_width = 0;
  u32 m_native_source_height = 0;
  AbstractTextureFormat m_native_source_format = AbstractTextureFormat::Undefined;

  // Downscale pipeline. m_downscale_factor caches the box factor the current pixel shader was baked
  // for (0 = bilinear fallback); the shader is rebuilt only when the factor/kind or format changes.
  std::unique_ptr<AbstractShader> m_downscale_vertex;
  std::unique_ptr<AbstractShader> m_downscale_pixel;
  std::unique_ptr<AbstractPipeline> m_downscale_pipeline;
  u32 m_downscale_factor = 0;
  bool m_downscale_is_box = false;
  AbstractTextureFormat m_downscale_format = AbstractTextureFormat::Undefined;

  // Draw-rect-sized target the filter chain renders into, then blitted 1:1 into the backbuffer.
  std::unique_ptr<AbstractTexture> m_output_target;
  u32 m_output_target_width = 0;
  u32 m_output_target_height = 0;
  AbstractTextureFormat m_output_target_format = AbstractTextureFormat::Undefined;
};
}  // namespace Vulkan
