cmake_minimum_required(VERSION 3.25)

project(Renderer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RENDERER_BUILD_SANDBOX "Build the Sandbox executable" ON)

include(FetchContent)


# ============================================================
#  WebGPU
#
#  eliemichel/WebGPU-distribution gives us a single `webgpu` CMake
#  target that resolves to:
#    - native: Dawn (Google's reference impl, statically linked,
#      Metal under the hood on macOS)
#    - emscripten: emdawnwebgpu port (browser-side Dawn; same C API
#      shape as native, no `#ifdef` fences inside renderer code)
#
#  glfw3webgpu is the GLFWwindow -> wgpu::Surface helper, native only.
# ============================================================
if(EMSCRIPTEN)
    set(WEBGPU_BACKEND "EMDAWNWEBGPU" CACHE STRING "" FORCE)
else()
    set(WEBGPU_BACKEND "DAWN" CACHE STRING "" FORCE)
endif()

# Optional: pre-cached clones at /tmp/webgpu-cache short-circuit
# FetchContent's git fetch (handy when CI / network are flaky).
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
#  GLFW (native only). 3.4 is required by glfw3webgpu's surface picker
#  (calls glfwGetPlatform, which 3.3 doesn't have). Browser GLFW comes
#  from the emscripten contrib.glfw3 port.
# ============================================================
if(NOT EMSCRIPTEN)
    set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan ON)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    set(GLFW_VULKAN_STATIC  OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw
        GIT_TAG        3.4
        GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(glfw)
endif()


# ============================================================
#  Header-only vendored libs
# ============================================================
add_library(vendor_glm INTERFACE)
target_include_directories(vendor_glm SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/glm")

# Logging stack: spdlog backed by std::format (no bundled fmt).
#
# Both the bundled fmt (in spdlog 1.x) and external fmt 10.x/11.x trip
# a `consteval` compile-time format-string check that emscripten clang
# refuses under C++23's stricter constant-eval rules. C++20+ libc++
# already ships `std::format`, so we side-step fmt entirely by setting
# SPDLOG_USE_STD_FORMAT — spdlog uses the standard library directly.
set(SPDLOG_BUILD_PIC      ON  CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT ON  CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE  OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog
    GIT_TAG        v1.15.1
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(spdlog)


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

target_include_directories(Engine SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/stb_image"
)

# WebGPU uses [0, 1] clip-space Z (D3D / Vulkan style). Force GLM's
# perspective / ortho to emit ZO matrices everywhere so we don't have
# to remember which projection convention each call site expects.
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
            # Disable spdlog/fmt char8_t — it triggers a missing
            # std::char_traits specialisation in Emscripten's libc++.
            FMT_USE_CHAR8_T=0
    )
endif()

# GLFW headers come from the FetchContent target (native) or the
# emscripten contrib port (web) — no manual include path needed.

if(NOT EMSCRIPTEN)
    target_link_libraries(Engine
        PUBLIC
            webgpu
            glfw
            glfw3webgpu
            vendor_glm
            spdlog::spdlog
    )
else()
    target_link_libraries(Engine
        PUBLIC
            webgpu
            vendor_glm
            spdlog::spdlog
    )
endif()


# ============================================================
#  Sandbox
# ============================================================
if(RENDERER_BUILD_SANDBOX)
    file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Sandbox/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Sandbox/src/*.h"
    )

    add_executable(Sandbox ${SANDBOX_SOURCES})
    target_link_libraries(Sandbox PRIVATE Engine)
    target_compile_definitions(Sandbox PRIVATE
        $<$<CONFIG:Debug>:DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:RELEASE>
    )

    if(EMSCRIPTEN)
        set_target_properties(Sandbox PROPERTIES SUFFIX ".html")
        set(ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")

        target_link_options(Sandbox PRIVATE
            # ASYNCIFY lets the C++ adapter / device bootstrap spin on a
            # `done` flag (emscripten_sleep yields to the JS event loop).
            -sASYNCIFY
            # FETCH wires up emscripten_fetch (used to download .splat scenes
            # at runtime from S3). Together with ASYNCIFY this lets the scene
            # constructor block on the download via emscripten_sleep loops.
            -sFETCH=1
            --use-port=contrib.glfw3
            -sALLOW_MEMORY_GROWTH=1
            -sINITIAL_MEMORY=134217728    # 128 MB
            -sMAXIMUM_MEMORY=1073741824   # 1 GB
            -sSTACK_SIZE=5242880          # 5 MB
            --preload-file ${ASSETS_DIR}@/assets
            -sEXPORTED_RUNTIME_METHODS=cwrap,ccall
            # JS bridge for the on-screen mobile joysticks: vinput_set_move
            # holds the left-stick vector + Q/E buttons; vinput_apply_look
            # accumulates right-stick swipe deltas.
            -sEXPORTED_FUNCTIONS=_main,_vinput_set_move,_vinput_apply_look
            -sASSERTIONS=1
        )
        target_compile_options(Sandbox PRIVATE
            --use-port=contrib.glfw3
            -O2
        )

        # Post-build: hide the default Emscripten UI, re-route .data
        # to /renderer/data/, forward load progress to the parent frame.
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
