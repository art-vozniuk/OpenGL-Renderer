cmake_minimum_required(VERSION 3.25)

project(OpenGLRenderer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OPENGL_RENDERER_BUILD_SANDBOX "Build the Sandbox executable" ON)

# ============================================================
#  Model loader (tinygltf, header-only) — shared between targets
# ============================================================
include(FetchContent)
set(TINYGLTF_HEADER_ONLY ON  CACHE INTERNAL "")
set(TINYGLTF_INSTALL     OFF CACHE INTERNAL "")
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.9.3
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(tinygltf)

# ============================================================
#  WebGPU (Dawn on native via Metal/Vulkan/D3D12, browser API on web)
#
#  We use eliemichel/WebGPU-distribution which exposes a single `webgpu`
#  CMake target that:
#    - on emscripten: passes -sUSE_WEBGPU=1 and provides <webgpu/webgpu.h>
#    - on native: pulls Dawn, builds it, links its Metal/Vulkan backend
#  This way the same C source compiles on both targets without ifdefs.
#
#  glfw3webgpu glues a GLFWwindow to a wgpu::Surface — Dawn doesn't
#  ship that helper itself.
# ============================================================
# Pick Dawn (Google's reference WebGPU implementation) over wgpu-native
# (Rust). Dawn is what Chromium ships, more thoroughly tested on Mac/Metal,
# statically linkable so no DYLD path gymnastics, and crucially its C API
# is in lock-step with glfw3webgpu's expectations. wgpu-native v24+ uses
# WGPUStringView which the glfw3webgpu glue doesn't grok.
set(WEBGPU_BACKEND "DAWN" CACHE STRING "" FORCE)
# Use a pre-cached clone of WebGPU-distribution at /tmp/webgpu-cache to avoid
# repeated `git fetch` failures inside FetchContent (we hit transient
# `invalid index-pack output` errors on this network). Falls back to a
# fresh clone if the cache directory doesn't exist.
if(EXISTS "/tmp/webgpu-cache/webgpu-src/CMakeLists.txt")
    FetchContent_Declare(webgpu SOURCE_DIR "/tmp/webgpu-cache/webgpu-src")
else()
    FetchContent_Declare(
        webgpu
        GIT_REPOSITORY https://github.com/eliemichel/WebGPU-distribution
        GIT_TAG        v0.3.0-gamma
        GIT_SHALLOW    ON
    )
endif()
FetchContent_MakeAvailable(webgpu)

# glfw3webgpu — only needed on native (emscripten provides the glue).
if(NOT EMSCRIPTEN)
    if(EXISTS "/tmp/webgpu-cache/glfw3webgpu-src/CMakeLists.txt")
        FetchContent_Declare(glfw3webgpu SOURCE_DIR "/tmp/webgpu-cache/glfw3webgpu-src")
    else()
        FetchContent_Declare(
            glfw3webgpu
            GIT_REPOSITORY https://github.com/eliemichel/glfw3webgpu
            GIT_TAG        v1.3.0-alpha
            GIT_SHALLOW    ON
        )
    endif()
    FetchContent_MakeAvailable(glfw3webgpu)
endif()


# ============================================================
#  NATIVE-ONLY dependencies (GLFW window — GLAD removed with OpenGL)
# ============================================================
if(NOT EMSCRIPTEN)
    set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan ON)

    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    set(GLFW_VULKAN_STATIC  OFF CACHE BOOL "" FORCE)

    # GLFW 3.4 — needed for `glfwGetPlatform`, used by glfw3webgpu's
    # cross-platform surface picker. The vendored GLFW under
    # Engine/vendor/GLFW is 3.3, predates that API.
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw
        GIT_TAG        3.4
        GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(glfw)
endif()

# ============================================================
#  Shared interface libraries (header-only, both targets)
# ============================================================
add_library(vendor_glm INTERFACE)
target_include_directories(vendor_glm SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/glm")

add_library(vendor_spdlog INTERFACE)
target_include_directories(vendor_spdlog SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/spdlog/include")

# ============================================================
#  ImGui (same sources for native + web; backend auto-adapts)
# ============================================================
add_library(ImGui STATIC
    Engine/vendor/imgui/imgui.cpp
    Engine/vendor/imgui/imgui_demo.cpp
    Engine/vendor/imgui/imgui_draw.cpp
    Engine/vendor/imgui/imgui_widgets.cpp
)
target_include_directories(ImGui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/imgui/examples"
)
target_include_directories(ImGui SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/imgui/examples"
)

# ============================================================
#  Engine library
# ============================================================
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/stb_image/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/stb_image/*.h"
)

add_library(Engine STATIC ${ENGINE_SOURCES})

target_include_directories(Engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/src/Engine"
)

# ---- compile definitions (platform-aware) ----
# WebGPU uses [0, 1] clip-space Z (like D3D / Vulkan), unlike OpenGL's
# [-1, 1]. Force GLM's perspective/ortho to emit ZO matrices everywhere
# so downstream code doesn't have to remember which projection convention
# its renderer expects.
if(NOT EMSCRIPTEN)
    target_compile_definitions(Engine
        PUBLIC
            ENGINE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
            GLFW_INCLUDE_NONE
            GLM_FORCE_DEPTH_ZERO_TO_ONE
            $<$<CONFIG:Debug>:DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:RELEASE>
    )
else()
    target_compile_definitions(Engine
        PUBLIC
            ENGINE_ASSETS_DIR="/assets"
            GLM_FORCE_DEPTH_ZERO_TO_ONE
            $<$<CONFIG:Debug>:DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:RELEASE>
            # spdlog's bundled fmt defines fmt::char8_t which triggers
            # a missing std::char_traits specialisation in Emscripten's libc++.
            # Disabling char8_t support in fmt avoids the error entirely.
            FMT_USE_CHAR8_T=0
    )
endif()

# ---- include paths ----
target_include_directories(Engine SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/imgui/examples"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/stb_image"
)

# tinygltf header lives in the fetched source tree; shared between both targets.
target_include_directories(Engine SYSTEM PUBLIC
    "${tinygltf_SOURCE_DIR}"
)

# GLFW headers come from the FetchContent target on native and from the
# emscripten contrib port on web — no manual include path needed.

# ---- link libraries ----
# WebGPU branch: Engine consumes the `webgpu` target on both native and web.
# On native we also link GLFW + glfw3webgpu for window/surface creation.
# Glad / OpenGL::GL are gone with the OpenGL platform layer.
if(NOT EMSCRIPTEN)
    target_link_libraries(Engine
        PUBLIC
            ImGui
            webgpu
            glfw
            glfw3webgpu
            vendor_glm
            vendor_spdlog
    )
else()
    target_link_libraries(Engine
        PUBLIC
            ImGui
            webgpu
            vendor_glm
            vendor_spdlog
    )
endif()

# ============================================================
#  HelloTriangle — minimal stand-alone WebGPU smoke test
#
#  Doesn't link Engine; just GLFW + WebGPU. If this builds and runs,
#  the Dawn / glfw3webgpu / WGSL toolchain is healthy and the larger
#  Engine port has no infrastructure surprises waiting.
# ============================================================
add_executable(HelloTriangle
    Sandbox/src/HelloTriangle/HelloTriangleApp.cpp
)
target_link_libraries(HelloTriangle PRIVATE webgpu)
if(NOT EMSCRIPTEN)
    target_link_libraries(HelloTriangle PRIVATE glfw glfw3webgpu)
endif()
if(EMSCRIPTEN)
    set_target_properties(HelloTriangle PROPERTIES SUFFIX ".html")
    target_link_options(HelloTriangle PRIVATE
        --use-port=contrib.glfw3
        -sUSE_WEBGPU=1
        -sASYNCIFY  # for emscripten_sleep in adapter/device request loop
        -sALLOW_MEMORY_GROWTH=1
    )
    target_compile_options(HelloTriangle PRIVATE
        --use-port=contrib.glfw3
    )
endif()


# ============================================================
#  Sandbox executable
# ============================================================
if(OPENGL_RENDERER_BUILD_SANDBOX)
    file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Sandbox/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Sandbox/src/*.h"
    )
    # HelloTriangle has its own main() and doesn't link Engine; remove its
    # source from the Sandbox glob so we don't end up with two main() symbols.
    list(FILTER SANDBOX_SOURCES EXCLUDE REGEX "Sandbox/src/HelloTriangle/.*")

    add_executable(Sandbox ${SANDBOX_SOURCES})
    target_link_libraries(Sandbox PRIVATE Engine)
    target_compile_definitions(Sandbox PRIVATE
        $<$<CONFIG:Debug>:DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:RELEASE>
    )

    if(EMSCRIPTEN)
        set_target_properties(Sandbox PROPERTIES SUFFIX ".html")

        # Assets directory to embed in .data bundle
        set(ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")

        target_link_options(Sandbox PRIVATE
            # WebGL 2 = OpenGL ES 3.0
            -sMAX_WEBGL_VERSION=2
            -sMIN_WEBGL_VERSION=2
            # GLFW via official Emscripten contrib port
            --use-port=contrib.glfw3
            # Memory
            -sALLOW_MEMORY_GROWTH=1
            -sINITIAL_MEMORY=134217728    # 128 MB
            -sMAXIMUM_MEMORY=1073741824   # 1 GB
            -sSTACK_SIZE=5242880          # 5 MB
            # Embed assets into .data file (loaded before main())
            --preload-file ${ASSETS_DIR}@/assets
            # Misc
            -sEXPORTED_RUNTIME_METHODS=cwrap,ccall
            -sASSERTIONS=1
        )

        # Compiler flags (applied to all TUs in the Sandbox target)
        target_compile_options(Sandbox PRIVATE
            --use-port=contrib.glfw3
            -O2
        )

        # Post-build patch that hides Emscripten's default UI, re-routes .data
        # loads through /renderer/data/, and forwards load progress to the
        # parent frame via postMessage. Lives in-repo so any build path
        # (local cmake, CI workflow, helper scripts) produces identical HTML.
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_custom_command(TARGET Sandbox POST_BUILD
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch_sandbox_html.py
                    $<TARGET_FILE_DIR:Sandbox>/Sandbox.html
            COMMENT "Patching Sandbox.html (hide UI + postMessage hooks)"
            VERBATIM
        )
    endif()
endif()
