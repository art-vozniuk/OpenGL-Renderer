# 3D Renderer
<img width="1460" height="839" alt="image" src="https://github.com/user-attachments/assets/e9491f87-3b90-42d1-9013-a9a49f0e4b50" />

Real-time 3D scene renderer built around a custom C++ engine.

## Platforms

- **macOS** — native OpenGL build with Xcode / AppleClang
- **Web** — compiled to WebAssembly via Emscripten, runs in the browser using WebGL 2

## Features

- Phong lighting
- Normal mapping
- Cubemaps / skybox
- Post-processing with framebuffer passes
- Asset loading through Assimp (native) and tinygltf (web)
- ImGui runtime controls (native)

## Requirements

- CMake 3.25+
- A C++17 compiler

**Native (macOS):**
- Xcode / AppleClang

**Web (Emscripten):**
- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)

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
cmake --build build-web --parallel $(sysctl -n hw.ncpu)
```

Output: `build-web/Sandbox.html`, `.js`, `.wasm`, `.data`

## Controls

- Hold right mouse button and use `W`, `A`, `S`, `D` to move the camera
- `Q` / `E` to move down / up
- Scroll to adjust speed
- Use the ImGui `Settings` panel for renderer and scene controls (native only)
