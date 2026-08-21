// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/ShaderPackSource.h"

namespace VideoCommon
{
const std::vector<ShaderPackSource>& GetShaderPackSources()
{
  static const std::vector<ShaderPackSource> sources = {
      {"libretro", "libretro slang shaders",
       "https://buildbot.libretro.com/assets/frontend/shaders_slang.zip",
       /*extract_subpath=*/"", /*install_subdir=*/"shaders_slang", /*depends_on=*/{}},
      {"satpixie", "CRT-SatPixie",
       "https://github.com/Conkwer/satpixie-crt-shader/releases/download/20260122/"
       "satpixie-crt-shader-20260122.zip",
       /*extract_subpath=*/"satpixie-crt-shader/RetroArch/shaders/shaders_slang/",
       /*install_subdir=*/"shaders_slang", /*depends_on=*/{}},
  };
  return sources;
}

const ShaderPackSource* FindShaderPackSource(std::string_view id)
{
  for (const ShaderPackSource& source : GetShaderPackSources())
  {
    if (source.id == id)
      return &source;
  }
  return nullptr;
}
}  // namespace VideoCommon
