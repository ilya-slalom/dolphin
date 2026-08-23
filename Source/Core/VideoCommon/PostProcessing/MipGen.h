// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "Common/CommonTypes.h"

namespace VideoCommon
{
struct MipLevel
{
  u32 width;
  u32 height;
  std::vector<u8> rgba8;  // width*height*4
};

// level0 is the full-res RGBA8 image (width*height*4). Returns level0 plus successive
// half-size box-filtered levels down to 1x1.
std::vector<MipLevel> GenerateBoxMips(u32 width, u32 height, const u8* rgba8);
}  // namespace VideoCommon
