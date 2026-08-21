// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/RetroCrisisInstall.h"

#include <deque>

namespace VideoCommon
{
namespace
{
constexpr char kRoot[] = "retro crisis/";
}  // namespace

std::string RetroCrisisProfileOf(const std::string& preset_rel_path)
{
  const std::size_t root_len = std::string(kRoot).size();
  if (preset_rel_path.size() <= root_len || preset_rel_path.compare(0, root_len, kRoot) != 0)
    return {};
  const std::size_t next = preset_rel_path.find('/', root_len);
  if (next == std::string::npos)
    return {};
  return preset_rel_path.substr(root_len, next - root_len);
}

std::set<std::string> ComputeRetroCrisisClosure(
    const std::string& chosen_profile,
    const std::map<std::string, std::set<std::string>>& profile_references)
{
  std::set<std::string> closure;
  std::deque<std::string> work = {chosen_profile};
  while (!work.empty())
  {
    const std::string profile = work.front();
    work.pop_front();
    if (!closure.insert(profile).second)
      continue;
    const auto it = profile_references.find(profile);
    if (it == profile_references.end())
      continue;
    for (const std::string& ref : it->second)
      work.push_back(ref);
  }
  return closure;
}
}  // namespace VideoCommon
