# Mini OpenGL Renderer

OpenGL scene renderer and sandbox application built around a small custom engine.

Current focus:
- CMake-based workflow
- macOS support

Features:
- Phong lighting
- Normal mapping
- Cubemaps / skybox
- Post-processing with framebuffer passes
- Asset loading through Assimp
- ImGui runtime controls

## Requirements

- CMake 3.25+
- A C++17 compiler
- macOS with Xcode / AppleClang

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target Sandbox -j 8
```

Run:

```bash
./build/Sandbox
```

## Controls

- Hold right mouse button and use `W`, `A`, `S`, `D` to move the camera
- Use the ImGui `Settings` panel for renderer and scene controls
