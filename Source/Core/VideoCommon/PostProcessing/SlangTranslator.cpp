// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/SlangTranslator.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/ShaderCompileUtils.h"

namespace VideoCommon
{
namespace
{
constexpr size_t MAX_SAMPLERS = 8;

std::string_view Trim(std::string_view s)
{
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

// Extracts the sampler name from a line declaring `... uniform sampler2D <Name>;`.
// Returns empty if the line does not declare a sampler2D.
std::string ExtractSamplerName(std::string_view line)
{
  const auto kw = line.find("sampler2D");
  if (kw == std::string_view::npos)
    return {};
  std::string_view rest = Trim(line.substr(kw + std::string_view("sampler2D").size()));
  // Name runs until ';' or whitespace.
  size_t end = 0;
  while (end < rest.size() && (std::isalnum(static_cast<unsigned char>(rest[end])) != 0 ||
                               rest[end] == '_'))
  {
    ++end;
  }
  return std::string(rest.substr(0, end));
}

// Replaces every whole-word occurrence of `from` with `to` in `text`.
std::string ReplaceWord(const std::string& text, const std::string& from, const std::string& to)
{
  std::string out;
  out.reserve(text.size());
  const auto is_word_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  size_t pos = 0;
  while (pos < text.size())
  {
    const auto found = text.find(from, pos);
    if (found == std::string::npos)
    {
      out.append(text, pos, std::string::npos);
      break;
    }
    const bool left_ok = found == 0 || !is_word_char(text[found - 1]);
    const size_t after = found + from.size();
    const bool right_ok = after >= text.size() || !is_word_char(text[after]);
    out.append(text, pos, found - pos);
    if (left_ok && right_ok)
    {
      out += to;
    }
    else
    {
      out += from;
    }
    pos = after;
  }
  return out;
}
}  // namespace

TranslatedPass TranslateSlangPass(const SlangShaderSource& shader,
                                  const std::vector<std::string>& known_aliases,
                                  const std::vector<std::string>& lut_names)
{
  TranslatedPass result;

  // 1. Discover sampler declarations across both stages, in first-seen order.
  //    "Source" is always binding 0, even if the shader also declares it explicitly.
  std::vector<std::string> sampler_names = {"Source"};
  const auto already_seen = [&sampler_names](const std::string& name) {
    return std::find(sampler_names.begin(), sampler_names.end(), name) != sampler_names.end();
  };

  const auto scan_stage = [&](const std::string& source) {
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line))
    {
      const std::string name = ExtractSamplerName(line);
      if (!name.empty() && !already_seen(name))
        sampler_names.push_back(name);
    }
  };
  scan_stage(shader.vertex_source);
  scan_stage(shader.fragment_source);

  if (sampler_names.size() > MAX_SAMPLERS)
  {
    result.ok = false;
    result.error = "pass references " + std::to_string(sampler_names.size()) +
                   " samplers; max is " + std::to_string(MAX_SAMPLERS);
    return result;
  }

  // Assign binding indices.
  const auto binding_of = [&sampler_names](const std::string& name) -> int {
    const auto it = std::find(sampler_names.begin(), sampler_names.end(), name);
    return static_cast<int>(std::distance(sampler_names.begin(), it));
  };

  // 2. Line-level rewrite of a stage.
  const auto rewrite_stage = [&](const std::string& source) {
    std::string out;
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line))
    {
      std::string_view view = line;
      if (!view.empty() && view.back() == '\r')
        view.remove_suffix(1);
      const std::string_view trimmed = Trim(view);

      // Sampler declaration -> SAMPLER_BINDING(<assigned>).
      const std::string sampler = ExtractSamplerName(trimmed);
      if (!sampler.empty())
      {
        const int binding = binding_of(sampler);
        out += "SAMPLER_BINDING(" + std::to_string(binding) + ") uniform sampler2D " + sampler +
               ";\n";
        continue;
      }

      // UBO opener -> UBO_BINDING(std140, 1) uniform PSBlock {
      if (trimmed.find("uniform UBO") != std::string_view::npos &&
          trimmed.find('{') != std::string_view::npos)
      {
        // Preserve everything from '{' onward (the member list may start on this line).
        const auto brace = std::string(trimmed).find('{');
        out += "UBO_BINDING(std140, 1) uniform PSBlock ";
        out += std::string(trimmed).substr(brace);
        out += "\n";
        continue;
      }

      // Fragment output -> FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;
      if (trimmed.find("out vec4 FragColor") != std::string_view::npos &&
          trimmed.find("layout(location") != std::string_view::npos)
      {
        out += "FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n";
        continue;
      }

      // Varyings: layout(location = N) out/in ...  -> VARYING_LOCATION(N) out/in ...
      if (trimmed.find("layout(location") != std::string_view::npos &&
          (trimmed.find(" out ") != std::string_view::npos ||
           trimmed.find(" in ") != std::string_view::npos))
      {
        // Extract N.
        const std::string t(trimmed);
        const auto eq = t.find('=');
        const auto close = t.find(')');
        if (eq != std::string::npos && close != std::string::npos && close > eq)
        {
          const std::string n = std::string(Trim(std::string_view(t).substr(eq + 1, close - eq - 1)));
          out += "VARYING_LOCATION(" + n + ")" + t.substr(close + 1) + "\n";
          continue;
        }
      }

      out += std::string(view);
      out += '\n';
    }
    // Replace FragColor references with ocol0 in the body.
    return ReplaceWord(out, "FragColor", "ocol0");
  };

  result.vertex_glsl = rewrite_stage(shader.vertex_source);
  result.fragment_glsl = rewrite_stage(shader.fragment_source);
  result.sampler_names = std::move(sampler_names);
  result.ok = true;

  // known_aliases / lut_names are accepted for interface completeness and future
  // validation; sampler discovery already picks up whatever the shader references.
  (void)known_aliases;
  (void)lut_names;
  return result;
}

CompiledPassShaders CompileTranslatedPass(const TranslatedPass& pass,
                                          const std::string& include_dir)
{
  CompiledPassShaders out;
  if (!pass.ok)
    return out;

  // #include resolver rooted at the shader's own directory and the Sys shaders dir.
  ShaderIncluder includer(include_dir + DIR_SEP,
                          File::GetSysDirectory() + SHADERS_DIR DIR_SEP);

  out.vertex = g_gfx->CreateShaderFromSource(ShaderStage::Vertex, pass.vertex_glsl, &includer,
                                             "slang post-process vertex");
  if (!out.vertex)
    return {};

  out.pixel = g_gfx->CreateShaderFromSource(ShaderStage::Pixel, pass.fragment_glsl, &includer,
                                            "slang post-process fragment");
  if (!out.pixel)
    return {};

  return out;
}
}  // namespace VideoCommon
