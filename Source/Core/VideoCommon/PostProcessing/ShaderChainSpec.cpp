// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/ShaderChainSpec.h"

#include <algorithm>
#include <set>

namespace VideoCommon
{
namespace
{
constexpr std::string_view ARROW = " \xE2\x86\x92 ";  // U+2192 RIGHTWARDS ARROW

// A hand-edited GFX.ini can contain "crt/a.slangp ; misc/b.slangp"; the surrounding spaces are not
// part of a preset name.
std::string_view Trim(std::string_view s)
{
  const auto first = s.find_first_not_of(" \t");
  if (first == std::string_view::npos)
    return {};
  return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}
}  // namespace

std::vector<std::string> SplitChainSpec(std::string_view spec)
{
  std::vector<std::string> presets;
  size_t start = 0;
  while (start <= spec.size())
  {
    const auto sep = spec.find(CHAIN_SEPARATOR, start);
    const auto end = sep == std::string_view::npos ? spec.size() : sep;
    const std::string_view entry = Trim(spec.substr(start, end - start));
    if (!entry.empty())
      presets.emplace_back(entry);
    if (sep == std::string_view::npos)
      break;
    start = end + 1;
  }
  return presets;
}

std::string JoinChainSpec(const std::vector<std::string>& presets)
{
  std::string spec;
  for (const std::string& preset : presets)
  {
    if (!spec.empty())
      spec += CHAIN_SEPARATOR;
    spec += preset;
  }
  return spec;
}

std::string DescribeChainSpec(std::string_view spec)
{
  std::string description;
  for (const std::string& preset : SplitChainSpec(spec))
  {
    if (!description.empty())
      description += ARROW;
    description += preset;
  }
  return description;
}

std::string AppendToChainSpec(std::string_view spec, std::string_view preset)
{
  if (preset.empty())
    return std::string(spec);
  std::vector<std::string> presets = SplitChainSpec(spec);
  presets.emplace_back(preset);
  return JoinChainSpec(presets);
}

std::string ChainCategoryOf(std::string_view preset)
{
  const auto slash = preset.find('/');
  return slash == std::string_view::npos ? std::string() : std::string(preset.substr(0, slash));
}

std::vector<std::string> ChainCategories(const std::vector<std::string>& presets)
{
  std::set<std::string> categories;
  for (const std::string& preset : presets)
  {
    std::string category = ChainCategoryOf(preset);
    if (!category.empty())
      categories.insert(std::move(category));
  }
  return {categories.begin(), categories.end()};
}

std::vector<std::string> PresetsInCategory(const std::vector<std::string>& presets,
                                           std::string_view category)
{
  if (category.empty())
    return presets;
  std::vector<std::string> filtered;
  for (const std::string& preset : presets)
  {
    if (ChainCategoryOf(preset) == category)
      filtered.push_back(preset);
  }
  return filtered;
}
}  // namespace VideoCommon
