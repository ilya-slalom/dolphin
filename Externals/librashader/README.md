# librashader

**Version:** `librashader-cache-v0.12.0`  
**Upstream:** https://github.com/SnowflakePowered/librashader  
**License:** MPL-2.0 OR GPL-3.0-only

## Build Configuration

This directory contains prebuilt shared libraries and C headers for librashader's Vulkan runtime (Android arm64, macOS arm64, Windows x64).

### Build Environment
- **NDK:** `29.0.14206865`
- **Target:** `aarch64-linux-android`
- **API Level:** 24 (specified in linker)

### Build Command
```bash
cargo build -p librashader-capi --release --target aarch64-linux-android \
  --no-default-features --features runtime-vulkan
```

### Cargo Configuration (`.cargo/config.toml`)
```toml
[target.aarch64-linux-android]
linker = "aarch64-linux-android24-clang"
```

### Environment Variables
```bash
export AR_aarch64_linux_android=llvm-ar
export CC_aarch64_linux_android=aarch64-linux-android24-clang
export CXX_aarch64_linux_android=aarch64-linux-android24-clang++
export RANLIB_aarch64_linux_android=llvm-ranlib
```

### Artifact Details
- **Library:** `librashader.so` (~13.5 MB)
- **ABI:** 2
- **API:** 5
- **Headers:** `librashader.h`, `librashader_ld.h`

The `cargo build` output is named `liblibrashader_capi.so`; it is vendored as
`librashader.so` because `librashader_ld.h` loads the runtime with a hardcoded
`dlopen("librashader.so", RTLD_LAZY)` on Android/Linux. The file carries no
SONAME, so the rename is safe — nothing references it by an embedded name.

### Dependencies
The prebuilt `librashader.so` requires `libc++_shared.so` as a NEEDED dependency. Both are vendored into `Source/Android/app/src/main/jniLibs/arm64-v8a/` to ensure runtime availability. Dolphin builds with `ANDROID_STL=c++_static`, so `libc++_shared.so` is not otherwise present in the APK. The two libc++ instances do not clash because librashader's ABI boundary is plain C.

### Architecture Support
Only `arm64-v8a` is supported. The `x86_64` ABI correctly falls back to Dolphin's built-in post-processing.

## Desktop Builds

### macOS arm64

**Build command** (executed on macOS arm64):
```bash
rustup target add aarch64-apple-darwin
git clone --branch librashader-cache-v0.12.0 --depth 1 \
    https://github.com/SnowflakePowered/librashader /tmp/librashader
cd /tmp/librashader
cargo build -p librashader-capi --release --target aarch64-apple-darwin \
    --no-default-features --features runtime-vulkan
```

**Artifact:**
- **Path:** `lib/macos-arm64/librashader.dylib`
- **Size:** 11,335,856 bytes
- **Source:** `target/aarch64-apple-darwin/release/liblibrashader_capi.dylib` (renamed)

### Windows x64

**Build command** (executed on Windows x64 with VS 18 Community):
```batch
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set RUSTC=C:\Users\Ilya\.rustup\toolchains\stable-x86_64-pc-windows-msvc\bin\rustc.exe
cd /d C:\src\librashader
"C:\Users\Ilya\.rustup\toolchains\stable-x86_64-pc-windows-msvc\bin\cargo.exe" build -p librashader-capi --release --target x86_64-pc-windows-msvc --no-default-features --features runtime-vulkan
```

**Note:** The build invokes the real toolchain binaries directly rather than `rustup run stable cargo` because rustup shims in `C:\Users\Ilya\.cargo\bin\` cannot be launched from ssh sessions (error 448 / "untrusted mount point").

**Artifact:**
- **Path:** `lib/windows-x64/librashader.dll`
- **Size:** 8,611,840 bytes
- **Source:** `target\x86_64-pc-windows-msvc\release\librashader_capi.dll` (renamed)

Both desktop builds use the same flags as the Android build (`--no-default-features --features runtime-vulkan`) and produce binaries with ABI 2 / API 5.

## Local Modifications

The headers are vendored unmodified. `librashader_ld.h` is kept for reference only — nothing in Dolphin includes it.

Dolphin resolves the C API itself, in `Source/Core/VideoCommon/PostProcessing/LibrashaderLoader.cpp`. The header cannot be used as intended here because it declares a single `libra_instance_t` whose members are gated by the `LIBRA_RUNTIME_*` macros and loads it from a `static inline` function: two translation units including it with different runtime sets would disagree on that struct's layout, and driving every backend from one translation unit would mean including the Vulkan, D3D11, D3D12, OpenGL and Metal headers together. Dolphin therefore includes plain `librashader.h` for the `PFN_libra_*` typedefs and resolves symbols through one `dlopen`/`LoadLibraryW`, which also lets it supply the absolute packaged path that the header's bare-name load cannot find inside a macOS `.app` bundle or a Windows install directory.

An earlier revision of this tree patched the header's three `_LIBRASHADER_LOAD` defines to be overridable. That patch has been reverted; do not reintroduce it.

**macOS dylib install name:**  
The macOS dylib's `LC_ID_DYLIB` install name still references the build-time path (`/private/tmp/librashader/target/aarch64-apple-darwin/release/deps/liblibrashader_capi.dylib`). This is harmless because `dlopen` with an absolute path ignores the install name. Do not rewrite it with `install_name_tool` unless `POSTPROCESS_BUNDLE=ON` (default OFF) requires it — the current packaging has been verified to work as-is.
