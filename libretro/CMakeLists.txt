
# fastjmp's register save and restore is file-scope assembly, which MSVC does
# not have on x64: fastjmp.c compiles to nothing under _MSC_VER and the
# sequence is a MASM source beside it. Without this the link ends in
# unresolved fastjmp_set and fastjmp_jmp -- every other toolchain uses the
# inline form and never notices.
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	enable_language(ASM_MASM)
endif()

add_library(pcsx2_libretro SHARED)

target_sources(pcsx2_libretro PRIVATE
   ${CMAKE_SOURCE_DIR}/libretro/audio_queue.c
   ${CMAKE_SOURCE_DIR}/libretro/state_ext.c
   ${CMAKE_SOURCE_DIR}/libretro/main.cpp
   ${CMAKE_SOURCE_DIR}/libretro/patches.cpp
   ${CMAKE_SOURCE_DIR}/libretro/USB.cpp
   ${CMAKE_SOURCE_DIR}/libretro/CrashHandler.cpp
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/compat_strl.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/compat_posix_string.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/compat_fnmatch.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/fopen_utf8.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_utf.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/file/file_path.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/file/file_path_io.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/string/stdstring.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/streams/file_stream.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/time/rtime.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/rthreads/rthreads.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/fastjmp/fastjmp.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/faulthandler/faulthandler.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/dds/rdds.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/memmap/memalign.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/memmap/memmap.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/rthreads/retro_asym_eventcount.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/rthreads/retro_eventcount.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/rthreads/retro_procbarrier.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/queues/retro_spsc.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/vfs/vfs_implementation.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/vfs/vfs_hybrid.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/file/retro_dirent.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/features/features_cpu.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/chd/rchd.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/flac/rflac.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/yaml/ryaml.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/string/rstrtod.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/7z/r7z_lzma.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_rzstd.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_huffman.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_deflate.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_crc32.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_rlz4.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/png/rpng.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/streams/trans_stream.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/streams/trans_stream_deflate.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/streams/trans_stream_pipe.c
)

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	target_sources(pcsx2_libretro PRIVATE
		${CMAKE_SOURCE_DIR}/libretro/libretro-common/fastjmp/fastjmp_msvc_x64.asm
	)
endif()

# getifaddrs()/freeifaddrs() are only declared by bionic from API 24 on,
# below what the NDK lanes target, so DEV9 gets libretro-common's netlink
# reimplementation instead. WANT_IFADDRS (set on PCSX2_FLAGS in
# pcsx2/CMakeLists.txt) is what switches the includes over. Every other
# platform has them in libc and must not compile this. Keep in step with
# Makefile.common.
if(ANDROID)
   target_sources(pcsx2_libretro PRIVATE
      ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/compat_ifaddrs.c
   )
endif()

# Which of rchd's hunk codecs are compiled in.  Stated here rather than
# left to rchd.c's fallback: that fallback turns on DEFLATE and LZMA
# unconditionally but gates FLAC and Zstandard behind HAVE_FLAC/HAVE_RFLAC
# and HAVE_ZSTD/HAVE_RZSTD, which this build has never defined.  The
# result was a reader that linked rflac.c and encoding_rzstd.c and then
# refused every 'cdfl' and 'cdzs' hunk with RCHD_ERROR_UNSUPPORTED - a
# per-hunk failure, so an image opened, reported correct geometry and a
# correct TOC, and only died on the fraction of hunks that happened to
# pick those codecs.  All four objects are in target_sources above, so
# all four codecs are named here.  Keep in step with Makefile.common.
target_compile_definitions(pcsx2_libretro PRIVATE
   HAVE_RCHD_DEFLATE
   HAVE_RCHD_LZMA
   HAVE_RCHD_FLAC
   HAVE_RCHD_ZSTD
)

target_link_libraries(pcsx2_libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
)

if(WIN32)
  # DEV9's winsock ethernet backend and adapter enumeration.
  target_link_libraries(pcsx2_libretro PRIVATE ws2_32 iphlpapi)
endif()

if(APPLE)
  target_link_libraries(pcsx2_libretro PRIVATE
    "-framework CoreGraphics"
    "-framework Foundation"
  )
  # tvOS is the one Apple platform with no IOKit framework, and the link there
  # ends in "framework 'IOKit' not found". Nothing in this tree calls into it.
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    target_link_libraries(pcsx2_libretro PRIVATE "-framework IOKit")
  endif()
endif()

target_include_directories(PCSX2_FLAGS INTERFACE
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
)

target_include_directories(common INTERFACE
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
)

target_include_directories(pcsx2_libretro PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
	"${CMAKE_SOURCE_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/3rdparty/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

set_target_properties(pcsx2_libretro PROPERTIES
   LIBRARY_OUTPUT_NAME pcsx2_libretro
   PREFIX ""
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
   set(CLANG 1)
endif()

if(NOT MSVC AND NOT CLANG)
   set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
endif()

if(PACKAGE_MODE)
    install(TARGETS pcsx2_libretro DESTINATION ${BIN_DIR})
else(PACKAGE_MODE)
    install(TARGETS pcsx2_libretro DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif(PACKAGE_MODE)

# The smoke gate loads the core the moment it links and walks the
# lifecycle a frontend performs before any content exists: init, the
# BIOS-less retro_load_game failure path, deinit, unload.  A crash in
# any of it fails the build, so a broken artifact never reaches the
# buildbot.  Opt-in because it executes host code: CI turns it on for
# jobs whose runner natively runs what it just built; cross builds
# cannot.
option(LRPS2_SMOKE_GATE "Run the libretro lifecycle smoke gate after linking the core" OFF)
if(LRPS2_SMOKE_GATE)
   add_executable(lrps2_smoke ${CMAKE_SOURCE_DIR}/tests/smoke/lrps2_smoke.c)
   target_include_directories(lrps2_smoke PRIVATE
      "${CMAKE_SOURCE_DIR}/libretro/libretro-common/include")
   target_link_libraries(lrps2_smoke PRIVATE ${CMAKE_DL_LIBS})
   # The CI template builds only the pcsx2_libretro target, so the
   # harness rides along as its dependency.
   add_dependencies(pcsx2_libretro lrps2_smoke)
   # An empty working directory of its own: the BIOS scan must find
   # nothing, and anything the core writes stays contained.
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke-work")
   add_custom_command(TARGET pcsx2_libretro POST_BUILD
      COMMAND lrps2_smoke "$<TARGET_FILE:pcsx2_libretro>"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke-work"
      COMMENT "smoke gate: libretro lifecycle against the freshly linked core"
      VERBATIM)
endif()
