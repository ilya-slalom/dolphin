// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "VideoCommon/PostProcessing/IPostProcessor.h"

// librashader's opaque Vulkan filter-chain handle is `struct _filter_chain_vk *`
// (see Externals/librashader/include/librashader.h). Forward-declare it here so this
// header does not pull in librashader_ld.h, which defines many static-inline symbols and
// is therefore included in exactly one translation unit (LibrashaderPostProcessing.cpp).
struct _filter_chain_vk;

class AbstractShader;
class AbstractPipeline;

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
};
}  // namespace Vulkan
