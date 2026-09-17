// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/PostProcessing/LibrashaderLoader.h"

class AbstractFramebuffer;
class AbstractTexture;

namespace VideoCommon
{
// One graphics backend's binding to a librashader native runtime.
//
// LibrashaderPostProcessing owns everything that is the same on every backend: preset
// resolution, the native-resolution source downscale, output sizing, the frame counter and the
// passthrough fallback. A runtime only has to create and free a chain and record one frame.
//
// The target is an AbstractFramebuffer rather than an AbstractTexture because D3D11's chain
// entry point takes an ID3D11RenderTargetView, and in Dolphin the RTV lives on DXFramebuffer
// while the SRV lives on DXTexture. Every other backend can reach its texture through
// GetColorAttachment().
class LibrashaderRuntime
{
public:
  virtual ~LibrashaderRuntime() = default;

  // True when every libra_<api>_* symbol this runtime needs resolved. Checked before a chain is
  // built, so an incomplete library degrades to the built-in executor rather than to a black
  // screen.
  virtual bool IsSupported() const = 0;

  // Builds a chain from `preset`. librashader invalidates the preset handle on BOTH success and
  // failure ("the shader preset is immediately invalidated"), so implementations must never free
  // it -- that would be a double free. Returns false and logs on failure.
  virtual bool CreateChain(libra_shader_preset_t preset) = 0;
  virtual void DestroyChain() = 0;
  virtual bool HasChain() const = 0;

  // Records the whole chain, reading `source` and writing `target` over its full extent with the
  // viewport at (0,0). Called outside any render pass. Implementations must transition `source`
  // to shader-read and `target` to render-target themselves, and must leave Dolphin's own
  // pipeline, descriptor and layout tracking consistent afterwards -- librashader binds its own
  // state and does not restore Dolphin's.
  virtual bool RunFrame(const AbstractTexture* source, AbstractFramebuffer* target,
                        u64 frame_count) = 0;

  // Pushes a #pragma parameter override into the live chain. No-op without a chain.
  virtual void SetParameter(const char* name, float value) = 0;

  // Called before RunFrame() when the chain writes straight into the backbuffer, so a backend
  // with a deferred clear can drop it: the chain's final pass covers every pixel.
  virtual void DiscardPendingTargetClear() {}
};
}  // namespace VideoCommon
