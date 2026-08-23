// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/PostProcessing/SlangPreset.h"

namespace VideoCommon
{
std::string RetroCrisisProfileOf(const std::string& preset_rel_path);

std::set<std::string> ComputeRetroCrisisClosure(
    const std::string& chosen_profile,
    const std::map<std::string, std::set<std::string>>& profile_references);

std::map<std::string, std::set<std::string>> ScanRetroCrisisReferences(
    const std::vector<std::string>& preset_rel_paths, const std::string& extract_root,
    const SlangPresetReader& reader);

u32 InstallRetroCrisisProfile(const std::string& extract_root, const std::string& install_root,
                              const std::string& chosen_profile);

std::string ReadRetroCrisisProfile(const std::string& install_root);

// True if a discovered preset path should be hidden from the picker because it lives in a
// RetroCrisis profile folder other than the manifest's chosen profile. install_root is
// <Shaders>/RetroCrisis. Presets outside the RetroCrisis tree are never hidden.
bool IsHiddenRetroCrisisPreset(const std::string& preset_abs_path, const std::string& install_root,
                               const std::string& chosen_profile);
}  // namespace VideoCommon
