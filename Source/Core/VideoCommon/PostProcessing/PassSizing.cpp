// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/PassSizing.h"

#include <algorithm>
#include <cmath>

namespace VideoCommon
{
u32 ComputePassAxisSize(ScaleType type, float scale, u32 source, u32 viewport)
{
  float base = 0.0f;
  switch (type)
  {
  case ScaleType::Source:
    base = static_cast<float>(source) * scale;
    break;
  case ScaleType::Viewport:
    base = static_cast<float>(viewport) * scale;
    break;
  case ScaleType::Absolute:
    base = scale;
    break;
  }
  const long rounded = std::lround(base);
  return static_cast<u32>(std::max<long>(1, rounded));
}
}  // namespace VideoCommon
