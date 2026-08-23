# librashader

**Version:** `librashader-cache-v0.12.0`  
**Upstream:** https://github.com/SnowflakePowered/librashader  
**License:** MPL-2.0 OR GPL-3.0-only

## Build Configuration

This directory contains prebuilt arm64 shared libraries and C headers for librashader's Vulkan runtime.

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
