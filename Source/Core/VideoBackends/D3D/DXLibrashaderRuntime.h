// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/PostProcessing/LibrashaderRuntime.h"

namespace DX11
{
// Drives librashader's Direct3D 11 runtime. librashader_ld.h is unused; the libra_d3d11_*
// entry points are resolved through VideoCommon::Librashader::GetSymbol in the .cpp, which is
// the only translation unit that defines LIBRA_RUNTIME_D3D11.
class DXLibrashaderRuntime final : public VideoCommon::LibrashaderRuntime
{
public:
  DXLibrashaderRuntime();
  ~DXLibrashaderRuntime() override;

  bool IsSupported() const override;
  bool CreateChain(libra_shader_preset_t preset) override;
  void DestroyChain() override;
  bool HasChain() const override;
  bool RunFrame(const AbstractTexture* source, AbstractFramebuffer* target,
                u64 frame_count) override;
  void SetParameter(const char* name, float value) override;

private:
  // Opaque here so the header does not need d3d11.h or a LIBRA_RUNTIME_* macro.
  void* m_chain = nullptr;
};
}  // namespace DX11
