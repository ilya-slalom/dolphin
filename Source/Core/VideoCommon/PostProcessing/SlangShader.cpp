// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/SlangShader.h"

#include <cstdlib>
#include <sstream>
#include <string_view>

namespace VideoCommon
{
namespace
{
std::string_view Trim(std::string_view s)
{
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

bool StartsWith(std::string_view s, std::string_view prefix)
{
  return s.substr(0, prefix.size()) == prefix;
}

// Parses `#pragma parameter <id> "<label>" <def> <min> <max> [step]`.
SlangParameter ParseParameter(std::string_view rest)
{
  SlangParameter param;

  rest = Trim(rest);
  const auto id_end = rest.find_first_of(" \t");
  if (id_end == std::string_view::npos)
  {
    param.id = std::string(rest);
    return param;
  }
  param.id = std::string(rest.substr(0, id_end));
  rest = Trim(rest.substr(id_end));

  // Quoted label.
  if (!rest.empty() && rest.front() == '"')
  {
    const auto close = rest.find('"', 1);
    if (close != std::string_view::npos)
    {
      param.label = std::string(rest.substr(1, close - 1));
      rest = Trim(rest.substr(close + 1));
    }
  }

  // Up to four whitespace-separated floats: default, min, max, step.
  float* targets[] = {&param.default_value, &param.min_value, &param.max_value, &param.step};
  size_t index = 0;
  std::string remaining(rest);
  std::istringstream stream(remaining);
  std::string token;
  while (index < 4 && (stream >> token))
  {
    char* parse_end = nullptr;
    const float value = std::strtof(token.c_str(), &parse_end);
    if (parse_end != token.c_str())
      *targets[index] = value;
    ++index;
  }
  return param;
}
}  // namespace

std::optional<SlangShaderSource> ParseSlangShader(const std::string& text, std::string* error)
{
  SlangShaderSource shader;

  enum class Stage
  {
    None,
    Vertex,
    Fragment
  };
  Stage stage = Stage::None;

  std::string prologue;
  std::string vertex_body;
  std::string fragment_body;

  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line))
  {
    std::string_view view = line;
    if (!view.empty() && view.back() == '\r')
      view.remove_suffix(1);

    const std::string_view trimmed = Trim(view);
    if (StartsWith(trimmed, "#pragma "))
    {
      const std::string_view rest = Trim(trimmed.substr(std::string_view("#pragma ").size()));
      if (StartsWith(rest, "name "))
      {
        shader.name = std::string(Trim(rest.substr(std::string_view("name ").size())));
        continue;
      }
      if (StartsWith(rest, "format "))
      {
        shader.format = std::string(Trim(rest.substr(std::string_view("format ").size())));
        continue;
      }
      if (StartsWith(rest, "parameter "))
      {
        shader.parameters.push_back(
            ParseParameter(rest.substr(std::string_view("parameter ").size())));
        continue;
      }
      if (StartsWith(rest, "stage "))
      {
        const std::string_view which = Trim(rest.substr(std::string_view("stage ").size()));
        if (which == "vertex")
          stage = Stage::Vertex;
        else if (which == "fragment")
          stage = Stage::Fragment;
        continue;
      }
      // Unknown pragma: keep it in the active buffer so glslang can see it.
    }

    // Re-append the original line (with its own trailing newline) to the active buffer.
    std::string emitted(view);
    emitted += '\n';
    switch (stage)
    {
    case Stage::None:
      prologue += emitted;
      break;
    case Stage::Vertex:
      vertex_body += emitted;
      break;
    case Stage::Fragment:
      fragment_body += emitted;
      break;
    }
  }

  if (vertex_body.empty() || fragment_body.empty())
  {
    if (error != nullptr)
      *error = "shader is missing a #pragma stage vertex or fragment body";
    return std::nullopt;
  }

  shader.vertex_source = prologue + vertex_body;
  shader.fragment_source = prologue + fragment_body;
  return shader;
}
}  // namespace VideoCommon
