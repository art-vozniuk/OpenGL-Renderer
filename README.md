# Renderer

Real-time Gaussian Splatting renderer. Custom C++ engine on WebGPU.
Runs natively on macOS (Dawn → Metal) and in the browser (Emscripten →
emdawnwebgpu → browser WebGPU) from one source tree.

## Highlights

- **Per-frame GPU radix sort.** Five compute kernels, four byte-passes
  of LSD radix, workgroup-local stable scatter via per-thread local
  rank in shared memory. Sorts 1M+ splats every frame.
- **EWA splat projection in WGSL.** Cov3D = R · S² · Rᵀ projected to
  2D via the standard EWA Jacobian, 3-σ quad sized off the eigen-
  values, alpha-over blend with premultiplied source.
- **Storage-buffer indirection.** Splat data lives in storage buffers;
  only the small `sortedIndices` buffer is rewritten between frames.
- **Headless capture.** `GS_CAPTURE_FRAME=N GS_CAPTURE_PATH=out.png ./Sandbox`
  encodes a CopyTextureToBuffer of the swap chain on frame N, writes
  a PNG, exits — used for visual regression.

## Architecture

```
Engine/src/Engine/Renderer/
    WGPUContext            instance / surface / adapter / device / queue
    Renderer               BeginScene / OpenColorPass / EndScene
    GaussianSplatRenderer  upload, GPU sort dispatches, indirected draw
    SplatLoader            antimatter15 .splat parser
    Camera, FlyCamera

assets/shaders/
    gsplat.wgsl            vertex + fragment for the splat draw
    gsplat_sort.wgsl       compute kernels for the GPU radix sort

assets/splat/
    train.splat            Inria "Train" scene

Sandbox/src/
    SandboxApp.cpp         entry — picks a scene by id, pushes a layer
    Scenes/                GaussianSplatScene + scene registry
```

## Requirements

- CMake 3.25+, C++17 compiler
- macOS native: Xcode / AppleClang
- Web: [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
- WebGPU-capable browser (Chrome 113+, Safari 18+, Firefox 141+)

## Build

### Native (macOS)

```bash
cmake -S . -B build
cmake --build build --target Sandbox -j 8
./build/Sandbox
```

### Web (Emscripten)

```bash
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web --target Sandbox -j 8
```

Output: `build-web/Sandbox.{html,js,wasm,data}`. Serve over HTTP and
open `Sandbox.html?scene=gsplat`.

## Controls

- Hold left mouse button + `W`/`A`/`S`/`D` to fly the camera
- `Q` / `E` to descend / ascend
- Scroll to adjust speed
