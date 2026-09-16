// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace VideoCommon
{
// GFX_ENHANCE_POST_SHADER holds a chain of preset paths separated by ';'. These helpers are the
// single source of truth for how that string is built, displayed and filtered, shared by the Qt
// and Android front ends.
constexpr char CHAIN_SEPARATOR = ';';

// The chain's entries, in order, with empty entries dropped.
std::vector<std::string> SplitChainSpec(std::string_view spec);

// Inverse of SplitChainSpec.
std::string JoinChainSpec(const std::vector<std::string>& presets);

// Human-readable rendering: entries joined with " -> " (U+2192). Empty for an empty chain; the
// caller supplies its own localized "off" label.
std::string DescribeChainSpec(std::string_view spec);

// Appends one preset to the chain. Appending an empty preset returns the chain unchanged.
std::string AppendToChainSpec(std::string_view spec, std::string_view preset);

// The leading directory of a preset path, or "" for a preset at the shaders root.
std::string ChainCategoryOf(std::string_view preset);

// Distinct, sorted, non-empty categories present in `presets`.
std::vector<std::string> ChainCategories(const std::vector<std::string>& presets);

// The subset of `presets` in `category`, preserving input order. An empty category returns
// everything.
std::vector<std::string> PresetsInCategory(const std::vector<std::string>& presets,
                                           std::string_view category);
}  // namespace VideoCommon
