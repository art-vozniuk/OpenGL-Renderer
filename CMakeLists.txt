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
#  NATIVE-ONLY dependencies
# ============================================================
if(NOT EMSCRIPTEN)
    set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan ON)

    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    set(GLFW_VULKAN_STATIC  OFF CACHE BOOL "" FORCE)

    set(BUILD_SHARED_LIBS           OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS   OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES        OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT            ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ZLIB           OFF CACHE BOOL "" FORCE)
    set(BUILD_DOCS                  OFF CACHE BOOL "" FORCE)

    add_subdirectory(Engine/vendor/GLFW EXCLUDE_FROM_ALL)
    add_subdirectory(Engine/vendor/assimp EXCLUDE_FROM_ALL)

    if(TARGET assimp)
        target_compile_definitions(assimp PRIVATE
            ASSIMP_BUILD_NO_OWN_ZLIB
            MINIZ_NO_ZLIB_COMPATIBLE_NAMES
        )
    endif()

    find_package(OpenGL REQUIRED)

    add_library(Glad STATIC Engine/vendor/Glad/src/glad.c)
    target_include_directories(Glad SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/Glad/include")
endif()

# ============================================================
#  WEB — tinygltf via FetchContent
# ============================================================
if(EMSCRIPTEN)
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
if(NOT EMSCRIPTEN)
    target_compile_definitions(Engine
        PUBLIC
            ENGINE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
            GLFW_INCLUDE_NONE
            $<$<CONFIG:Debug>:DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:RELEASE>
            $<$<PLATFORM_ID:Darwin>:GL_SILENCE_DEPRECATION>
    )
else()
    target_compile_definitions(Engine
        PUBLIC
            ENGINE_ASSETS_DIR="/assets"
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

if(NOT EMSCRIPTEN)
    target_include_directories(Engine SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/Glad/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/GLFW/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/vendor/assimp/include"
    )
else()
    target_include_directories(Engine SYSTEM PUBLIC
        "${tinygltf_SOURCE_DIR}"
    )
endif()

# ---- link libraries ----
if(NOT EMSCRIPTEN)
    target_link_libraries(Engine
        PUBLIC
            Glad
            ImGui
            OpenGL::GL
            assimp
            glfw
            vendor_glm
            vendor_spdlog
    )
else()
    target_link_libraries(Engine
        PUBLIC
            ImGui
            vendor_glm
            vendor_spdlog
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
    endif()
endif()
