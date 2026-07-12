// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace VideoCommon
{
struct SlangParameter
{
  std::string id;
  std::string label;
  float default_value = 0.0f;
  float min_value = 0.0f;
  float max_value = 0.0f;
  float step = 0.0f;
};

struct SlangShaderSource
{
  std::string name;             // from #pragma name (may be empty)
  std::string format;           // from #pragma format (may be empty)
  std::string vertex_source;    // code selected by #pragma stage vertex + common prologue
  std::string fragment_source;  // code selected by #pragma stage fragment + common prologue
  std::vector<SlangParameter> parameters;
};

// Parses a .slang file's raw text. Lines before the first "#pragma stage" are
// common to both stages. Returns nullopt with *error on malformed input.
std::optional<SlangShaderSource> ParseSlangShader(const std::string& text, std::string* error);
}  // namespace VideoCommon
