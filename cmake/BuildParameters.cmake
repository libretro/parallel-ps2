# Extra preprocessor definitions that will be added to all pcsx2 builds
set(PCSX2_DEFS "")

execute_process(
    COMMAND git rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
add_definitions(-DGIT_VERSION="${GIT_VERSION}")

#-------------------------------------------------------------------------------
# Misc option
#-------------------------------------------------------------------------------
option(LIBRETRO "Enables building the libretro core" ON)
set(CMAKE_BUILD_PO FALSE)
set(USE_SYSTEM_LIBS OFF)
add_definitions(-D__LIBRETRO__)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(LTO_PCSX2_CORE "Enable LTO/IPO/LTCG on the subset of pcsx2 that benefits most from it but not anything else")
# iOS and tvOS are Apple but not macOS, and the difference decides what can be
# found on the host: no desktop libGL, no GLX, no framework layer meant for a
# desktop. CMake tells them apart by system name - APPLE is true for all three.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	set(APPLE_EMBEDDED TRUE)
else()
	set(APPLE_EMBEDDED FALSE)
endif()

# webOS is a Linux, and CMAKE_SYSTEM_NAME says so, but its buildroot ships EGL
# and GLES and no desktop GL - so the renderer takes the route Android and the
# Apple embedded systems take, with every entry point coming from the frontend
# and nothing linked against libGL. The SDK's compiler triple
# (aarch64-webos-linux-gnu) is what gives it away; -DWEBOS=ON also works for
# anyone driving the build by hand.
if(NOT WEBOS AND CMAKE_CXX_COMPILER MATCHES "webos")
	set(WEBOS TRUE)
endif()

#-------------------------------------------------------------------------------
# Graphical option
#-------------------------------------------------------------------------------
option(USE_OPENGL "Enable OpenGL GS renderer" ON)
option(USE_VULKAN "Enable Vulkan GS renderer" ON)

#-------------------------------------------------------------------------------
# Path and lib option
#-------------------------------------------------------------------------------
if(APPLE)
	option(OSX_USE_DEFAULT_SEARCH_PATH "Don't prioritize system library paths" OFF)
	option(SKIP_POSTPROCESS_BUNDLE "Skip postprocessing bundle for redistributability" OFF)
endif()

#-------------------------------------------------------------------------------
# Compiler extra
#-------------------------------------------------------------------------------
option(USE_ASAN "Enable address sanitizer")

if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	set(USE_CLANG_CL TRUE)
	message(STATUS "Building with Clang-CL.")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
	set(USE_CLANG TRUE)
	message(STATUS "Building with Clang/LLVM.")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(USE_GCC TRUE)
	message(STATUS "Building with GNU GCC")
elseif(MSVC)
	message(STATUS "Building with MSVC")
else()
	message(FATAL_ERROR "Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

#-------------------------------------------------------------------------------
# if no build type is set, use Devel as default
# Note without the CMAKE_BUILD_TYPE options the value is still defined to ""
# Ensure that the value set by the User is correct to avoid some bad behavior later
#-------------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE MATCHES "Debug|Devel|MinSizeRel|RelWithDebInfo|Release")
	set(CMAKE_BUILD_TYPE Devel)
	message(STATUS "BuildType set to ${CMAKE_BUILD_TYPE} by default")
endif()
# Add Devel build type
set(CMAKE_C_FLAGS_DEVEL "${CMAKE_C_FLAGS_RELWITHDEBINFO}"
	CACHE STRING "Flags used by the C compiler during development builds" FORCE)
set(CMAKE_CXX_FLAGS_DEVEL "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}"
	CACHE STRING "Flags used by the C++ compiler during development builds" FORCE)
set(CMAKE_LINKER_FLAGS_DEVEL "${CMAKE_LINKER_FLAGS_RELWITHDEBINFO}"
	CACHE STRING "Flags used for linking binaries during development builds" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS_DEVEL "${CMAKE_LINKER_FLAGS_RELWITHDEBINFO}"
	CACHE STRING "Flags used for linking modules during development builds" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEVEL "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO}"
	CACHE STRING "Flags used for linking shared libraries during development builds" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEVEL "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}"
	CACHE STRING "Flags used for linking executables during development builds" FORCE)
if(CMAKE_CONFIGURATION_TYPES)
	list(INSERT CMAKE_CONFIGURATION_TYPES 0 Devel)
endif()
mark_as_advanced(CMAKE_C_FLAGS_DEVEL CMAKE_CXX_FLAGS_DEVEL CMAKE_LINKER_FLAGS_DEVEL CMAKE_MODULE_LINKER_FLAGS_DEVEL CMAKE_SHARED_LINKER_FLAGS_DEVEL CMAKE_EXE_LINKER_FLAGS_DEVEL)

#-------------------------------------------------------------------------------
# Select the architecture
#-------------------------------------------------------------------------------
# DISABLE_ADVANCE_SIMD=ON builds the GS/IPU/SPU2 MultiISA sources three times
# (sse4/avx/avx2) and selects the best path per-CPU at runtime via
# libretro-common features_cpu,
# instead of baking a single advanced-SIMD level into the whole build. The
# sse4 tier remains the fallback on CPUs without AVX/AVX2, so this preserves
# support for SSE4-or-earlier hosts. Defaulted ON to match the Makefile's
# ENABLE_MULTI_ISA=1 default; set OFF for a single-ISA build.
option(DISABLE_ADVANCE_SIMD "Disable advance use of SIMD (SSE2+ & AVX)" ON)

# Print if we are cross compiling.
if(CMAKE_CROSSCOMPILING)
	message(STATUS "Cross compilation is enabled.")
else()
	message(STATUS "Cross compilation is disabled.")
endif()

# Architecture bitness detection
include(TargetArch)
target_architecture(PCSX2_TARGET_ARCHITECTURES)
if(${PCSX2_TARGET_ARCHITECTURES} MATCHES "x86_64")
	message(STATUS "Compiling a ${PCSX2_TARGET_ARCHITECTURES} build on a ${CMAKE_HOST_SYSTEM_PROCESSOR} host.")

	# x86_64 requires -fPIC
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)

	if(NOT DEFINED ARCH_FLAG AND NOT MSVC)
		if (DISABLE_ADVANCE_SIMD)
			set(ARCH_FLAG "-msse -msse2 -msse4.1 -mfxsr")
		else()
			# Can't use march=native on Apple Silicon.
			if(NOT APPLE OR "${CMAKE_HOST_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
				set(ARCH_FLAG "-march=native")
			endif()
		endif()
	elseif(NOT DEFINED ARCH_FLAG AND USE_CLANG_CL)
		set(ARCH_FLAG "-msse4.1")
	endif()
	list(APPEND PCSX2_DEFS _ARCH_64=1 _M_X86=1)
	set(_ARCH_64 1)
	set(_M_X86 1)

	# SSE4.1 is not set by MSVC, it uses _M_SSE instead.
	# TODO/FIXME (LIBRETRO) - we want to make this runtime optional in the future instead of compile time
	if(MSVC)
		list(APPEND PCSX2_DEFS __SSE4_1__=1)
	endif()
elseif(${PCSX2_TARGET_ARCHITECTURES} MATCHES "aarch64")
	# AArch64 (arm64) port, Phase A: get configure past the arch gate. NEON is
	# baseline in ARMv8 so no x86 SIMD flags are needed. The x86 recompilers and
	# x86 intrinsics are still compiled here and will fail to build until they
	# are guarded behind _M_X86 and the interpreters are forced (Phase B); a
	# hardware renderer (paraLLEl-GS) avoids porting the x86 GS SW rasterizer.
	message(STATUS "Compiling a ${PCSX2_TARGET_ARCHITECTURES} build on a ${CMAKE_HOST_SYSTEM_PROCESSOR} host.")

	# The libretro core is a shared object; needs -fPIC.
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)

	# Don't define _M_ARM64 (an MSVC-only predefined macro); the code keys arm64
	# off __aarch64__ too, and defining _M_ARM64 on GCC/Clang leaks into 3rdparty
	# code that treats it as "MSVC ARM64" and pulls intrin.h/__umulh.
	list(APPEND PCSX2_DEFS _ARCH_64=1)
	set(_ARCH_64 1)
	set(_M_ARM64 1)
else()
	message(FATAL_ERROR "Unsupported architecture: ${PCSX2_TARGET_ARCHITECTURES}")
endif()
string(REPLACE " " ";" ARCH_FLAG_LIST "${ARCH_FLAG}")
add_compile_options("${ARCH_FLAG_LIST}")

#-------------------------------------------------------------------------------
# Set some default compiler flags
#-------------------------------------------------------------------------------
option(USE_PGO_GENERATE "Enable PGO optimization (generate profile)")
option(USE_PGO_OPTIMIZE "Enable PGO optimization (use profile)")

if(MSVC AND NOT USE_CLANG_CL)
	add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/Zc:externConstexpr>")
endif()

if(MSVC)
	# Disable RTTI
	string(REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})

	# Disable Exceptions
	string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
else()
	add_compile_options(-pipe -fvisibility=hidden -pthread)
	# -fvisibility-inlines-hidden is a C++-only option: passed to C files it
	# is a per-TU cc1 warning on GCC, so scope it by language instead of
	# letting add_compile_options spray it at everything.
	add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>")
	if(NOT APPLE)
		# -fno-semantic-interposition matches the Makefile build: the core is
		# a shared object that never relies on symbol interposition, and
		# without it the compiler must assume its own definitions can be
		# replaced at load time, which blocks direct binding and cross-TU
		# constant folding. It is an ELF concept; Mach-O has no interposition
		# to disable, and Apple clang warns -Wunused-command-line-argument on
		# every single compile -- once per TU across the whole macOS log.
		add_compile_options(-fno-semantic-interposition)
	endif()
	add_compile_options(
		"$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
		"$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
	)
endif()

set(CONFIG_REL_NO_DEB $<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>)
set(CONFIG_ANY_REL $<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>,$<CONFIG:RelWithDebInfo>>)

if(WIN32)
	add_compile_definitions(
		$<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=2>
		$<$<CONFIG:Devel>:_ITERATOR_DEBUG_LEVEL=1>
		$<${CONFIG_ANY_REL}:_ITERATOR_DEBUG_LEVEL=0>
		_HAS_EXCEPTIONS=0
	)
	list(APPEND PCSX2_DEFS _SCL_SECURE_NO_WARNINGS _UNICODE UNICODE)
endif()

# Enable debug information in release builds for Linux.
# Makes the backtrace actually meaningful.
if(UNIX AND NOT APPLE)
	add_compile_options($<$<CONFIG:Release>:-g1>)
endif()

if(MSVC)
	# Enable PDB generation in release builds
	add_compile_options(
		$<${CONFIG_REL_NO_DEB}:/Zi>
	)
	add_link_options(
		$<${CONFIG_REL_NO_DEB}:/DEBUG>
		$<${CONFIG_REL_NO_DEB}:/OPT:REF>
		$<${CONFIG_REL_NO_DEB}:/OPT:ICF>
	)
endif()

if(USE_OPENGL)
	list(APPEND PCSX2_DEFS ENABLE_OPENGL)
endif()

if(USE_VULKAN)
	list(APPEND PCSX2_DEFS ENABLE_VULKAN)
endif()

# -Wno-attributes: "always_inline function might not be inlinable" <= real spam (thousand of warnings!!!)
# -Wno-missing-field-initializers: standard allow to init only the begin of struct/array in static init. Just a silly warning.
# Note: future GCC (aka GCC 5.1.1) has less false positive so warning could maybe put back
# -Wno-unused-function: warn for function not used in release build
# -Wno-unused-value: lots of warning for this kind of statements "0 && ...". There are used to disable some parts of code in release/dev build.
# -Wno-format*: Yeah, these need to be taken care of, but...
# -Wno-stringop-truncation: Who comes up with these compiler warnings, anyways?
# -Wno-stringop-overflow: Probably the same people as this one...
# -Wno-maybe-uninitialized: Lots of gcc warnings like "‘test.GSVector8i::<anonymous>.GSVector8i::<unnamed union>::m’ may be used uninitialized" if this is removed.

if (MSVC)
	set(DEFAULT_WARNINGS)
else()
	set(DEFAULT_WARNINGS -Wall -Wextra -Wno-attributes -Wno-unused-function -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format -Wno-format-security -Wno-unused-value)
endif()

# An undeclared call in a C file is an error on Clang and GCC 14, a warning
# on older GCC. Make it an error everywhere so no lane waves one through.
if (NOT MSVC)
	add_compile_options($<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
endif()

if (USE_GCC)
	list(APPEND DEFAULT_WARNINGS -Wno-stringop-truncation -Wno-stringop-overflow -Wno-maybe-uninitialized )
endif()

if (USE_PGO_GENERATE OR USE_PGO_OPTIMIZE)
	add_compile_options("-fprofile-dir=${CMAKE_SOURCE_DIR}/profile")
endif()

if (USE_PGO_GENERATE)
	add_compile_options(-fprofile-generate)
endif()

if(USE_PGO_OPTIMIZE)
	add_compile_options(-fprofile-use)
endif()

list(APPEND PCSX2_DEFS
	"$<$<CONFIG:Debug>:PCSX2_DEBUG;_DEBUG>"
	"$<$<CONFIG:Devel>:_DEVEL>")

if (USE_ASAN)
	add_compile_options(-fsanitize=address)
	add_link_options(-fsanitize=address)
	list(APPEND PCSX2_DEFS ASAN_WORKAROUND)
endif()

if(USE_CLANG AND TIMETRACE)
	add_compile_options(-ftime-trace)
endif()

set(PCSX2_WARNINGS ${DEFAULT_WARNINGS})

#-------------------------------------------------------------------------------
# MacOS-specific things
#-------------------------------------------------------------------------------

# macOS only, and only when nothing else has said which version to target: this
# used to be set unconditionally, so a caller that asked for a target got 10.13
# anyway. On iOS and tvOS that number is not even a version of the system being
# built for - the core came out stamped "minimum iOS 10.13" - and it decided the
# aligned-allocation question below for those platforms as well.
if(NOT CMAKE_GENERATOR MATCHES "Xcode" AND CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
	# Assume Xcode builds aren't being used for distribution
	# Helpful because Xcode builds don't build multiple metallibs for different macOS versions
	# Also helpful because Xcode's interactive shader debugger requires apps be built for the latest macOS
	set(CMAKE_OSX_DEPLOYMENT_TARGET 10.13)
endif()

if (APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET AND "${CMAKE_OSX_DEPLOYMENT_TARGET}" VERSION_LESS 10.14 AND NOT ${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS 9)
	# Older versions of the macOS stdlib don't have operator new(size_t, align_val_t)
	# Disable use of them with this flag
	# Not great, but also no worse that what we were getting before we turned on C++17
	add_compile_options(-fno-aligned-allocation)
endif()

# CMake defaults the suffix for modules to .so on macOS but wx tells us that the
# extension is .dylib (so that's what we search for)
if(APPLE)
	set(CMAKE_SHARED_MODULE_SUFFIX ".dylib")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
	if(NOT OSX_USE_DEFAULT_SEARCH_PATH)
		# Hack up the path to prioritize the path to built-in OS libraries to
		# increase the chance of not depending on a bunch of copies of them
		# installed by MacPorts, Fink, Homebrew, etc, and ending up copying
		# them into the bundle.  Since we depend on libraries which are not
		# part of OS X (wx, etc.), however, don't remove the default path
		# entirely.  This is still kinda evil, since it defeats the user's
		# path settings...
		# See http://www.cmake.org/cmake/help/v3.0/command/find_program.html
		list(APPEND CMAKE_PREFIX_PATH "/usr")
	endif()

	add_link_options(-Wl,-dead_strip,-dead_strip_dylibs)
endif()
