// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>

namespace VideoCommon
{
std::string RetroCrisisProfileOf(const std::string& preset_rel_path);

std::set<std::string> ComputeRetroCrisisClosure(
    const std::string& chosen_profile,
    const std::map<std::string, std::set<std::string>>& profile_references);
}  // namespace VideoCommon
