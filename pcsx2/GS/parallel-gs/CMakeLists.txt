# SPDX-FileCopyrightText: 2024 Arntzen Software AS
# SPDX-FileContributor: Hans-Kristian Arntzen
# SPDX-FileContributor: Runar Heyer
# SPDX-License-Identifier: LGPL-3.0+

if (POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

if (NOT CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

cmake_minimum_required(VERSION 3.18)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 99)
project(parallel-gs LANGUAGES CXX C)

if (CMAKE_COMPILER_IS_GNUCXX OR (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"))
    set(PARALLEL_GS_CXX_FLAGS -Wshadow -Wall -Wextra -Wno-comment -Wno-missing-field-initializers -Wno-empty-body)
    if (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        set(PARALLEL_GS_CXX_FLAGS ${PARALLEL_GS_CXX_FLAGS} -Wno-backslash-newline-escape)
    endif()
    if (NOT (${CMAKE_BUILD_TYPE} MATCHES "Release"))
        message("Enabling frame pointer for profiling/debug.")
        set(PARALLEL_GS_CXX_FLAGS ${PARALLEL_GS_CXX_FLAGS} -fno-omit-frame-pointer)
    endif()
elseif (MSVC)
    set(PARALLEL_GS_CXX_FLAGS /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005 /MP /DNOMINMAX /fp:precise)
endif()

option(PARALLEL_GS_STANDALONE "Build parallel-gs as a library." OFF)
option(PARALLEL_GS_DEBUG "Add extra debug logging." OFF)

set(GRANITE_FAST_MATH OFF CACHE BOOL "Fast math" FORCE)
set(GRANITE_VULKAN_FOSSILIZE OFF CACHE BOOL "" FORCE)
# Vendored libretro subset: RenderDoc API capture is not compiled in the
# libretro core (the Makefile build never defines GRANITE_RENDERDOC_CAPTURE;
# device.cpp provides stub implementations under #ifndef). Force it off so the
# vendored tree, which omits renderdoc_capture.cpp, matches the Makefile set.
set(GRANITE_RENDERDOC_CAPTURE OFF CACHE BOOL "" FORCE)
# Vendored libretro subset: DXGI interop (GRANITE_VULKAN_DXGI_INTEROP) defaults
# ON and, on WIN32, pulls wsi_dxgi.cpp into granite-vulkan. The libretro core
# integrates with the frontend's swapchain via retro_vulkan_* and never uses
# Vulkan::WSI, so wsi.cpp / wsi_dxgi.cpp are not vendored. Force it off.
set(GRANITE_VULKAN_DXGI_INTEROP OFF CACHE BOOL "" FORCE)

# Vendored libretro subset: one configuration, and it is the lean one.
#
# This used to branch on PARALLEL_GS_STANDALONE, with the OFF path -- the one
# the libretro core takes -- turning everything ON: the shader manager and its
# runtime GLSL compiler, SPIRV-Cross reflection, the higher-level renderer,
# system handles, and GRANITE_SHIPPING OFF, on an "SDL" platform.  None of that
# is in the core.  Makefile.common builds 39 Granite/parallel-gs translation
# units with -DGRANITE_SHIPPING and no other GRANITE_* define, links, and runs;
# CMake was compiling those 39 plus six more (managers/shader_manager.cpp,
# managers/resource_manager.cpp, texture/memory_mapped_texture.cpp,
# texture/texture_files.cpp, texture/texture_decoder.cpp, mesh/meshlet.cpp) and
# dragging in granite-filesystem, granite-threading, granite-rapidjson and
# granite-stb behind GRANITE_VULKAN_SYSTEM_HANDLES.  So the two build systems
# were producing different binaries from the same tree, which is the thing the
# per-commit both-build-systems rule exists to prevent.
#
# parallel-gs gets its shaders from slangmosh as pre-compiled SPIR-V, so the
# shader manager and the runtime compiler have nothing to do; pcsx2's own
# GS/Renderers/Vulkan gets its SPIR-V pre-compiled too (tools/vk_spirv.py), and
# there is no glslang in the tree.  GRANITE_PLATFORM is "libretro" rather than "null" or "SDL":
# Granite's own CMakeLists already knows that platform, and it is what selects
# -fvisibility=hidden and position-independent code for a core.
set(GRANITE_RENDERER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SPIRV_CROSS OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SYSTEM_HANDLES OFF CACHE BOOL "" FORCE)
set(GRANITE_SHADER_COMPILER_OPTIMIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_SHIPPING ON CACHE BOOL "" FORCE)
set(GRANITE_PLATFORM "libretro" CACHE STRING "" FORCE)

add_subdirectory(Granite EXCLUDE_FROM_ALL)

# Vendored libretro subset: only the gs library is built. The analog, dump,
# sandbox and tools subdirectories are parallel-gs's own standalone debug /
# trace-replay utilities and are not part of the libretro core.
add_subdirectory(gs)
