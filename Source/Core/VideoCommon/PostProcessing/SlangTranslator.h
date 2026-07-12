// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "VideoCommon/PostProcessing/SlangShader.h"

class AbstractShader;

namespace VideoCommon
{
// The set of texture samplers a pass reads, in binding order (index 0..N-1).
// Includes "Source", "Original", each referenced alias, and each referenced LUT.
struct TranslatedPass
{
  std::string vertex_glsl;    // ready for g_gfx->CreateShaderFromSource(Vertex, ...)
  std::string fragment_glsl;  // ready for CreateShaderFromSource(Pixel, ...)
  std::vector<std::string> sampler_names;  // binding index -> semantic/alias/LUT name
  bool ok = false;
  std::string error;  // set when ok == false (e.g. > 8 samplers)
};

// known_aliases: names produced by earlier passes; lut_names: declared LUTs.
TranslatedPass TranslateSlangPass(const SlangShaderSource& shader,
                                  const std::vector<std::string>& known_aliases,
                                  const std::vector<std::string>& lut_names);

struct CompiledPassShaders
{
  std::unique_ptr<AbstractShader> vertex;
  std::unique_ptr<AbstractShader> pixel;
};

// Compiles a translated pass. include_dir roots the #include resolver (the directory of the
// .slang file); the Sys shaders dir is added as the system include root. Returns empty uniques
// on failure.
CompiledPassShaders CompileTranslatedPass(const TranslatedPass& pass,
                                          const std::string& include_dir);
}  // namespace VideoCommon
