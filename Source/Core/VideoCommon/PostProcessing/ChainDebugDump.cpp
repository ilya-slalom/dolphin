// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/ChainDebugDump.h"

#include <atomic>
#include <string>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/GraphicsSettings.h"
#include "VideoCommon/AbstractTexture.h"

namespace VideoCommon
{
namespace
{
// One frame's worth of evidence, not a stream: a readback per frame stalls the GPU and fills the
// disk. Reset only by restarting, which is what the flag's single use (a UAT capture) needs.
std::atomic<bool> s_budget_spent{false};
}  // namespace

bool ShouldDumpChainImages()
{
  return Config::Get(Config::GFX_LIBRASHADER_DUMP_CHAIN_IMAGES) && !s_budget_spent.load();
}

void NoteChainImagesDumped()
{
  s_budget_spent.store(true);
}

void ResetChainImageDumpBudgetForTest()
{
  s_budget_spent.store(false);
}

bool DumpChainImage(const AbstractTexture* texture, std::string_view label)
{
  if (texture == nullptr)
    return false;

  const std::string path =
      fmt::format("{}librashader-{}.png", File::GetUserPath(D_DUMPTEXTURES_IDX), label);
  if (!texture->Save(path, 0))
  {
    ERROR_LOG_FMT(VIDEO, "Librashader: failed to dump chain image to '{}'", path);
    return false;
  }
  INFO_LOG_FMT(VIDEO, "Librashader: dumped chain image {}x{} to '{}'", texture->GetWidth(),
               texture->GetHeight(), path);
  return true;
}
}  // namespace VideoCommon
