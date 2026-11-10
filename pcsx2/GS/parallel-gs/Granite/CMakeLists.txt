# Granite as the libretro core carries it: the Vulkan backend
# (granite-vulkan), the utilities it uses (granite-util) and the volk
# loader. Nothing else is vendored, so nothing else is configurable.
cmake_minimum_required(VERSION 3.6)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 99)
project(Granite LANGUAGES CXX C)

if (CMAKE_COMPILER_IS_GNUCXX OR (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"))
    set(GRANITE_CXX_FLAGS -Wshadow -Wall -Wextra -Wno-comment -Wno-missing-field-initializers -Wno-empty-body)
    if (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        set(GRANITE_CXX_FLAGS ${GRANITE_CXX_FLAGS} -Wno-backslash-newline-escape)
    endif()
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
        set(GRANITE_CXX_FLAGS ${GRANITE_CXX_FLAGS} -msse3)
    endif()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")
elseif (MSVC)
    set(GRANITE_CXX_FLAGS /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005 /MP /fp:precise)
endif()

# Linked into a shared core.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

function(add_granite_internal_lib NAME)
    add_library(${NAME} STATIC ${ARGN})
    target_compile_options(${NAME} PRIVATE ${GRANITE_CXX_FLAGS})
endfunction()

add_subdirectory(third_party)
add_subdirectory(util)
add_subdirectory(vulkan)
