# Librashader Desktop Runtimes and Single-Preset Shader UI — Design

**Date:** 2026-09-16
**Status:** approved (scope decisions recorded in §6; the §7 gate resolved by measurement 2026-09-18)
**Related:** [desktop post-processing parity](2026-09-16-desktop-postprocessing-parity-design.md),
[librashader Android post-processing](2026-08-22-librashader-android-postprocessing-design.md)
**Reference implementation:** PCSX2 (`~/work/pcsx2`), `pcsx2/GS/ShaderChain/` and `pcsx2-qt/Shader*Dialog.*`

## 1. Problem

Windows UAT of the `2606-fork-2` release produced three findings:

1. **D3D12 + a slang shader renders a black screen.**
2. **The chain-building UI is very hard to use.**
3. **crt-royale and its derivatives render very dark** (observed on Vulkan with the
   librashader renderer selected).

Finding 1 is a defect in the hand-rolled multipass executor's D3D12 path. Finding 2 is a
design problem — the UI was ported from Android without rethinking it for a 30,864-preset
library. Finding 3 is resolved by measurement: no Dolphin defect (§7).

## 2. What is already established

### 2.1 The D3D12 black screen (finding 1) — root cause confirmed

Post-processing draws are *utility* draws, and
`DX12Context::CreateUtilityRootSignature` (`DX12Context.cpp:394-409`) declares only **8**
SRVs and 8 samplers, while `CreateGXRootSignature` (`:352-356`) declares
`VideoCommon::MAX_PIXEL_SHADER_SAMPLERS` = **16**. The slang translator's ceiling
(`SlangTranslator.cpp:20-26`) was raised to 16 to match Vulkan's
`NUM_UTILITY_PIXEL_SAMPLERS`, so a pass declaring 9 samplers — crt-royale's mask-apply pass
does — produces a pipeline D3D12 cannot create.

`MultipassPostProcessing.cpp:628` then swallows the failure:

```cpp
if (!pass.pipeline) continue;
```

A dropped *final* pass writes nothing to the backbuffer, which is the black screen. The
failure is silent: no log line names the pass or the reason.

### 2.2 The chain UI (finding 2) — design, not a bug

`PostProcessingChainDialog` (127 lines) offers a flat list plus two buttons that **both
close the dialog**, so building an N-preset chain takes N round trips, and the chain itself
is a read-only `QLabel`. Upstream of it, `EnhancementsWidget` presents every discovered
preset in one flat `QComboBox` (`EnhancementsWidget.cpp:367-403`) — 30,864 entries in the
full libretro pack.

PCSX2's equivalent is a tree with a filter box (`ShaderPresetPickerDialog`, 135 lines:
`QStandardItemModel` + `QSortFilterProxyModel` with `setRecursiveFilteringEnabled(true)` and
`setFilterRole(ROLE_PATH)`; folder items non-selectable; the filter expands all and an empty
filter collapses all), plus a separate parameter editor (`ShaderParametersDialog`, 359
lines).

**PCSX2 is single-preset.** Its settings tab exposes one read-only preset line edit with
*Browse…*, *Clear*, *Parameters…*, *Favorites…* — there is no chain editor to copy.

### 2.3 Chains are already a lie on the librashader path

`LibrashaderPostProcessing.cpp:78-80` truncates silently:

> librashader accepts a single preset, so only the first entry of a ';'-separated chain is used.

So a user who builds a 3-preset chain and selects the librashader renderer gets preset 1 and
no warning. This live defect is what drove the decision to drop chains rather than extend
them.

### 2.4 The vendored library is Vulkan-only, but its preset API is complete

`strings` on `Externals/librashader/lib/windows-x64/librashader.dll` yields **every**
runtime-independent symbol — `libra_preset_create_with_options`, `libra_preset_ctx_*`,
`libra_preset_get_runtime_params`, `libra_preset_free_runtime_params` — and **only** the
`libra_vk_*` runtime. It was built `--no-default-features --features runtime-vulkan`
(`Externals/librashader/README.md`).

Two consequences that shape the task order:

- The **parameters UI needs no rebuild.** Parameter enumeration uses only
  runtime-independent symbols, which are present today.
- The **per-backend runtimes need a rebuild** of the Windows DLL and the macOS dylib.

### 2.5 The D3D11 and D3D12 runtimes work on the target GPU

A librashader CLI built from source on the Windows UAT host ran crt-royale successfully
through `-r d3d11` and `-r d3d12`, agreeing with each other to within 0.1% on a flat-field
photometric probe. The upstream source tree is already checked out on that machine at
`C:\src\librashader`, on the `librashader-cache-v0.12.0` tag with a working
`vcvars64` + direct-`cargo.exe` recipe (`C:\src\build-librashader.bat`). Option C's rebuild
needs no new software on that host.

### 2.6 The D3D12 runtime needs no `dxcompiler.dll` if built `-static`

`librashader-runtime-d3d12/src/util.rs:233-252` selects its DXC entry point by feature:

```rust
#[cfg(all(feature = "static", target_arch = "x86_64"))]
pub unsafe fn DxcCreateInstance<T>(...)  // mach_dxcompiler_rs, linked in

#[cfg(not(all(feature = "static", target_arch = "x86_64")))]
pub use windows::Win32::Graphics::Direct3D::Dxc::DxcCreateInstance;  // needs dxcompiler.dll
```

`librashader-capi` exposes this as `runtime-d3d12-static`. Building with it links DXC
statically, so **no `dxcompiler.dll` has to be packaged**. (This corrects an earlier
assumption in this investigation that the DLL was mandatory; PCSX2 ships no `dxcompiler.dll`
either.)

Feature sets for the rebuild:

| Target | `--features` |
|---|---|
| `x86_64-pc-windows-msvc` | `runtime-vulkan,runtime-d3d11,runtime-d3d12-static,runtime-opengl` |
| `aarch64-apple-darwin` | `runtime-vulkan,runtime-metal,runtime-opengl` |
| `aarch64-linux-android` | `runtime-vulkan` (unchanged) |

## 3. Approved approach

**Drive librashader's native runtime per backend; retire the hand-rolled path on desktop.**
The engine of record on Windows and macOS becomes librashader for every graphics backend
(D3D11, D3D12, Vulkan, Metal, OpenGL), so there is one code path to make correct instead of
five backend-specific translator/executor paths.

This subsumes finding 1: D3D12 gets a real runtime rather than a translator whose output
D3D12's utility root signature cannot accept.

It does **not** subsume finding 3 — the darkening was observed on the librashader path, so
it lives inside the path this makes exclusive. See §7.

## 4. Architecture

### 4.1 The loader: stop using `librashader_ld.h`

`librashader_ld.h` declares one `libra_instance_t` whose members are gated by
`LIBRA_RUNTIME_*` macros, and its `librashader_load_instance()` is `static inline`. Two
translation units including it with different macro sets get two incompatible structs under
one name. It also forces a single TU to include `vulkan.h`, `d3d11.h`, `d3d12.h`, the GL
headers *and* the Metal headers together.

PCSX2 solves this by not using the `_ld` header at all. Adopt its shape
(`pcsx2/GS/ShaderChain/LibrashaderLoader.h`):

- A `VideoCommon::Librashader` module includes plain `librashader.h` (which supplies the
  `PFN_libra_*` typedefs) and owns the `dlopen`/`LoadLibraryW`, the availability probe with a
  human-readable failure reason, error formatting, and a `CommonFunctions` struct of the
  runtime-independent entry points.
- It exposes `void* GetSymbol(const char* name)` so each backend TU resolves only its own
  `libra_<api>_*` symbols, with only its own `LIBRA_RUNTIME_*` macro defined and only its own
  API headers included.

This also removes the `_LIBRASHADER_LOAD` patch that `Externals/librashader/README.md`
currently documents against the vendored `librashader_ld.h`.

### 4.2 What moves into VideoCommon

Reading `LibrashaderPostProcessing.cpp` (523 lines), the Vulkan-specific surface is small.
Backend-agnostic and hoisted to a `VideoCommon::LibrashaderPostProcessing` base implementing
`IPostProcessor`:

| Currently | Depends on |
|---|---|
| `ResolvePresetPath` (`:78`) | `File::` only |
| `BuildPassthroughPipeline` (`:198`) | `g_gfx` only |
| `BuildDownscalePipeline` (`:244`) | `g_gfx` only |
| `EnsureOutputTarget` (`:375`) | `g_gfx` only |
| `DownscaleToNativeSource` (`:327`) | `g_gfx`, plus a Vulkan barrier tail |
| `BlitFromTexture` orchestration (`:392`) | `g_gfx`, plus five hooks |

The five hooks become the per-backend interface:

1. **Create** the chain from a `libra_shader_preset_t` with the backend's device struct and
   options.
2. **Free** the chain.
3. **Describe** an `AbstractTexture` as the backend's `libra_image_*`.
4. **Prepare** — end the render pass, commit clears, transition source → shader-read and
   destination → render-target.
5. **Reconcile** — restore Dolphin's own tracking after librashader has recorded commands.

Hook 5 is where the backends differ most. PCSX2's D3D12 path
(`GSDevice12.cpp:5015-5061`) shows the requirement: after
`libra_d3d12_filter_chain_frame`, call `InvalidateCachedState()` **and re-bind the
descriptor heaps**. Vulkan's equivalent is today's `OverrideImageLayout`.

### 4.3 Renderer selection

The user-facing "Post-Processing Renderer" choice goes away. The rule becomes: **use
librashader when the library loaded; use the built-in multipass executor only when it did
not.** That satisfies "librashader only on desktop" without platform `#ifdef`s, and it keeps
the executor reachable where no librashader binary is vendored — Android x86_64 (documented
in `Externals/librashader/README.md:51`) and Linux.

`MultipassPostProcessing` therefore stays in the tree. It is not dead code, it is the
fallback, and 15 unit-test targets cover it.

### 4.4 Single preset

`GFX_ENHANCE_POST_SHADER` stops being a `;`-separated chain spec and becomes one preset
path. `ShaderChainSpec` (`AppendToChainSpec`, `DescribeChainSpec`, `CHAIN_SEPARATOR`) and
`PostProcessingChainDialog` are removed along with their tests. Existing configs holding a
chain resolve to their first entry — which is what the librashader path already did
silently, now without the silence.

### 4.5 The UI

Two dialogs ported from PCSX2, matching its layout and behaviour as closely as Dolphin's
settings plumbing allows:

- **`ShaderPresetPickerDialog`** — tree of folders and presets built from the discovered
  preset list, filter box with recursive filtering, non-selectable folder items,
  double-click to accept, OK disabled until a preset is current.
- **`ShaderParametersDialog`** — one row per `#pragma parameter` (label, slider, spin box,
  per-row Reset), *Reset All*, a debounced write, and rows hidden behind a status label when
  the preset has no parameters or fails to load.

The `EnhancementsWidget` post-processing row becomes PCSX2's shape: a read-only preset field
plus *Browse…*, *Clear*, *Parameters…*, keeping this fork's existing *Download…* menu.

Parameter values are enumerated through `libra_preset_get_runtime_params` — available in the
current vendored binaries — and persisted per preset, then pushed into the live chain with
`libra_<api>_filter_chain_set_param`.

## 5. Files

**New:** `VideoCommon/PostProcessing/LibrashaderLoader.{h,cpp}`,
`LibrashaderPostProcessing.{h,cpp}` (VideoCommon), `LibrashaderParameters.{h,cpp}`;
per-backend runtime adapters under `VideoBackends/{D3D,D3D12,Metal,OGL,Vulkan}/`;
`DolphinQt/Config/Graphics/ShaderPresetPickerDialog.{h,cpp}`,
`ShaderParametersDialog.{h,cpp}`.

**Modified:** `AbstractGfx.{h,cpp}` (selection rule), each backend's `*Gfx.cpp`,
`EnhancementsWidget.{h,cpp}`, `DX12Context.cpp` (§2.1),
`MultipassPostProcessing.cpp:628` (§2.1), `Externals/librashader/README.md` (rebuild
provenance and the removal of the `_LIBRASHADER_LOAD` patch), the CMake packaging for the
rebuilt binaries.

**Deleted:** `PostProcessingChainDialog.{h,cpp}`, `ShaderChainSpec.{h,cpp}`,
`ShaderChainSpecTest.cpp`.

## 6. Scope decisions

Recorded from this session's review:

- **Approach C** — librashader only on desktop, native runtime per backend. (Not B, which
  kept both engines.)
- **Drop chains** — single preset, exactly as PCSX2. (Not: extend the chain editor, and not:
  keep chains on the built-in path only.)
- **Port the picker and the parameters dialog.** Explicitly *not* ported: PCSX2's
  *Favorites…* dialog, its preset-cycling hotkeys, and its per-game settings layer, none of
  which Dolphin has an equivalent for.
- **Match PCSX2's layout as closely as possible** rather than inventing a Dolphin-native
  arrangement.

Out of scope: the librashader D3D9 runtime (Dolphin has no D3D9 backend); Linux and macOS
Intel librashader binaries; the R50 `#endif` preprocessor bug in UBO-member extraction
(~956 of 2987 presets), which belongs to the built-in translator and is tracked separately.

## 7. Resolved: the crt-royale darkening

This section left three candidates open, all of which needed pixels from a running Dolphin:
**(1)** there is no defect and crt-royale is simply this dark; **(2)** the chain's input is already
dark, so the defect is upstream in the XFB or the downscale; **(3)** the chain's output is mishandled
downstream, in the passthrough blit or the backbuffer handling.

The answer is **(1): no Dolphin defect.** Measured 2026-09-18 on the UAT host against real GameCube
content. crt-royale's brightness loss is the shader's own, and Dolphin hands the chain's output to the
screen intact: on one static frame the on-screen picture is **99.0%** of the chain's own output image.
UAT finding 3 closes on this measurement.

Refuted before that measurement, each by evidence, and none of them resurrected by it:

| Theory | Refuted by |
|---|---|
| Dynamic rendering path differs | Both branches bind the same image view and `render_pass_format` |
| Library version differs from PCSX2's | Both are 0.12.0, built on the same machine |
| Our box-downscale loses energy | `sum` over n×n then `* (1.0/n²)` — verified correct |
| Input-resolution policy | PCSX2's regime measures 70.8%, ours 68% |
| 10-bit swapchain format | librashader maps `A2B10G10R10_UNORM_PACK32` correctly (`librashader-common/src/vk.rs:18-19,56-57`). Right about the renderer — but the 10-bit swapchain is exactly what broke the *instrument*, see §7.2 |
| HDR/color-space frame options | Both Dolphin and PCSX2 pass `nullptr` frame options |

The earlier numbers here came from librashader's CLI on a **flat sRGB-128 field**, not from a running
Dolphin — the UAT host had no GameCube/Wii images at the time:

| runtime | source | output | mean vs input |
|---|---|---|---|
| vulkan | 640×528 | 640×528 | 110.9% |
| vulkan | 1920×1080 | 1920×1080 | 70.8% |
| d3d11 | 640×528 | 1920×1080 | 68.1% |
| d3d12 | 640×528 | 1920×1080 | 68.0% |

That table is kept for provenance, but its ~68-71% is **not a criterion**. A phosphor-mask shader's
loss depends on both the output resolution and the picture: the same table already spans 68% to 111%
purely by changing resolution, and the measurement below spans 86% to 56% purely by changing which
frame it lands on. Candidate 1 was originally phrased as "output/input ≈ 68-71%"; its substance
— no Dolphin defect — is what was established, by a different and stronger test than a band.

### 7.1 What was measured

Game images arrived on the UAT host (`F:\games\emulated\gc`), so the chain dump could be pointed at
real content. All figures are mean channel value over the region, 0-255, via
`Tools/chain-dump-mean.py`. The frame is the static
Metroid Prime (GM8E01) title screen, dump armed at post-processed frame 2100 (~35 s in, at the rig's
measured 58-59 fps). The chain input dump is **bit-identical across runs and across backends** — sha1
`34a584f20d1634c0c33992e1f726c532a9528b17` on both Vulkan and D3D12 — which is what makes these rows
comparable. Chain input is 640×448; chain output and the window's render rect are both 640×477, so the
screen column is `--crop 8,32,640,477` of the 656×519 window capture.

| backend | executor | preset | chain input | chain output | on screen | output/input | screen/input |
|---|---|---|---|---|---|---|---|
| Vulkan | librashader | crt-royale | 63.292 | 54.760 | 54.203 | **86.5%** | 85.6% |
| Vulkan | librashader | `nearest` (control) | 63.292 | 63.271 | 62.200 | **100.0%** | 98.3% |
| D3D12 | librashader | crt-royale | 63.292 | — (§7.2) | 52.710 | — | 83.3% |
| D3D12 | librashader | crt-geom | — | — | 48.240 | — | 76.2% |
| D3D12 | built-in | crt-geom | — | — | 46.272 | — | 73.1% |

Three things this establishes:

- **The chain's input is not dark** (rules out candidate 2). `nearest.slangp` is `stock.slang`, a pure
  passthrough; its chain output is 100.0% of its input and its on-screen picture is 98.3% of it — the
  residual being the 448→477 letterbox rows the crop includes.
- **Nothing downstream loses light** (rules out candidate 3). For crt-royale the screen is 99.0% of the
  chain's own output image. The passthrough blit and backbuffer handling that Task 4 restructured are
  faithful.
- **librashader is not darker than Dolphin's built-in executor.** On the same preset and backend it is
  *brighter* — 76.2% against 73.1% — which is the opposite of the direction UAT finding 3 assumed.

The loss is strongly content-dependent, so one frame is not the answer. On the dark Retro Studios
logo frame (frame 1200) the same Vulkan + crt-royale chain measures input 19.697 → output 11.015, i.e.
**55.9%**. A mask-and-scanline simulation costs proportionally more on a dark picture, which has less
headroom for its bloom to recover.

Visual check, not only ratios: every capture above was inspected. All show the Metroid Prime title
screen at normal brightness with the preset's characteristic look — crt-royale's fine phosphor triads,
crt-geom's barrel curvature and vignette. None is a black or near-black screen.

Note for anyone repeating this: **Dolphin's own screenshot will not show the shader.**
`Present.cpp:322` hands the frame dumper `m_xfb_entry->texture`, which is pre-post-processing. An
OS-level capture is required.

### 7.2 The instrument had to be fixed twice first, and O10 is diagnosed

Neither defect below is in Dolphin's rendering; both are in the debug dump added early in this branch.
Both are recorded because either one silently fabricates this section's numbers.

1. **The dump fired on the black boot screen.** The one-shot budget spends itself on the first frame
   the chain runs, which on a fresh boot is the console's black screen, so the first pair came back
   bit-exact zero — mean 0.000, max byte 0. Fixed in `bc6e7e1ccd` by adding
   `LibrashaderDumpChainDelayFrames` (default 0, deliberately not in the UI), counting only frames
   seen while the flag is set.
2. **The chain output dump is not RGBA8, and reading it as RGBA8 fabricates a plausible number.** The
   chain output target inherits the backbuffer's format (`LibrashaderPostProcessing.cpp:180,367`) and
   this host's swapchain is 10-bit. `AbstractTexture::Save` builds an **RGBA8** readback staging
   texture regardless of the source format, copies into it, and hands the bytes to the PNG encoder
   unconverted — so the file is a valid PNG full of misread `A2B10G10R10` words. It does not look
   broken. Read that way, the passthrough control measured **160.0%** of its own input, and crt-royale
   appeared to be 1.72× brighter than the screen, which is what an earlier pass of this task
   misdiagnosed as a downstream blit defect. The giveaway is alpha: an opaque 10-bit image read as
   RGBA8 has alpha in exactly **192..255**, because a 2-bit alpha of 3 occupies the top two bits of
   the fourth byte. `chain-dump-mean.py --rgb10a2-output` unpacks it; unpacked, the passthrough
   reproduces its input to within 0.05/255 with alpha == 3 everywhere.

**The same defect takes Dolphin down on D3D12**, which is almost certainly O10 — the never-diagnosed
crash where setting `LibrashaderDumpChainImages` killed Dolphin shortly after the first dump on D3D11.

What was observed on 2026-09-18, separating evidence from inference. Observed: with the dump armed on
D3D12, the **input** dump line is written (that texture really is RGBA8) and the log ends there — no
output-dump line, and no window by the next capture 14 s later; the byte-identical run without the
dump flag survived all three captures and logged no error. So the fatal step is the **output** dump's
readback, not rendering and not the input dump. Inferred, not verified at the API level: the cause is
item 2's cross-format staging copy, since D3D's `CopyTextureRegion` requires copy-compatible formats
while Vulkan's image-to-buffer copy is byte-legal at 4 bytes per pixel and so succeeds with the wrong
meaning instead of failing. Nobody has yet read the D3D12 debug layer's own message for this. O10 was
not re-run on D3D11 here, but the log kept from the run that first hit it (`E:\work\log-d3d11-dump.txt`
on the UAT host) shows the **same signature** — an input-dump line and no output-dump line — so on the
evidence available the two are one defect rather than two.

**Not fixed here.** The dump is a debug-only, UI-hidden, default-off option, and the two hypotheses
this task was asked to test (a repeating per-frame readback when `RunFrame` fails; `Save` disturbing
D3D11 state mid-frame) are both refuted by the above, which was the diagnosis asked for. The fix —
convert to RGBA8 before saving, or refuse and log the format — is a scope decision rather than part of
resolving this section. Carried forward as **O11**.

### 7.3 UAT finding 1: D3D12 + slang shader renders a black screen

**Not reproducible on this build.** Run on 2026-09-18 at `bc6e7e1ccd`, three legs, Metroid Prime,
D3D12, validation layer on:

| leg | executor | preset | log evidence | result |
|---|---|---|---|---|
| 1 | librashader | crt-royale | `Direct3D 12 filter chain created` | renders; 292 distinct sampled colours |
| 2 | librashader | crt-geom | `Direct3D 12 filter chain created` | renders; 418 distinct sampled colours |
| 3 | built-in | crt-geom | `librashader unavailable (…); using the built-in post-processor.` | renders; 421 distinct sampled colours |

Leg 3 renamed the build tree's `librashader.dll` aside and back, and is the **first observation of
Task 10's availability-driven fallback on Windows** rather than an argument about it: the fallback
fired, named the reason, and the built-in executor rendered the preset. No errors or warnings in any
leg's log.

One limit of this matrix: leg 3 exercised the built-in executor with **crt-geom**, not crt-royale, so
§2.1's specific case — a pass declaring 9 samplers against D3D12's 8-SRV utility root signature — was
not re-tested. §2.1 stands as written and is unaffected by these three legs.

**No commit is claimed as the fix.** Tasks 1, 9a, 9b and 9d all touched D3D12 correctness on this
branch and any of them could plausibly account for it, but the mechanism was never identified, so this
is reported as unreproducible rather than fixed. If it returns, this matrix is the baseline to
re-run.

## 8. Risks

- **Rebuilding librashader with four runtimes** enlarges the shipped binary — the current
  Vulkan-only DLL is 8.6 MB and statically linked DXC is not small. Mitigated by §2.5/§2.6:
  both D3D runtimes are proven on the target GPU and need no extra DLL.
- **Metal and OpenGL runtimes are unproven here.** Neither was exercised by the CLI probe.
  `runtime-metal` pulls `__cbindgen_internal_objc`, and librashader's GL runtime targets
  GL 3.3/4.6 while macOS caps at 4.1 — either could fail to build or to initialise. If one
  does, the §4.3 rule degrades that backend to the built-in executor rather than breaking
  it, which is exactly the path finding 1 lives on — so §2.1's fixes are not optional.
- **Hook 5 (state reconciliation) is the likeliest source of new bugs**, because a missed
  invalidation shows up as corruption in unrelated later draws. PCSX2's D3D12 sequence is
  the reference to follow literally.
- **Dropping chains is user-visible.** Anyone who built a multi-preset chain loses entries.
  On the librashader path they had already lost them silently.

## 9. Verification

- Unit tests for the loader (availability, symbol resolution, error formatting), parameter
  enumeration and override round-tripping, and the picker's tree construction and filtering —
  all CPU-only, no GPU or Qt event loop.
- Full unit suite green on macOS arm64 and Windows x64 (the current baselines are 1143/1144
  and 1455/1456, one environment-gated skip on each).
- Per-backend smoke on the Windows host: crt-royale on D3D11, D3D12 and Vulkan.
- The §7 instrumentation dump compared against the photometric table before finding 3 is
  called resolved.
