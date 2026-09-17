// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "Common/FileUtil.h"
#include "VideoCommon/PostProcessing/LibrashaderParameters.h"

using namespace VideoCommon::LibrashaderParameters;

TEST(LibrashaderParameters, DecimalsForStepUsesFormulaNotTable)
{
  // PCSX2's logarithmic formula, not a lookup table. The table cannot work: the shader pack
  // declares 73 distinct step values; a six-entry table leaves 67 undefined.
  EXPECT_EQ(DecimalsForStep(0.0f), 3);
  EXPECT_EQ(DecimalsForStep(-1.0f), 3);
  EXPECT_EQ(DecimalsForStep(1.0f), 0);
  EXPECT_EQ(DecimalsForStep(0.5f), 1);   // ceil(-log10(0.5)) = ceil(0.301...) = 1
  EXPECT_EQ(DecimalsForStep(0.05f), 2);  // ceil(-log10(0.05)) = ceil(1.301...) = 2
  EXPECT_EQ(DecimalsForStep(0.01f), 2);  // The 1e-6 bias exists for this case
  EXPECT_EQ(DecimalsForStep(0.001f), 3);
  EXPECT_EQ(DecimalsForStep(0.0001f), 4);
  EXPECT_EQ(DecimalsForStep(0.00001f), 4);  // Clamped to max 4
  EXPECT_EQ(DecimalsForStep(0.00009f), 4);
  EXPECT_EQ(DecimalsForStep(2.0f), 0);

  // Four real steps from the shader pack (73 distinct values total).
  // 0.017453292519943295 = pi/180 (degrees-to-radians): ceil(-log10(0.017453...)) = 2
  EXPECT_EQ(DecimalsForStep(0.017453292519943295f), 2);
  // 0.00390625 = 1/256: ceil(-log10(0.00390625)) = 3
  EXPECT_EQ(DecimalsForStep(0.00390625f), 3);
  // 0.05555: ceil(-log10(0.05555)) = 2
  EXPECT_EQ(DecimalsForStep(0.05555f), 2);
  // 0.25: ceil(-log10(0.25)) = 1
  EXPECT_EQ(DecimalsForStep(0.25f), 1);
}

TEST(LibrashaderParameters, IsDefaultValueUsesRelativeEpsilon)
{
  // PCSX2's relative epsilon: abs(value - initial) <= 1e-6 * max(1, abs(initial)).
  EXPECT_TRUE(IsDefaultValue(0.0f, 0.0f));
  EXPECT_TRUE(IsDefaultValue(1.0f, 1.0f));
  EXPECT_TRUE(IsDefaultValue(1.0f + 1e-7f, 1.0f));
  EXPECT_FALSE(IsDefaultValue(1.0f + 1e-5f, 1.0f));

  // Large initial values scale the epsilon proportionally.
  EXPECT_TRUE(IsDefaultValue(1000.0f + 1e-4f, 1000.0f));
  EXPECT_FALSE(IsDefaultValue(1000.0f + 1e-2f, 1000.0f));

  // Edge: initial is negative.
  EXPECT_TRUE(IsDefaultValue(-1.0f - 1e-7f, -1.0f));
  EXPECT_FALSE(IsDefaultValue(-1.0f - 1e-5f, -1.0f));
}

TEST(LibrashaderParameters, ParseOverridesSucceeds)
{
  // Parameter names come from #pragma parameter and none in the shader pack contains ';' or '=',
  // so ';' is a safe separator and '=' splits name from value.
  const std::vector<std::string> entries = {"param1=1.5", "param2=2.0", "param3=0.0"};
  const Overrides overrides = ParseOverrides(entries);

  ASSERT_EQ(overrides.size(), 3u);
  EXPECT_EQ(overrides[0].first, "param1");
  EXPECT_FLOAT_EQ(overrides[0].second, 1.5f);
  EXPECT_EQ(overrides[1].first, "param2");
  EXPECT_FLOAT_EQ(overrides[1].second, 2.0f);
  EXPECT_EQ(overrides[2].first, "param3");
  EXPECT_FLOAT_EQ(overrides[2].second, 0.0f);
}

TEST(LibrashaderParameters, ParseOverridesSkipsMalformed)
{
  // No '=', value unparseable, name empty: all skipped so malformed entries cannot poison the list.
  const std::vector<std::string> entries = {"valid=1.0", "no_equals", "=2.0", "bad=value",
                                            "also_valid=3.14"};
  const Overrides overrides = ParseOverrides(entries);

  ASSERT_EQ(overrides.size(), 2u);
  EXPECT_EQ(overrides[0].first, "valid");
  EXPECT_FLOAT_EQ(overrides[0].second, 1.0f);
  EXPECT_EQ(overrides[1].first, "also_valid");
  EXPECT_FLOAT_EQ(overrides[1].second, 3.14f);
}

TEST(LibrashaderParameters, FormatOverridesRoundTrips)
{
  // Non-default values only are persisted, so resetting a parameter removes it instead of writing
  // the default back -- which is how a preset's own defaults can change on a pack update without
  // users being pinned to the old ones.
  const Overrides overrides = {{"brightness", 1.5f}, {"contrast", 0.75f}};
  const std::vector<std::string> entries = FormatOverrides(overrides);

  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0], "brightness=1.500000");
  EXPECT_EQ(entries[1], "contrast=0.750000");

  // Round-trip: parse what we just formatted.
  const Overrides parsed = ParseOverrides(entries);
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].first, "brightness");
  EXPECT_FLOAT_EQ(parsed[0].second, 1.5f);
  EXPECT_EQ(parsed[1].first, "contrast");
  EXPECT_FLOAT_EQ(parsed[1].second, 0.75f);
}

#if defined(__APPLE__)
// Enumerate against a real preset, gated on the shader pack being present the way existing preset
// tests do. On macOS the pack is under PCSX2's Application Support.
TEST(LibrashaderParameters, EnumerateRealPreset)
{
  const std::string pack_root =
      "/Users/ilya.lissoboi/Library/Application Support/PCSX2/shaders/shaders_slang";
  const std::string preset_path = pack_root + "/presets/crt-royale-kurozumi.slangp";
  if (!File::Exists(preset_path))
  {
    GTEST_SKIP() << "Shader pack not present; skipping real-preset test";
  }

  std::vector<ParameterInfo> params;
  std::string error;
  const bool ok = Enumerate(preset_path, &params, &error);

  ASSERT_TRUE(ok) << "Enumerate failed: " << error;
  EXPECT_FALSE(params.empty())
      << "crt-royale-kurozumi declares parameters; list should not be empty";

  // Spot-check: every parameter has a non-empty name and a valid range.
  for (const auto& p : params)
  {
    EXPECT_FALSE(p.name.empty());
    EXPECT_LE(p.minimum, p.initial);
    EXPECT_LE(p.initial, p.maximum);
    EXPECT_GE(p.step, 0.0f);
  }

  // Assert against a known parameter from crt-royale-kurozumi.slangp to verify struct layout and
  // that p.initial reflects the preset's override (2.4), not the underlying .slang default (2.5).
  // This catches struct-layout and free-semantics regressions across the C ABI.
  auto it = std::find_if(params.begin(), params.end(),
                         [](const ParameterInfo& p) { return p.name == "crt_gamma"; });
  ASSERT_NE(it, params.end()) << "crt_gamma not found in enumerated list";
  EXPECT_FLOAT_EQ(it->initial, 2.4f)
      << "crt_gamma initial should be preset override, not .slang default";
  EXPECT_FLOAT_EQ(it->minimum, 1.0f);
  EXPECT_FLOAT_EQ(it->maximum, 5.0f);
  EXPECT_FLOAT_EQ(it->step, 0.025f);
}
#endif
