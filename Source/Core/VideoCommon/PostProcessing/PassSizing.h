// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/PostProcessing/SlangPreset.h"

namespace VideoCommon
{
struct PassSizeInputs
{
  u32 source_width;    // previous pass output (or pipeline input for pass 0)
  u32 source_height;
  u32 viewport_width;  // final on-screen target
  u32 viewport_height;
};

// Computes one axis. scale_type Source: round(source * scale); Viewport: round(viewport * scale);
// Absolute: round(scale). Result is clamped to >= 1.
u32 ComputePassAxisSize(ScaleType type, float scale, u32 source, u32 viewport);
}  // namespace VideoCommon
