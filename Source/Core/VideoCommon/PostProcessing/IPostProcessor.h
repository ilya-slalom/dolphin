// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/TextureConfig.h"

class AbstractTexture;

namespace VideoCommon
{
// Backend-agnostic post-processing engine interface. Presenter owns one of these and drives it
// per frame. Implemented by MultipassPostProcessing (all backends) and, on Vulkan, by
// LibrashaderPostProcessing.
class IPostProcessor
{
public:
  virtual ~IPostProcessor() = default;

  virtual bool Initialize(AbstractTextureFormat format) = 0;
  virtual void RecompileShader() = 0;
  virtual void RecompilePipeline() = 0;
  virtual void BlitFromTexture(const MathUtil::Rectangle<int>& dst,
                               const MathUtil::Rectangle<int>& src,
                               const AbstractTexture* src_tex, int src_layer,
                               u32 native_width, u32 native_height) = 0;
};
}  // namespace VideoCommon
