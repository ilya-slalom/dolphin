// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/PostProcessing/SlangPreset.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"

class AbstractFramebuffer;
class AbstractPipeline;
class AbstractShader;
class AbstractTexture;

namespace VideoCommon
{
// Multi-pass RetroArch-style .slangp post-processor. Replaces the old single-pass PostProcessing.
class MultipassPostProcessing
{
public:
  MultipassPostProcessing();
  ~MultipassPostProcessing();

  // *.slangp presets discovered under the user + sys Shaders dirs, as identifiers relative to
  // the containing Shaders dir with the extension stripped (e.g. "crt/crt-royale"). This is the
  // value stored in GFX_ENHANCE_POST_SHADER and resolved back by LoadPreset.
  static std::vector<std::string> GetPresetList();

  // Loads the preset named by GFX_ENHANCE_POST_SHADER (a .slangp under the Shaders dir).
  // Empty/failed -> pass-through mode.
  bool Initialize(AbstractTextureFormat format);
  void RecompileShader();    // reload preset (config change)
  void RecompilePipeline();  // rebuild pipelines only (format change)

  // Runs the pass chain, presenting into the currently-bound framebuffer over dst.
  void BlitFromTexture(const MathUtil::Rectangle<int>& dst, const MathUtil::Rectangle<int>& src,
                       const AbstractTexture* src_tex, int src_layer = -1);

private:
  struct Pass
  {
    SlangPassConfig config;
    std::vector<std::string> sampler_names;  // binding index -> input name
    SamplerState input_sampler;              // sampler applied to this pass's inputs
    std::unique_ptr<AbstractShader> vertex_shader;
    std::unique_ptr<AbstractShader> pixel_shader;
    std::unique_ptr<AbstractPipeline> pipeline;
    std::unique_ptr<AbstractTexture> output_texture;  // null for final pass
    std::unique_ptr<AbstractFramebuffer> output_framebuffer;
    std::string alias;
  };
  struct Lut
  {
    std::string name;
    std::unique_ptr<AbstractTexture> texture;
    SamplerState sampler;
  };

  bool LoadPreset(const std::string& preset_name);
  void ClearChain();

  std::vector<Pass> m_passes;
  std::vector<Lut> m_luts;
  AbstractTextureFormat m_framebuffer_format = AbstractTextureFormat::Undefined;
  bool m_passthrough = true;
  u32 m_frame_count = 0;
  u32 m_target_width = 0;
  u32 m_target_height = 0;
};

// Computes the preset identifier for a discovered .slangp path: the path relative to whichever
// root in `roots` contains it, with separators normalized to '/' and the ".slangp" extension
// stripped (e.g. "/sys/Shaders/crt/crt-royale.slangp" -> "crt/crt-royale"). If no root matches,
// falls back to the bare filename without extension. Pure/testable.
std::string PresetNameFromPath(const std::string& full_path,
                               const std::vector<std::string>& roots);
}  // namespace VideoCommon
