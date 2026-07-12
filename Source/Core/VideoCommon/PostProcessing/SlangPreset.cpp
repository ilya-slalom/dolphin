// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/SlangPreset.h"

#include <cstdlib>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>

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

// Strips a single pair of surrounding double quotes, if present.
std::string_view Unquote(std::string_view s)
{
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

// Splits "key = value" on the first '='. Returns false if no '=' present.
bool SplitKeyValue(std::string_view line, std::string* key, std::string* value)
{
  const auto eq = line.find('=');
  if (eq == std::string_view::npos)
    return false;
  *key = std::string(Trim(line.substr(0, eq)));
  *value = std::string(Unquote(Trim(line.substr(eq + 1))));
  return true;
}

// Lexically normalizes a POSIX-style path, collapsing "." and ".." segments without
// touching the filesystem. Preserves a leading "/".
std::string NormalizePath(const std::string& path)
{
  const bool absolute = !path.empty() && path.front() == '/';
  std::vector<std::string_view> parts;
  std::string_view view = path;
  size_t start = 0;
  while (start <= view.size())
  {
    const auto slash = view.find('/', start);
    const auto end = slash == std::string_view::npos ? view.size() : slash;
    const std::string_view seg = view.substr(start, end - start);
    if (seg.empty() || seg == ".")
    {
      // skip empty and current-dir segments
    }
    else if (seg == "..")
    {
      if (!parts.empty() && parts.back() != "..")
        parts.pop_back();
      else if (!absolute)
        parts.push_back(seg);
    }
    else
    {
      parts.push_back(seg);
    }
    if (slash == std::string_view::npos)
      break;
    start = end + 1;
  }

  std::string result = absolute ? "/" : "";
  for (size_t i = 0; i < parts.size(); ++i)
  {
    if (i != 0)
      result += '/';
    result += std::string(parts[i]);
  }
  return result;
}

std::string ResolvePath(const std::string& base_dir, const std::string& value)
{
  return NormalizePath(base_dir + "/" + value);
}

bool ParseBool(const std::string& value)
{
  return value == "true" || value == "1";
}

ScaleType ParseScaleType(const std::string& value)
{
  if (value == "viewport")
    return ScaleType::Viewport;
  if (value == "absolute")
    return ScaleType::Absolute;
  return ScaleType::Source;
}

SlangWrapMode ParseWrapMode(const std::string& value)
{
  if (value == "repeat")
    return SlangWrapMode::Repeat;
  if (value == "mirrored_repeat")
    return SlangWrapMode::MirroredRepeat;
  if (value == "clamp_to_edge")
    return SlangWrapMode::ClampToEdge;
  return SlangWrapMode::ClampToBorder;
}

float ParseFloat(const std::string& value, float fallback)
{
  const std::string trimmed(Trim(value));
  if (trimmed.empty())
    return fallback;
  char* parse_end = nullptr;
  const float out = std::strtof(trimmed.c_str(), &parse_end);
  if (parse_end == trimmed.c_str())
    return fallback;
  return out;
}

using KeyMap = std::map<std::string, std::string>;

const std::string* Find(const KeyMap& map, const std::string& key)
{
  const auto it = map.find(key);
  return it == map.end() ? nullptr : &it->second;
}

std::vector<std::string> SplitList(const std::string& value, char delim)
{
  std::vector<std::string> out;
  std::string_view view = value;
  size_t start = 0;
  while (start <= view.size())
  {
    const auto pos = view.find(delim, start);
    const auto end = pos == std::string_view::npos ? view.size() : pos;
    const std::string_view seg = Trim(view.substr(start, end - start));
    if (!seg.empty())
      out.emplace_back(seg);
    if (pos == std::string_view::npos)
      break;
    start = end + 1;
  }
  return out;
}
}  // namespace

std::optional<SlangPresetConfig> ParseSlangPreset(const std::string& text,
                                                  const std::string& base_dir, std::string* error)
{
  KeyMap map;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line))
  {
    std::string_view view = line;
    if (!view.empty() && view.back() == '\r')
      view.remove_suffix(1);
    view = Trim(view);
    if (view.empty() || view.front() == '#')
      continue;

    std::string key, value;
    if (SplitKeyValue(view, &key, &value))
      map[key] = value;
  }

  const std::string* shaders_value = Find(map, "shaders");
  if (shaders_value == nullptr)
  {
    if (error != nullptr)
      *error = "missing 'shaders' count";
    return std::nullopt;
  }

  long pass_count = 0;
  {
    const std::string trimmed(Trim(*shaders_value));
    char* parse_end = nullptr;
    pass_count = std::strtol(trimmed.c_str(), &parse_end, 10);
    if (trimmed.empty() || parse_end == trimmed.c_str() || pass_count < 0)
    {
      if (error != nullptr)
        *error = "invalid 'shaders' count";
      return std::nullopt;
    }
  }

  SlangPresetConfig config;
  config.passes.reserve(static_cast<size_t>(pass_count));

  for (long i = 0; i < pass_count; ++i)
  {
    const std::string idx = std::to_string(i);
    const std::string* shader = Find(map, "shader" + idx);
    if (shader == nullptr)
    {
      if (error != nullptr)
        *error = "missing 'shader" + idx + "'";
      return std::nullopt;
    }

    SlangPassConfig pass;
    pass.shader_path = ResolvePath(base_dir, *shader);
    if (const std::string* v = Find(map, "alias" + idx))
      pass.alias = *v;
    if (const std::string* v = Find(map, "filter_linear" + idx))
      pass.filter_linear = ParseBool(*v);
    if (const std::string* v = Find(map, "wrap_mode" + idx))
      pass.wrap_mode = ParseWrapMode(*v);
    if (const std::string* v = Find(map, "mipmap_input" + idx))
      pass.mipmap_input = ParseBool(*v);
    if (const std::string* v = Find(map, "srgb_framebuffer" + idx))
      pass.srgb_framebuffer = ParseBool(*v);
    if (const std::string* v = Find(map, "float_framebuffer" + idx))
      pass.float_framebuffer = ParseBool(*v);

    // Scale: axis-specific keys win; fall back to combined scale_type/scale for both axes.
    const std::string* combined_type = Find(map, "scale_type" + idx);
    const std::string* combined_scale = Find(map, "scale" + idx);
    const std::string* type_x = Find(map, "scale_type_x" + idx);
    const std::string* type_y = Find(map, "scale_type_y" + idx);
    const std::string* scale_x = Find(map, "scale_x" + idx);
    const std::string* scale_y = Find(map, "scale_y" + idx);

    if (type_x != nullptr)
      pass.scale_type_x = ParseScaleType(*type_x);
    else if (combined_type != nullptr)
      pass.scale_type_x = ParseScaleType(*combined_type);

    if (type_y != nullptr)
      pass.scale_type_y = ParseScaleType(*type_y);
    else if (combined_type != nullptr)
      pass.scale_type_y = ParseScaleType(*combined_type);

    if (scale_x != nullptr)
      pass.scale_x = ParseFloat(*scale_x, 1.0f);
    else if (combined_scale != nullptr)
      pass.scale_x = ParseFloat(*combined_scale, 1.0f);

    if (scale_y != nullptr)
      pass.scale_y = ParseFloat(*scale_y, 1.0f);
    else if (combined_scale != nullptr)
      pass.scale_y = ParseFloat(*combined_scale, 1.0f);

    config.passes.push_back(std::move(pass));
  }

  // Textures (LUTs): "textures = A;B;C" then per-name keys.
  if (const std::string* textures = Find(map, "textures"))
  {
    for (const std::string& name : SplitList(*textures, ';'))
    {
      const std::string* path = Find(map, name);
      if (path == nullptr)
        continue;

      SlangLutConfig lut;
      lut.name = name;
      lut.path = ResolvePath(base_dir, *path);
      if (const std::string* v = Find(map, name + "_wrap_mode"))
        lut.wrap_mode = ParseWrapMode(*v);
      if (const std::string* v = Find(map, name + "_linear"))
        lut.linear = ParseBool(*v);
      if (const std::string* v = Find(map, name + "_mipmap"))
        lut.mipmap = ParseBool(*v);
      config.luts.push_back(std::move(lut));
    }
  }

  return config;
}
}  // namespace VideoCommon
