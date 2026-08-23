// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/SlangTranslator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
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
// Matches the Vulkan utility descriptor set's combined-image-sampler count
// (NUM_UTILITY_PIXEL_SAMPLERS). crt-royale's mask-apply pass needs 9.
constexpr size_t MAX_SAMPLERS = 16;

std::string_view Trim(std::string_view s)
{
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

// Extracts the sampler name from a line declaring `... uniform sampler2D <Name>;`.
// Returns empty if the line does not declare a uniform sampler2D (e.g. a `sampler2D` function
// parameter or a helper typedef is ignored -- only uniform declarations count).
std::string ExtractSamplerName(std::string_view line)
{
  if (line.find("uniform") == std::string_view::npos)
    return {};
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

// If the line declares its sampler with an explicit `layout(binding = N)`, returns N.
// Returns -1 when no explicit binding is present. crt-royale reuses the same binding number
// in mutually-exclusive #ifdef/#else branches, so distinct binding numbers -- not distinct
// names -- give the true sampler count (the translator has no preprocessor).
int ExtractSamplerBinding(std::string_view line)
{
  const auto pos = line.find("binding");
  if (pos == std::string_view::npos)
    return -1;
  const auto eq = line.find('=', pos);
  if (eq == std::string_view::npos)
    return -1;
  std::string_view rest = Trim(line.substr(eq + 1));
  int value = 0;
  bool any = false;
  for (const char c : rest)
  {
    if (c < '0' || c > '9')
      break;
    value = value * 10 + (c - '0');
    any = true;
  }
  return any ? value : -1;
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

bool StartsWith(std::string_view s, std::string_view prefix)
{
  return s.substr(0, prefix.size()) == prefix;
}

// Strips a trailing `// ...` line comment (RetroArch shaders annotate varyings with comments
// that can contain the words "in"/"out", which must not be mistaken for storage qualifiers).
std::string_view StripLineComment(std::string_view s)
{
  const auto pos = s.find("//");
  return pos == std::string_view::npos ? s : Trim(s.substr(0, pos));
}

// For a `layout(...) <qualifier> <type> <name>;` declaration, returns the storage qualifier
// token immediately after the closing ')': "in", "out", or "" if none.
std::string_view StorageQualifierAfterLayout(std::string_view decl)
{
  const auto close = decl.find(')');
  if (close == std::string_view::npos)
    return {};
  std::string_view rest = Trim(decl.substr(close + 1));
  const auto space = rest.find_first_of(" \t");
  const std::string_view token = space == std::string_view::npos ? rest : rest.substr(0, space);
  if (token == "in" || token == "out")
    return token;
  return {};
}

// Classifies the type keyword at the start of a member declaration for std140 packing.
UboMemberType ClassifyMemberType(std::string_view decl)
{
  decl = Trim(decl);
  const auto space = decl.find_first_of(" \t");
  const std::string_view kw = space == std::string_view::npos ? decl : decl.substr(0, space);
  if (kw == "mat4" || kw == "float4x4")
    return UboMemberType::Mat4;
  if (kw == "vec4" || kw == "float4" || kw == "ivec4" || kw == "uvec4")
    return UboMemberType::Vec4;
  if (kw == "vec3" || kw == "float3" || kw == "ivec3" || kw == "uvec3")
    return UboMemberType::Vec3;
  if (kw == "vec2" || kw == "float2" || kw == "ivec2" || kw == "uvec2")
    return UboMemberType::Vec2;
  if (kw == "float" || kw == "int" || kw == "uint" || kw == "bool")
    return UboMemberType::Float;
  return UboMemberType::Unknown;
}

// Extracts every RetroArch uniform block from a stage source, appending merged member lines to
// `out_members` (deduplicated by member name -- the two blocks never share names in practice,
// but a member repeated across stages must not be emitted twice) and recording each block's
// instance name in `out_instances`. Returns the source with those block declarations removed.
std::string ExtractUniformBlocks(const std::string& source, std::vector<std::string>* out_members,
                                 std::vector<std::string>* out_instances,
                                 std::vector<std::string>* out_member_names,
                                 std::vector<UboMember>* out_typed_members)
{
  std::string out;
  std::istringstream in(source);
  std::string line;
  while (std::getline(in, line))
  {
    std::string_view view = line;
    if (!view.empty() && view.back() == '\r')
      view.remove_suffix(1);
    const std::string_view trimmed = Trim(view);

    // A block opener declares `uniform <Name>` and is a layout(push_constant|std140) line.
    const bool is_block_opener =
        trimmed.find("uniform ") != std::string_view::npos &&
        (trimmed.find("push_constant") != std::string_view::npos ||
         trimmed.find("std140") != std::string_view::npos) &&
        trimmed.find("sampler") == std::string_view::npos;

    if (!is_block_opener)
    {
      out += std::string(view);
      out += '\n';
      continue;
    }

    // Consume until the closing `} instance;`. The opener may or may not include the '{'.
    std::string block(trimmed);
    while (block.find('}') == std::string::npos && std::getline(in, line))
    {
      std::string_view v = line;
      if (!v.empty() && v.back() == '\r')
        v.remove_suffix(1);
      block += '\n';
      block += std::string(v);
    }

    // Instance name: text between '}' and ';'.
    const auto brace_close = block.find('}');
    const auto semi = block.find(';', brace_close);
    if (brace_close != std::string::npos && semi != std::string::npos)
    {
      const std::string instance = std::string(Trim(
          std::string_view(block).substr(brace_close + 1, semi - brace_close - 1)));
      if (!instance.empty())
        out_instances->push_back(instance);
    }

    // Members: everything between '{' and '}'.
    const auto brace_open = block.find('{');
    if (brace_open != std::string::npos && brace_close != std::string::npos &&
        brace_close > brace_open)
    {
      const std::string body = block.substr(brace_open + 1, brace_close - brace_open - 1);
      std::istringstream member_in(body);
      std::string member_line;
      while (std::getline(member_in, member_line))
      {
        const std::string_view m = Trim(member_line);
        if (m.empty())
          continue;
        // Derive the member name (last identifier before the ';').
        const auto sc = m.find(';');
        std::string_view decl = sc == std::string_view::npos ? m : m.substr(0, sc);
        decl = Trim(decl);
        size_t name_start = decl.size();
        while (name_start > 0 && (std::isalnum(static_cast<unsigned char>(decl[name_start - 1])) ||
                                  decl[name_start - 1] == '_'))
        {
          --name_start;
        }
        const std::string member_name(decl.substr(name_start));
        if (std::find(out_member_names->begin(), out_member_names->end(), member_name) !=
            out_member_names->end())
        {
          continue;  // already emitted (e.g. block appears in both stages)
        }
        out_member_names->push_back(member_name);
        out_members->emplace_back(m);
        out_typed_members->push_back({member_name, ClassifyMemberType(decl)});
      }
    }
    // Block declaration removed from output.
  }
  return out;
}
}  // namespace

TranslatedPass TranslateSlangPass(const SlangShaderSource& shader,
                                  const std::vector<std::string>& known_aliases,
                                  const std::vector<std::string>& lut_names)
{
  TranslatedPass result;

  // 1. Discover sampler declarations across both stages and assign each a Dolphin binding
  //    "slot". "Source" is always slot 0. Samplers declared with an explicit slang
  //    layout(binding = N) are deduplicated by N: crt-royale reuses the same N across
  //    mutually-exclusive #ifdef/#else branches, so distinct binding numbers -- not distinct
  //    textual names -- give the true sampler count. Samplers with no explicit binding are
  //    keyed by name.
  std::vector<std::string> sampler_names = {"Source"};
  std::map<int, size_t> slang_binding_to_slot;  // explicit slang binding -> slot index
  std::map<std::string, size_t> name_to_slot;   // sampler name -> slot index
  name_to_slot["Source"] = 0;

  const auto scan_stage = [&](const std::string& source) {
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line))
    {
      const std::string name = ExtractSamplerName(line);
      if (name.empty())
        continue;
      if (name_to_slot.count(name) != 0)
        continue;

      const int binding = ExtractSamplerBinding(line);
      if (binding >= 0)
      {
        const auto existing = slang_binding_to_slot.find(binding);
        if (existing != slang_binding_to_slot.end())
        {
          // A different name reusing an already-seen slang binding (dead #ifdef branch):
          // map this name onto the same slot; do not allocate a new one.
          name_to_slot[name] = existing->second;
          continue;
        }
      }

      const size_t slot = sampler_names.size();
      sampler_names.push_back(name);
      name_to_slot[name] = slot;
      if (binding >= 0)
        slang_binding_to_slot[binding] = slot;
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
  const auto binding_of = [&name_to_slot](const std::string& name) -> int {
    const auto it = name_to_slot.find(name);
    return it == name_to_slot.end() ? 0 : static_cast<int>(it->second);
  };

  // 2. Extract the RetroArch uniform blocks (push_constant "Push {} params" + "std140 UBO {}
  //    global") from both stages, merging their members into one Dolphin PSBlock. Each stage's
  //    source has the block declarations removed.
  std::vector<std::string> ubo_members;
  std::vector<std::string> ubo_member_names;
  std::vector<std::string> instances;
  std::vector<UboMember> typed_members;
  std::string vs_noblocks = ExtractUniformBlocks(shader.vertex_source, &ubo_members, &instances,
                                                 &ubo_member_names, &typed_members);
  std::string fs_noblocks = ExtractUniformBlocks(shader.fragment_source, &ubo_members, &instances,
                                                 &ubo_member_names, &typed_members);

  // Build the merged block declaration with a single instance name "params". Any other instance
  // name (e.g. "global") is aliased to it, so params.X, global.X, and macro-expanded IN.X all
  // resolve to the same block.
  std::string ubo_decl = "UBO_BINDING(std140, 1) uniform PSBlock {\n";
  for (const std::string& member : ubo_members)
  {
    ubo_decl += "  ";
    ubo_decl += member;
    ubo_decl += '\n';
  }
  ubo_decl += "} params;\n";
  for (const std::string& instance : instances)
  {
    if (instance != "params")
      ubo_decl += "#define " + instance + " params\n";
  }

  // Neutralize the HLSL-compat macros the shader redefines. Dolphin's backend header defines
  // `frac`/`lerp` as OBJECT-like (`#define lerp mix`) while RetroArch's compat_macros.inc
  // defines them FUNCTION-like (`#define lerp(a,b,c) mix(a,b,c)`); glslang errors on that
  // mismatch. #undef-ing them before the shader's own #define makes the redefinition harmless.
  // We only undef the names Dolphin's header actually defines AND that collide in kind -- the
  // type aliases (float2/3/4, uint2..) are object-like in both, so identical redefinition is
  // allowed and they must stay defined (our own emitted `out float4 ocol0` depends on them).
  static const char* const kCompatMacros[] = {"frac", "lerp"};
  std::string undefs;
  for (const char* macro : kCompatMacros)
    undefs += std::string("#undef ") + macro + "\n";

  // 3. Line-level rewrite of a stage. is_vertex controls attribute/varying handling.
  const auto rewrite_stage = [&](const std::string& source, bool is_vertex) {
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

      // Fragment output -> FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;
      if (trimmed.find("out vec4 FragColor") != std::string_view::npos &&
          trimmed.find("layout(location") != std::string_view::npos)
      {
        out += "FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n";
        continue;
      }

      const std::string_view decl = StripLineComment(trimmed);
      const bool has_location = StartsWith(decl, "layout(location");
      const std::string_view qualifier = has_location ? StorageQualifierAfterLayout(decl)
                                                       : std::string_view{};

      // Vertex attribute inputs (`layout(location=N) in ...`) are dropped: Dolphin utility draws
      // are attribute-less; the values the shader reads (Position/TexCoord) are synthesized
      // inside main() below.
      if (is_vertex && qualifier == "in")
        continue;

      // Varyings: layout(location = N) in/out ... -> VARYING_LOCATION(N) in/out ...
      if (has_location && !qualifier.empty())
      {
        const std::string t(decl);
        const auto eq = t.find('=');
        const auto close = t.find(')');
        if (eq != std::string::npos && close != std::string::npos && close > eq)
        {
          const std::string n =
              std::string(Trim(std::string_view(t).substr(eq + 1, close - eq - 1)));
          out += "VARYING_LOCATION(" + n + ")" + t.substr(close + 1) + "\n";
          continue;
        }
      }

      out += std::string(view);
      out += '\n';
    }

    if (is_vertex)
    {
      // Synthesize the vertex attributes the shader expects from the fullscreen-triangle
      // vertex id, so `MVP * Position` (MVP is identity) yields fullscreen clip coords and
      // TexCoord spans [0,1]. On Vulkan the clip-space Y is inverted (matching Dolphin's
      // fixed post-process vertex shader and pass-through pipeline); since MVP is identity we
      // bake the flip into Position.y.
      const std::string inject =
          "  vec2 dolphin_fsq = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
          "  vec4 Position = vec4(dolphin_fsq * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);\n"
          "  vec2 TexCoord = dolphin_fsq;\n"
          "#ifdef API_VULKAN\n"
          "  Position.y = -Position.y;\n"
          "#endif\n";
      const auto main_pos = out.find("void main");
      if (main_pos != std::string::npos)
      {
        const auto brace = out.find('{', main_pos);
        if (brace != std::string::npos)
          out.insert(brace + 1, "\n" + inject);
      }
    }

    // Replace FragColor references with ocol0 in the body.
    return ReplaceWord(out, "FragColor", "ocol0");
  };

  // Assemble: merged UBO block + #undef of compat macros, then the stage body.
  result.vertex_glsl = ubo_decl + undefs + rewrite_stage(vs_noblocks, /*is_vertex=*/true);
  result.fragment_glsl = ubo_decl + undefs + rewrite_stage(fs_noblocks, /*is_vertex=*/false);
  result.sampler_names = std::move(sampler_names);
  result.ubo_members = std::move(typed_members);
  result.ok = true;

  // known_aliases / lut_names are accepted for interface completeness and future
  // validation; sampler discovery already picks up whatever the shader references.
  (void)known_aliases;
  (void)lut_names;
  return result;
}

std::vector<u8> PackSlangUniforms(const std::vector<UboMember>& members,
                                  const UniformResolver& resolver)
{
  // std140: {align, size} in bytes for each supported type.
  const auto layout = [](UboMemberType t) -> std::pair<size_t, size_t> {
    switch (t)
    {
    case UboMemberType::Float:
      return {4, 4};
    case UboMemberType::Vec2:
      return {8, 8};
    case UboMemberType::Vec3:
      return {16, 12};
    case UboMemberType::Vec4:
      return {16, 16};
    case UboMemberType::Mat4:
      return {16, 64};
    case UboMemberType::Unknown:
    default:
      return {16, 16};  // conservative: treat as vec4
    }
  };
  const auto component_count = [](UboMemberType t) -> int {
    switch (t)
    {
    case UboMemberType::Float:
      return 1;
    case UboMemberType::Vec2:
      return 2;
    case UboMemberType::Vec3:
      return 3;
    case UboMemberType::Mat4:
      return 16;
    default:
      return 4;
    }
  };

  std::vector<u8> buffer;
  size_t offset = 0;
  for (const UboMember& member : members)
  {
    const auto [align, size] = layout(member.type);
    offset = (offset + align - 1) & ~(align - 1);
    if (buffer.size() < offset + size)
      buffer.resize(offset + size, 0);

    const int count = component_count(member.type);
    std::array<float, 16> values = {};
    if (!resolver || !resolver(member.name, values.data(), count))
      values.fill(0.0f);
    std::memcpy(buffer.data() + offset, values.data(), size);
    offset += size;
  }

  // std140 rounds the whole block up to a multiple of 16.
  const size_t rounded = (buffer.size() + 15) & ~static_cast<size_t>(15);
  buffer.resize(rounded, 0);
  return buffer;
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
