// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/SlangSourceDownscale.h"

namespace VideoCommon
{
SlangSourceDownscalePlan PlanSlangSourceDownscale(u32 src_w, u32 src_h, u32 native_w, u32 native_h)
{
  SlangSourceDownscalePlan plan;

  // No native size to target, or the source is not larger than native on either axis: pass through.
  if (native_w == 0 || native_h == 0)
    return plan;
  if (src_w <= native_w && src_h <= native_h)
    return plan;

  plan.downscale = true;

  // The box filter takes a single integer factor and averages the whole factor x factor footprint,
  // so it only applies when both axes reduce by the same exact whole multiple (>= 2).
  const u32 fx = src_w / native_w;
  const u32 fy = src_h / native_h;
  if (fx >= 2 && fx == fy && fx * native_w == src_w && fy * native_h == src_h)
  {
    plan.box_filter = true;
    plan.factor = fx;
  }

  return plan;
}
}  // namespace VideoCommon
