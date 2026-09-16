// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/PostProcessing/SlangPreset.h"
#include "VideoCommon/PostProcessing/SlangSamplers.h"
#include "VideoCommon/RenderState.h"

using namespace VideoCommon;

TEST(SlangSamplers, LinearRepeat)
{
  const SamplerState s = MakeSlangSamplerState(SlangWrapMode::Repeat, true, false);
  EXPECT_EQ(s.tm0.min_filter, FilterMode::Linear);
  EXPECT_EQ(s.tm0.mag_filter, FilterMode::Linear);
  EXPECT_EQ(s.tm0.wrap_u, WrapMode::Repeat);
  EXPECT_EQ(s.tm0.wrap_v, WrapMode::Repeat);
}

TEST(SlangSamplers, PointClampNoMips)
{
  const SamplerState s = MakeSlangSamplerState(SlangWrapMode::ClampToEdge, false, false);
  EXPECT_EQ(s.tm0.min_filter, FilterMode::Near);
  EXPECT_EQ(s.tm0.wrap_u, WrapMode::Clamp);
  // No mips -> lod range collapsed to 0.
  EXPECT_EQ(s.tm1.min_lod, 0u);
  EXPECT_EQ(s.tm1.max_lod, 0u);
}

TEST(SlangSamplers, MipmappedHasLodRange)
{
  const SamplerState s = MakeSlangSamplerState(SlangWrapMode::Repeat, true, true);
  EXPECT_GT(s.tm1.max_lod, 0u);
}

TEST(SlangSamplers, BorderMapsToClamp)
{
  const SamplerState s = MakeSlangSamplerState(SlangWrapMode::ClampToBorder, true, false);
  EXPECT_EQ(s.tm0.wrap_u, WrapMode::Clamp);  // border unsupported -> clamp
}

TEST(SlangSamplers, MirroredRepeatMapsToMirror)
{
  const SamplerState s = MakeSlangSamplerState(SlangWrapMode::MirroredRepeat, true, false);
  EXPECT_EQ(s.tm0.wrap_u, WrapMode::Mirror);
}

TEST(SlangSamplers, TranslatorCeilingMatchesBackendSamplerLimit)
{
  // The translator's MAX_SAMPLERS ceiling and the backends' descriptor-range sizes are the same
  // number by contract. D3D12's utility root signature declared 8 while the translator emitted up
  // to 16, which silently blanked crt-royale (UAT finding 1).
  EXPECT_EQ(VideoCommon::MAX_PIXEL_SHADER_SAMPLERS, 16u);
}
