// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/PostProcessing/LibrashaderRuntime.h"

namespace DX12
{
// Drives librashader's Direct3D 12 runtime. librashader_ld.h is unused; the libra_d3d12_*
// entry points are resolved through VideoCommon::Librashader::GetSymbol in the .cpp, which is
// the only translation unit that defines LIBRA_RUNTIME_D3D12.
class DX12LibrashaderRuntime final : public VideoCommon::LibrashaderRuntime
{
public:
  DX12LibrashaderRuntime();
  ~DX12LibrashaderRuntime() override;

  bool IsSupported() const override;
  bool CreateChain(libra_shader_preset_t preset) override;
  void DestroyChain() override;
  bool HasChain() const override;
  bool RunFrame(const AbstractTexture* source, AbstractFramebuffer* target,
                u64 frame_count) override;
  void SetParameter(const char* name, float value) override;

  // DiscardPendingTargetClear() is deliberately not overridden: Dolphin's D3D12 backend has no
  // deferred clear to drop. SetAndDiscardFramebuffer() only transitions render targets and
  // DXFramebuffer exposes nothing but the immediate ClearRenderTargets()/ClearDepth(), so there is
  // no clear-pending flag anywhere -- unlike PCSX2's GSTexture12::CommitClear.

private:
  // Opaque here so the header does not need d3d12.h or a LIBRA_RUNTIME_* macro.
  void* m_chain = nullptr;
};
}  // namespace DX12
