# Release Notes — Next Fork Release

## User-Visible Changes

### Post-Processing

1. **The Post-Processing Renderer setting has been removed.** librashader is now used wherever it is available (desktop Windows, desktop macOS, Android arm64). On platforms without a vendored librashader binary (desktop Linux, Android x86_64), Dolphin automatically falls back to the built-in post-processor.

2. **Shader chains are no longer supported.** The post-processing shader field now accepts a single preset path instead of a semicolon-separated chain. Existing configurations holding a multi-preset chain will resolve to the first entry in the chain. This matches PCSX2's single-preset model and reflects what librashader was already doing silently on the librashader path.

3. **The preset picker is now a filterable tree.** Instead of a flat combo box with all 30,864 presets from the full libretro pack, the post-processing shader picker presents a hierarchical tree with a recursive filter box. Presets declaring `#pragma parameter` directives are now editable through a new *Parameters…* dialog with per-parameter sliders, spin boxes, and per-row *Reset* buttons. Edited parameter values are persisted per preset and survive restarts.

## Corrections to Previous Release

The `2606-fork-2` release notes claimed native `GenerateMipmaps` support for "Metal, OpenGL, D3D11 and D3D12". The correct list is **Metal, OpenGL, and D3D11**. D3D12 has no `GenerateMipmaps` implementation and was incorrectly included.
