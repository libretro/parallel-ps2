DEBUG = 0
FRONTEND_SUPPORTS_RGB565 = 1
HAVE_OPENGL = 1
GLES = 0
GLES3 = 0 # HW renderer now supported on GLES3
HAVE_VULKAN = 1
HAVE_CHD = 1
HAVE_PCAP = 0
HAVE_CDROM = 0
LINK_STATIC_LIBCPLUSPLUS = 1
THREADED_RECOMPILER = 1

CORE_DIR := .
HAVE_GRIFFIN = 0
SILENT := 0

SPACE :=
SPACE := $(SPACE) $(SPACE)
BACKSLASH :=
BACKSLASH := \$(BACKSLASH)
filter_out1 = $(filter-out $(firstword $1),$1)
filter_out2 = $(call filter_out1,$(call filter_out1,$1))

GIT_VERSION ?= " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
   FLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

ifeq ($(platform),)
   platform = unix
   ifeq ($(shell uname -s),)
      platform = win
   else ifneq ($(findstring Darwin,$(shell uname -s)),)
      platform = osx
      arch     = intel
      ifeq ($(shell uname -p),powerpc)
         arch = ppc
      else ifneq (,$(findstring arm64,$(shell uname -m)))
         # Apple Silicon: default to the native arm64 recompiler backend.
         arch = arm64
      endif
   else ifneq ($(findstring MINGW,$(shell uname -s)),)
      platform = win
   endif
else ifneq (,$(findstring armv,$(platform)))
   override platform += unix
endif

ifneq ($(platform), osx)
   ifeq ($(findstring Haiku,$(shell uname -s)),)
      PTHREAD_FLAGS = -lpthread
   endif
endif

NEED_CD = 1
NEED_TREMOR = 1
NEED_BPP = 32
NEED_DEINTERLACER = 1
NEED_THREADING = 1
SET_HAVE_HW = 0
CORE_DEFINE :=
TARGET_NAME := pcsx2

# Per-subsystem wall-time profiler. Off by default; PROFILER=1 adds the source
# and the define, matching -DPROFILER=ON in the CMake build. With it off
# neither is part of the build, so the macros in Profiler.h expand to nothing.
ifeq ($(PROFILER), 1)
   FLAGS += -DENABLE_PCSX2_PROFILER
endif

ifeq ($(HAVE_HW), 1)
   HAVE_VULKAN = 1
   HAVE_OPENGL = 1
   SET_HAVE_HW = 1
endif

ifeq ($(HAVE_VULKAN), 1)
   SET_HAVE_HW = 1
endif

ifeq ($(HAVE_OPENGL), 1)
   SET_HAVE_HW = 1
endif

ifeq ($(SET_HAVE_HW), 1)
   FLAGS += -DHAVE_HW
endif

# Unix
ifneq (,$(findstring unix,$(platform)))
   # local VFS may mmap FREQUENT_ACCESS files (CDVD zero-copy)
   FLAGS += -DHAVE_MMAP
   # libretro-common reads the CPU topology out of sysfs
   FLAGS += -D_GNU_SOURCE
   TARGET := $(TARGET_NAME)_libretro.so
   fpic   := -fPIC
   ifneq ($(findstring SunOS,$(shell uname -a)),)
      GREP = ggrep
      SHARED := -shared -z defs
   else
      GREP = grep
      SHARED := -shared -Wl,--no-undefined -Wl,--version-script=link.T
   endif
   ifeq ($(LINK_STATIC_LIBCPLUSPLUS),1)
      LDFLAGS += -static-libgcc -static-libstdc++
   endif
   ifneq ($(shell uname -p | $(GREP) -E '((i.|x)86|amd64)'),)
      IS_X86 = 1
   endif
   ifneq (,$(findstring Haiku,$(shell uname -s)))
      LDFLAGS += $(PTHREAD_FLAGS) -lroot
   else
      LDFLAGS += $(PTHREAD_FLAGS) -ldl
   endif
   FLAGS   +=
   # No GL library on the link line: every GL entry point comes from the
   # frontend through gladLoadGLLoader(), as in the CMake build.
   ifeq ($(HAVE_OPENGL),1)
      ifneq (,$(findstring gles,$(platform)))
         GLES = 1
      endif
   endif

ifneq ($(findstring Linux,$(shell uname -s)),)
   HAVE_CDROM = 1
endif

# OS X
else ifeq ($(platform), osx)
   TARGET  := $(TARGET_NAME)_libretro.dylib
   fpic    := -fPIC
   SHARED  := -dynamiclib -Wl,-exported_symbols_list,libretro.osx.def
   LDFLAGS += $(PTHREAD_FLAGS)
   FLAGS   += $(PTHREAD_FLAGS)
   ifeq ($(arch),ppc)
      ENDIANNESS_DEFINES := -DMSB_FIRST
      OLD_GCC := 1
   endif
   OSXVER = `sw_vers -productVersion | cut -d. -f 2`
   OSX_LT_MAVERICKS = `(( $(OSXVER) <= 9)) && echo "YES"`
   ifeq ($(OSX_LT_MAVERICKS),"YES")
      fpic += -mmacosx-version-min=10.5
   endif
   ifeq ($(HAVE_OPENGL),1)
      GL_LIB := -framework OpenGL
   endif
   ifeq ($(CROSS_COMPILE),1)
	TARGET_RULE   = -target $(LIBRETRO_APPLE_PLATFORM) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
	CFLAGS   += $(TARGET_RULE)
	CPPFLAGS += $(TARGET_RULE)
	CXXFLAGS += $(TARGET_RULE)
	LDFLAGS  += $(TARGET_RULE)
   endif

# iOS
else ifneq (,$(findstring ios,$(platform)))
   ifeq ($(platform),$(filter $(platform),ios-arm64))
   iarch := arm64
   else
   iarch := armv7
   endif
   TARGET  := $(TARGET_NAME)_libretro_ios.dylib
   fpic    := -fPIC
   SHARED  := -dynamiclib
   LDFLAGS += $(PTHREAD_FLAGS)
   FLAGS   += $(PTHREAD_FLAGS)
   ifeq ($(IOSSDK),)
      IOSSDK := $(shell xcrun -sdk iphoneos -show-sdk-path)
   endif
   ifeq ($(HAVE_OPENGL),1)
      GL_LIB := -framework OpenGLES
      GLES = 1
      GLES3 = 1
   endif

   CC = cc -arch $(iarch) -isysroot $(IOSSDK)
   CXX = c++ -arch $(iarch) -isysroot $(IOSSDK)
   IPHONEMINVER :=
   ifeq ($(platform),$(filter $(platform),ios9 ios-arm64))
      IPHONEMINVER = -miphoneos-version-min=8.0
   else
      IPHONEMINVER = -miphoneos-version-min=5.0
   endif
   LDFLAGS += $(IPHONEMINVER)
   FLAGS   += $(IPHONEMINVER) -DHAVE_UNISTD_H -DIOS=1
   CC      += $(IPHONEMINVER)
   CXX     += $(IPHONEMINVER)

# tvOS
else ifeq ($(platform), tvos-arm64)
   TARGET := $(TARGET_NAME)_libretro_tvos.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   FLAGS += -DHAVE_UNISTD_H -DIOS=1 -DTVOS=1

   ifeq ($(IOSSDK),)
      IOSSDK := $(shell xcrun -sdk appletvos -show-sdk-path)
   endif
   ifeq ($(HAVE_OPENGL),1)
      GL_LIB := -framework OpenGLES
      GLES = 1
      GLES3 = 1
   endif

   CC = cc -arch arm64 -isysroot $(IOSSDK)
   CXX = c++ -arch arm64 -isysroot $(IOSSDK)
   MINVER = -mappletvos-version-min=11.0
   LDFLAGS += $(MINVER)
   FLAGS += $(MINVER)
   CC += $(MINVER)
   CXX += $(MINVER)

# QNX
else ifeq ($(platform), qnx)
   TARGET := $(TARGET_NAME)_libretro_$(platform).so
   fpic   := -fPIC
   SHARED := -lcpp -lm -shared -Wl,--no-undefined -Wl,--version-script=link.T
   #LDFLAGS += $(PTHREAD_FLAGS)
   CC     = qcc -Vgcc_ntoarmv7le
   CXX    = QCC -Vgcc_ntoarmv7le_cpp
   AR     = QCC -Vgcc_ntoarmv7le
   FLAGS += -D__BLACKBERRY_QNX__ -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp
   ifeq ($(HAVE_OPENGL),1)
      GL_LIB := -lGLESv2
   endif

# PS3
else ifeq ($(platform), ps3)
   TARGET := $(TARGET_NAME)_libretro_$(platform).a
   CC      = $(CELL_SDK)/host-win32/ppu/bin/ppu-lv2-gcc.exe
   CXX     = $(CELL_SDK)/host-win32/ppu/bin/ppu-lv2-g++.exe
   AR      = $(CELL_SDK)/host-win32/ppu/bin/ppu-lv2-ar.exe
   ENDIANNESS_DEFINES := -DMSB_FIRST
   OLD_GCC := 1
   FLAGS += -DARCH_POWERPC_ALTIVEC
   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING

# sncps3
else ifeq ($(platform), sncps3)
   TARGET := $(TARGET_NAME)_libretro_ps3.a
   CC      = $(CELL_SDK)/host-win32/sn/bin/ps3ppusnc.exe
   CXX     = $(CELL_SDK)/host-win32/sn/bin/ps3ppusnc.exe
   AR      = $(CELL_SDK)/host-win32/sn/bin/ps3snarl.exe
   ENDIANNESS_DEFINES := -DMSB_FIRST
   CXXFLAGS += -Xc+=exceptions
   OLD_GCC  := 1
   NO_GCC   := 1
   FLAGS    += -DARCH_POWERPC_ALTIVEC
   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING

# Lightweight PS3 Homebrew SDK
else ifeq ($(platform), psl1ght)
   TARGET := $(TARGET_NAME)_libretro_$(platform).a
   CC      = $(PS3DEV)/ppu/bin/ppu-gcc$(EXE_EXT)
   CXX     = $(PS3DEV)/ppu/bin/ppu-g++$(EXE_EXT)
   AR      = $(PS3DEV)/ppu/bin/ppu-ar$(EXE_EXT)
   ENDIANNESS_DEFINES := -DMSB_FIRST
   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING

# PSP
else ifeq ($(platform), psp1)
   TARGET := $(TARGET_NAME)_libretro_$(platform).a
   CC      = psp-gcc$(EXE_EXT)
   CXX     = psp-g++$(EXE_EXT)
   AR      = psp-ar$(EXE_EXT)
   FLAGS  += -DPSP -G0
   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING
   EXTRA_INCLUDES := -I$(shell psp-config --pspsdk-path)/include

# Vita
else ifeq ($(platform), vita)
   TARGET := $(TARGET_NAME)_libretro_$(platform).a
   CC      = arm-vita-eabi-gcc$(EXE_EXT)
   CXX     = arm-vita-eabi-g++$(EXE_EXT)
   AR      = arm-vita-eabi-ar$(EXE_EXT)
   FLAGS  += -DVITA
   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING

# Xbox 360
else ifeq ($(platform), xenon)
   TARGET := $(TARGET_NAME)_libretro_xenon360.a
   CC      = xenon-gcc$(EXE_EXT)
   CXX     = xenon-g++$(EXE_EXT)
   AR      = xenon-ar$(EXE_EXT)
   ENDIANNESS_DEFINES += -D__LIBXENON__ -m32 -D__ppc__ -DMSB_FIRST
   LIBS := $(PTHREAD_FLAGS)
   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING

# Nintendo Game Cube / Nintendo Wii
else ifneq (,$(filter $(platform),ngc wii))
   ifeq ($(platform), ngc)
      TARGET := $(TARGET_NAME)_libretro_$(platform).a
      ENDIANNESS_DEFINES += -DHW_DOL
   else ifeq ($(platform), wii)
      TARGET := $(TARGET_NAME)_libretro_$(platform).a
      ENDIANNESS_DEFINES += -DHW_RVL
   endif
   ENDIANNESS_DEFINES += -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -DMSB_FIRST
   CC  = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
   CXX = $(DEVKITPPC)/bin/powerpc-eabi-g++$(EXE_EXT)
   AR  = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
   EXTRA_INCLUDES := -I$(DEVKITPRO)/libogc/include
   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING

# Nintendo WiiU
else ifeq ($(platform), wiiu)
   TARGET := $(TARGET_NAME)_libretro_$(platform).a
   CC      = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
   CXX     = $(DEVKITPPC)/bin/powerpc-eabi-g++$(EXE_EXT)
   AR      = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
   FLAGS  += -DGEKKO -D__wiiu__ -mcpu=750 -meabi -mhard-float
   FLAGS  += -U__INT32_TYPE__ -U __UINT32_TYPE__ -D__INT32_TYPE__=int
   ENDIANNESS_DEFINES += -DMSB_FIRST
   EXTRA_INCLUDES     := -Ideps
   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING
   NEED_THREADING = 0

# GCW0
else ifeq ($(platform), gcw0)
   TARGET  := $(TARGET_NAME)_libretro.so
   CC       = /opt/gcw0-toolchain/usr/bin/mipsel-linux-gcc
   CXX      = /opt/gcw0-toolchain/usr/bin/mipsel-linux-g++
   AR       = /opt/gcw0-toolchain/usr/bin/mipsel-linux-ar
   fpic    := -fPIC
   SHARED  := -shared -Wl,--no-undefined -Wl,--version-script=link.T
   LDFLAGS += $(PTHREAD_FLAGS)
   FLAGS   += $(PTHREAD_FLAGS)
   FLAGS   += -ffast-math -march=mips32 -mtune=mips32r2 -mhard-float
   GLES     = 1
   GL_LIB  := -lGLESv2

# Emscripten
else ifeq ($(platform), emscripten)
   TARGET  := $(TARGET_NAME)_libretro_$(platform).bc
   fpic    := -fPIC
   FLAGS   += -DEMSCRIPTEN
   FLAGS   += -msimd128 -ftree-vectorize

   HAVE_OPENGL = 1
   GLES = 1
   GLES3 = 1
   NEED_THREADING = 0
   HAVE_CDROM = 0
   THREADED_RECOMPILER = 0

   STATIC_LINKING = 1
   FLAGS += -DSTATIC_LINKING

# Raspberry Pi 4 in 64bit mode
else ifeq ($(platform), rpi4_64)
   TARGET := $(TARGET_NAME)_libretro.so
   fpic   := -fPIC
   GREP = grep
   SHARED := -shared -Wl,--no-undefined -Wl,--version-script=link.T
   CFLAGS   += -O3 -DNDEBUG -march=armv8-a+crc+simd -mtune=cortex-a72 -fsigned-char 
   CXXFLAGS += -O3 -DNDEBUG -march=armv8-a+crc+simd -mtune=cortex-a72 -fsigned-char
   LDFLAGS += $(PTHREAD_FLAGS) -ldl -lrt
   FLAGS += -DHAVE_SHM
   GLES = 1
   GLES3 = 1
   GL_LIB := -lGLESv2
   HAVE_CDROM = 0

# Windows MSVC 2017 all architectures
else ifneq (,$(findstring windows_msvc2017,$(platform)))

   NO_GCC := 1

   PlatformSuffix = $(subst windows_msvc2017_,,$(platform))
   ifneq (,$(findstring desktop,$(PlatformSuffix)))
      WinPartition = desktop
      MSVC2017CompileFlags = -DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP -FS
      LDFLAGS += -MANIFEST -LTCG:incremental -NXCOMPAT -DYNAMICBASE -DEBUG -OPT:REF -INCREMENTAL:NO -SUBSYSTEM:WINDOWS -MANIFESTUAC:"level='asInvoker' uiAccess='false'" -OPT:ICF -ERRORREPORT:PROMPT -NOLOGO -TLBID:1
      LIBS += kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib
      HAVE_CDROM = 1
   else ifneq (,$(findstring uwp,$(PlatformSuffix)))
      WinPartition = uwp
      MSVC2017CompileFlags = -DWINAPI_FAMILY=WINAPI_FAMILY_APP -D_WINDLL -D_UNICODE -DUNICODE -D__WRL_NO_DEFAULT_LIB__ -EHsc -FS
      LDFLAGS += -APPCONTAINER -NXCOMPAT -DYNAMICBASE -MANIFEST:NO -LTCG -OPT:REF -SUBSYSTEM:CONSOLE -MANIFESTUAC:NO -OPT:ICF -ERRORREPORT:PROMPT -NOLOGO -TLBID:1 -DEBUG:FULL -WINMD:NO
      LIBS += WindowsApp.lib
   endif

   CFLAGS += $(MSVC2017CompileFlags)
   CXXFLAGS += $(MSVC2017CompileFlags)

   TargetArchMoniker = $(subst $(WinPartition)_,,$(PlatformSuffix))

   CC  = cl.exe
   CXX = cl.exe
   LD = link.exe

   reg_query = $(call filter_out2,$(subst $2,,$(shell reg query "$2" -v "$1" 2>nul)))
   fix_path = $(subst $(SPACE),\ ,$(subst \,/,$1))

   ProgramFiles86w := $(shell cmd /c "echo %PROGRAMFILES(x86)%")
   ProgramFiles86 := $(shell cygpath "$(ProgramFiles86w)")

   WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Microsoft\Microsoft SDKs\Windows\v10.0)
   WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_CURRENT_USER\SOFTWARE\Wow6432Node\Microsoft\Microsoft SDKs\Windows\v10.0)
   WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0)
   WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_CURRENT_USER\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0)
   WindowsSdkDir := $(WindowsSdkDir)

   WindowsSDKVersion ?= $(firstword $(foreach folder,$(subst $(subst \,/,$(WindowsSdkDir)Include/),,$(wildcard $(call fix_path,$(WindowsSdkDir)Include\*))),$(if $(wildcard $(call fix_path,$(WindowsSdkDir)Include/$(folder)/um/Windows.h)),$(folder),)))$(BACKSLASH)
   WindowsSDKVersion := $(WindowsSDKVersion)

   VsInstallBuildTools = $(ProgramFiles86)/Microsoft Visual Studio/2017/BuildTools
   VsInstallEnterprise = $(ProgramFiles86)/Microsoft Visual Studio/2017/Enterprise
   VsInstallProfessional = $(ProgramFiles86)/Microsoft Visual Studio/2017/Professional
   VsInstallCommunity = $(ProgramFiles86)/Microsoft Visual Studio/2017/Community

   VsInstallRoot ?= $(shell if [ -d "$(VsInstallBuildTools)" ]; then echo "$(VsInstallBuildTools)"; fi)
   ifeq ($(VsInstallRoot), )
      VsInstallRoot = $(shell if [ -d "$(VsInstallEnterprise)" ]; then echo "$(VsInstallEnterprise)"; fi)
   endif
   ifeq ($(VsInstallRoot), )
      VsInstallRoot = $(shell if [ -d "$(VsInstallProfessional)" ]; then echo "$(VsInstallProfessional)"; fi)
   endif
   ifeq ($(VsInstallRoot), )
      VsInstallRoot = $(shell if [ -d "$(VsInstallCommunity)" ]; then echo "$(VsInstallCommunity)"; fi)
   endif
   VsInstallRoot := $(VsInstallRoot)

   VcCompilerToolsVer := $(shell cat "$(VsInstallRoot)/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt" | grep -o '[0-9\.]*')
   VcCompilerToolsDir := $(VsInstallRoot)/VC/Tools/MSVC/$(VcCompilerToolsVer)

   WindowsSDKSharedIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\shared")
   WindowsSDKUCRTIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\ucrt")
   WindowsSDKUMIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\um")
   WindowsSDKUCRTLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib\$(WindowsSDKVersion)\ucrt\$(TargetArchMoniker)")
   WindowsSDKUMLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib\$(WindowsSDKVersion)\um\$(TargetArchMoniker)")

   # For some reason the HostX86 compiler doesn't like compiling for x64
   # ("no such file" opening a shared library), and vice-versa.
   # Work around it for now by using the strictly x86 compiler for x86, and x64 for x64.
   # NOTE: What about ARM?
   ifneq (,$(findstring x64,$(TargetArchMoniker)))
      VCCompilerToolsBinDir := $(VcCompilerToolsDir)\bin\HostX64
   else
      VCCompilerToolsBinDir := $(VcCompilerToolsDir)\bin\HostX86
   endif

   PATH := $(shell IFS=$$'\n'; cygpath "$(VCCompilerToolsBinDir)/$(TargetArchMoniker)"):$(PATH)
   PATH := $(PATH):$(shell IFS=$$'\n'; cygpath "$(VsInstallRoot)/Common7/IDE")
   INCLUDE := $(shell IFS=$$'\n'; cygpath -w "$(VcCompilerToolsDir)/include")
   LIB := $(shell IFS=$$'\n'; cygpath -w "$(VcCompilerToolsDir)/lib/$(TargetArchMoniker)")
   ifneq (,$(findstring uwp,$(PlatformSuffix)))
      LIB := $(shell IFS=$$'\n'; cygpath -w "$(LIB)/store")
   endif

   export INCLUDE := $(INCLUDE);$(WindowsSDKSharedIncludeDir);$(WindowsSDKUCRTIncludeDir);$(WindowsSDKUMIncludeDir)
   export LIB := $(LIB);$(WindowsSDKUCRTLibDir);$(WindowsSDKUMLibDir)
   TARGET := $(TARGET_NAME)_libretro.dll
   TARGET_TMP := $(TARGET_NAME)_libretro.lib $(TARGET_NAME)_libretro.pdb $(TARGET_NAME)_libretro.exp
   LDFLAGS += -DLL

# Windows (MSYS2 / MinGW-w64).
# Targets full feature parity with the cmake LIBRETRO=ON Windows build:
# OpenGL, Vulkan, parallel-gs, DX11/DX12.  Requires reasonably current
# mingw-w64 headers (>= 14.x; MSYS2 ships these by default) so that
# d3d12.h exposes ID3D12Device4 etc.
else
   TARGET    := $(TARGET_NAME)_libretro.dll
   CC        ?= gcc
   CXX       ?= g++
   IS_X86     = 1
   IS_WINDOWS = 1
   IS_WIN_MINGW = 1
   SHARED    := -shared -Wl,--no-undefined -Wl,--version-script=link.T
   ifeq ($(GCSCAN),1)
      SHARED += -Wl,--gc-sections -Wl,--print-gc-sections -Wl,-Map=gcscan.map
   endif
   HAVE_CDROM = 1

   # Match cmake target_compile_definitions on Windows + LIBRETRO=ON.
   FLAGS += -DHAVE__MKDIR \
            -D_WIN32_WINNT=0x0A00 \
            -DWINVER=0x0A00 \
            -DNTDDI_VERSION=0x0A000006 \
            -DWIN32_LEAN_AND_MEAN= \
            -DNOMINMAX= \
            -DUNICODE -D_UNICODE \
            -D_HAS_EXCEPTIONS=0 \
            -D_ITERATOR_DEBUG_LEVEL=0 \
            -D_SCL_SECURE_NO_WARNINGS \
            -D__SSE4_1__ \
            -DHAVE_D3D11 -DHAVE_D3D12 \
            -DCPUINFO_SUPPORTED_PLATFORM=1
   # MinGW chokes on the very large object files generated by some of the
   # GS / parallel-gs sources without -Wa,-mbig-obj; the cmake build sets
   # this globally for the libretro target on Windows.
   FLAGS += -Wa,-mbig-obj

   # Static C++ runtime - matches cmake's static-libgcc/static-libstdc++ on
   # Linux and the /MT runtime on MSVC.
   LDFLAGS += -static-libgcc -static-libstdc++ -static -lwinpthread

   # System libraries used by the libretro core; mirrors cmake's
   # PCSX2_FLAGS Windows interface link list.
   LIBS += -lopengl32 -ld3d11 -ld3d12 -ldxgi -ld3dcompiler \
           -lonecore -lwinmm -ladvapi32 -lole32 -loleaut32 \
           -luuid -lshell32 -lkernel32 -luser32 -lgdi32 \
           -lwinspool -lcomdlg32 -lws2_32 -liphlpapi \
           -lwbemuuid -lsetupapi -lversion

   ifeq ($(HAVE_OPENGL),1)
      GL_LIB :=
   endif
endif

# ---------------------------------------------------------------------------
# Target-architecture selector for the CPU recompiler backend.
#
# PR #143 added native AArch64 (arm64) EE/IOP/VU recompilers plus the VIXL code
# emitter, but only wired them into the cmake build. Mirror that here for the
# static Makefile so modern Apple Silicon Macs (and Linux/iOS/tvOS arm64) use
# the arm64 dynarec instead of the x86 one. Keyed off the *target* arch (like
# cmake's _M_X86 vs aarch64 gate), not the OS.
#
# On macOS with no explicit arch, fall back to the host arch so a bare `make`
# on Apple Silicon targets arm64 (clang already defaults its codegen to arm64
# there; without this the x86 recompiler sources would be selected and fail).
ifeq ($(platform), osx)
   ifeq ($(arch),)
      arch := $(shell uname -m)
   endif
endif

ifneq (,$(filter arm64 aarch64,$(arch)))
   IS_ARM64 = 1
endif
ifneq (,$(filter ios-arm64 tvos-arm64 rpi4_64,$(platform)))
   IS_ARM64 = 1
endif
ifneq (,$(findstring unix,$(platform)))
   ifneq (,$(filter aarch64 arm64,$(shell uname -m)))
      IS_ARM64 = 1
   endif
endif

# IS_X86 is set from the HOST (uname -p) in the unix branch above, so a cross
# build -- make arch=aarch64 CXX=aarch64-linux-gnu-g++ -- would otherwise keep
# the x86 SIMD baseline and hand -msse to an aarch64 compiler. The target
# architecture wins.
ifeq ($(IS_ARM64), 1)
   IS_X86 =
endif

# The C89 macro core is the default emitter; pass C89_EMITTER=0 to build the
# original C++ reference implementation. Set BEFORE the include: the source
# exclusion in Makefile.common reads it.
C89_EMITTER ?= 1

# Opt-in A/B verification scaffolding; the flag becomes a define next to
# PCSX2_C89_EMITTER below -- appending to COMMON_FLAGS here, before the
# include, silently did nothing, the same trap the C89_EMITTER placement
# comment above already warns about from the other direction.
XE_AB ?= 0

ifeq ($(HAVE_PCAP), 1)
   FLAGS   += -DHAVE_PCAP
   LDFLAGS += -lpcap
endif

include Makefile.common

# GameDatabaseBuiltin.cpp is generated from the shipped GameIndex.yaml at
# build time (tools/gen_gamedb_builtin.sh, POSIX shell + od + awk only).
pcsx2/GameDatabaseBuiltin.cpp: bin/resources/GameIndex.yaml tools/gen_gamedb_builtin.sh
	sh tools/gen_gamedb_builtin.sh bin/resources/GameIndex.yaml $@

# The multi-ISA block below uses $(eval) to generate explicit per-tier object
# rules. In make, the first explicit target seen becomes the default goal if
# none is set yet -- so a generated rule like GSBlock.sse4.o would otherwise
# hijack the default goal, making a bare `make` build only that one object.
# Pin the default goal to `all` (defined later) before any rule generation.
.DEFAULT_GOAL := all

# ---------------------------------------------------------------------------
# Multi-ISA runtime SIMD dispatch (default ON; set ENABLE_MULTI_ISA=0 to opt
# out and get the single-ISA SSE4.1 build).
#
# When ON (default), the unshared MultiISA sources are compiled three times
# (sse4/avx/avx2) into separate objects and all tiers are linked;
# features_cpu selects the best path the host CPU supports at runtime
# (MULTI_ISA_SELECT in
# MultiISA.h). The sse4 tier remains and is the path chosen on any CPU lacking
# AVX/AVX2, so this does NOT regress support for SSE4-or-earlier hosts -- those
# CPUs run exactly the SSE4.1 code they did before.
#
# When OFF (ENABLE_MULTI_ISA=0), the GS/IPU/SPU2 MultiISA sources are compiled
# once at the SSE4.1 baseline and MultiISA.h resolves CURRENT_ISA to
# isa_native -- byte-identical to the historical single-ISA core build.
#
# Declared before first use below.
ENABLE_MULTI_ISA ?= 1

# ---------------------------------------------------------------------------
# Multi-ISA tier flags. Defined here, before the object generation below
# consumes them.
#
# GCC/clang (incl. MinGW, which is GCC): use feature flags rather than -march
# so the compiler can still inline shared helpers across tiers (per cmake).
# MSVC: use /arch:. There is no /arch:SSE4.1 -- SSE2 is MSVC's implicit x64
# baseline and SSE3/SSSE3/SSE4.1 intrinsics are available without an /arch:
# flag, so the sse4 tier takes no extra flag. /arch:AVX and /arch:AVX2 enable
# the wider tiers.
ifeq ($(IS_X86),1)
ifeq ($(ENABLE_MULTI_ISA),1)
   ifneq (,$(findstring msvc,$(platform)))
      MULTI_ISA_FLAGS_sse4 :=
      MULTI_ISA_FLAGS_avx  := /arch:AVX
      MULTI_ISA_FLAGS_avx2 := /arch:AVX2
   else
      MULTI_ISA_FLAGS_sse4 := -msse4.1
      MULTI_ISA_FLAGS_avx  := -msse4.1 -mavx
      MULTI_ISA_FLAGS_avx2 := -msse4.1 -mavx -mavx2 -mbmi -mbmi2 -mfma
   endif
   # Tiers in oldest->newest order. Link/archive order MUST follow this so the
   # linker resolves shared inline (ODR-duplicated) symbols to the SSE4-safe
   # copy; see the ordering note where tier objects are archived.
   MULTI_ISA_TIERS := sse4 avx avx2
endif
endif

# ---------------------------------------------------------------------------
# Multi-ISA object generation (only when ENABLE_MULTI_ISA=1 on x86).
#
# The "unshared" GS/IPU sources contain the SIMD-templated code that
# MultiISA.h splits into per-ISA namespaces. When multi-ISA is enabled they
# must be compiled once per tier (sse4/avx/avx2) with that tier's feature
# flags and MULTI_ISA_UNSHARED_COMPILATION=isa_<tier>, producing distinct
# objects (e.g. GSRasterizer.sse4.o). Every other TU is compiled once as the
# "shared" build with MULTI_ISA_SHARED_COMPILATION defined.
#
# When the switch is OFF this whole block is skipped: the unshared files stay
# in SOURCES_CXX and are built once at the SSE4.1 baseline (CURRENT_ISA ==
# isa_native), exactly as before.
MULTI_ISA_UNSHARED_SRC := \
	$(LRPS2_DIR)/GS/GSBlock.cpp \
	$(LRPS2_DIR)/GS/GSLocalMemoryMultiISA.cpp \
	$(LRPS2_DIR)/GS/GSXXH.cpp \
	$(LRPS2_DIR)/GS/Renderers/Common/GSVertexTraceFMM.cpp \
	$(LRPS2_DIR)/GS/Renderers/HW/GSRendererHWMultiISA.cpp \
	$(LRPS2_DIR)/GS/Renderers/SW/GSDrawScanline.cpp \
	$(LRPS2_DIR)/GS/Renderers/SW/GSDrawScanlineCodeGenerator.cpp \
	$(LRPS2_DIR)/GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp \
	$(LRPS2_DIR)/GS/Renderers/SW/GSRasterizer.cpp \
	$(LRPS2_DIR)/GS/Renderers/SW/GSRendererSW.cpp \
	$(LRPS2_DIR)/GS/Renderers/SW/GSSetupPrimCodeGenerator.cpp \
	$(LRPS2_DIR)/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp \
	$(LRPS2_DIR)/IPU/IPU_MultiISA.cpp

ifeq ($(IS_X86),1)
ifeq ($(ENABLE_MULTI_ISA),1)
   # The shared build (everything that stays in SOURCES_CXX) needs to know it
   # is the shared half of a multi-isa build.
   CXXFLAGS += -DMULTI_ISA_SHARED_COMPILATION

   # Pull the unshared files out of the normal (single-compile) source list;
   # they are built per-tier below instead.
   SOURCES_CXX := $(filter-out $(MULTI_ISA_UNSHARED_SRC),$(SOURCES_CXX))

   # First tier in MULTI_ISA_TIERS is the "is-first" one (drives
   # MULTI_ISA_IS_FIRST / MULTI_ISA_COMPILE_ONCE so compile-once globals are
   # emitted exactly once, in the sse4 tier).
   MULTI_ISA_FIRST_TIER := $(firstword $(MULTI_ISA_TIERS))

   MULTI_ISA_OBJECTS :=
endif
endif

# For each (source, tier) pair, define an explicit object + rule:
#   <src-without-.cpp>.<tier>.o : <src>
# compiled with the tier flags and the unshared-compilation defines.
#
# The tier objects are the UNSHARED half of the multi-isa build, so they must
# NOT see -DMULTI_ISA_SHARED_COMPILATION (it lives in the global CXXFLAGS for
# the shared TUs). MultiISA.h treats the two defines as mutually exclusive;
# having both poisons MULTI_ISA_UNSHARED_START with a static_assert. Strip the
# shared define from CXXFLAGS for the per-tier compile.
# NOTE: define-block bodies must start at column 0; the recipe line is a TAB.
define MULTI_ISA_template
$(1:.cpp=.$(2).o): MULTI_ISA_OBJ_FLAGS := $(MULTI_ISA_FLAGS_$(2)) -DMULTI_ISA_UNSHARED_COMPILATION=isa_$(2) -DMULTI_ISA_IS_FIRST=$(if $(filter $(2),$(MULTI_ISA_FIRST_TIER)),1,0)
$(1:.cpp=.$(2).o): $(1)
	$$(CXX) -c $$(OBJOUT)$$@ $$< $$(filter-out -DMULTI_ISA_SHARED_COMPILATION,$$(CXXFLAGS)) $$(MULTI_ISA_OBJ_FLAGS)
MULTI_ISA_OBJECTS += $(1:.cpp=.$(2).o)
endef

ifeq ($(IS_X86),1)
ifeq ($(ENABLE_MULTI_ISA),1)
   # ODR / link-order safety (the requirement that protects SSE4-or-earlier
   # hosts):
   #
   # Each tier object is compiled with a different -m flag, so any inline
   # function pulled in from a shared header (STL helpers, etc.) is emitted as
   # a weak/COMDAT symbol with the SAME mangled name in every tier object, but
   # with potentially tier-specific instructions in its body. The linker keeps
   # the FIRST such weak symbol it sees and discards the rest. If an AVX2-built
   # copy were kept and then executed on an SSE4 CPU (which happens because the
   # isa_sse4 dispatch path still calls these shared inlines), it would issue
   # an illegal instruction and crash -- exactly the regression we must avoid.
   #
   # Therefore ALL sse4 objects must precede ALL avx objects, which must
   # precede ALL avx2 objects, in the order presented to ar/ld, so the
   # SSE4-safe copy of every shared inline wins. We accumulate the objects
   # tier-outer (sse4 group, then avx group, then avx2 group) to guarantee
   # this globally -- not merely per-file. This mirrors cmake building
   # GS-sse4 / GS-avx / GS-avx2 as separate archives linked oldest-first.
   $(foreach tier,$(MULTI_ISA_TIERS),\
     $(foreach src,$(MULTI_ISA_UNSHARED_SRC),\
       $(eval $(call MULTI_ISA_template,$(src),$(tier)))))
endif
endif




WARNINGS := -Wall \
            -Wno-sign-compare \
            -Wno-unused-variable \
            -Wno-unused-function \
            -Wno-uninitialized \
            $(NEW_GCC_WARNING_FLAGS) \
            -Wno-strict-aliasing

# MinGW + cmake's PCSX2 build also suppresses these
ifeq ($(IS_WIN_MINGW),1)
   WARNINGS += -Wno-stringop-overflow \
               -Wno-stringop-truncation \
               -Wno-maybe-uninitialized \
               -Wno-attributes \
               -Wno-unused-parameter \
               -Wno-unused-value \
               -Wno-missing-field-initializers \
               -Wno-parentheses \
               -Wno-missing-braces \
               -Wno-unknown-pragmas \
               -Wno-packed-not-aligned
   # -Wno-class-memaccess is a C++-only diagnostic.  Putting it in the
   # shared WARNINGS makes gcc emit a noisy 'valid for C++/ObjC++ but
   # not for C' warning on every C TU, so keep it CXX-only.
   CXX_WARNINGS += -Wno-class-memaccess
endif

ifeq ($(NO_GCC),1)
   WARNINGS :=
endif

OBJECTS := $(SOURCES_CXX:.cpp=.o) $(SOURCES_CC:.cc=.o) $(SOURCES_C:.c=.o) $(SOURCES_ASM:.S=.o) $(MULTI_ISA_OBJECTS)
DEPS    := $(SOURCES_CXX:.cpp=.d) $(SOURCES_CC:.cc=.d) $(SOURCES_C:.c=.d)
# Each per-tier object writes its own dependency file beside it (-MMD), and a
# header it includes has to rebuild it like any other object.
DEPS    += $(MULTI_ISA_OBJECTS:.o=.d)

# The software renderer draws the same picture on every instruction set:
# its objects, and the vertex trace it draws by, get no multiply-adds
# fused behind the code's back (the avx2 tier's -mfma, aarch64).
ifeq (,$(findstring msvc,$(platform)))
   $(filter $(LRPS2_DIR)/GS/Renderers/SW/%.o $(LRPS2_DIR)/GS/Renderers/Common/GSVertexTrace%.o,$(OBJECTS)): CXXFLAGS += -ffp-contract=off
endif

# VIXL (SOURCES_CC) is third-party; silence its warnings for those objects only
# (matches cmake's -w on the vixl target) without loosening warnings elsewhere.
ifeq ($(IS_ARM64), 1)
   $(SOURCES_CC:.cc=.o): CXXFLAGS += -w
endif

all: $(TARGET)

-include $(DEPS)

ifeq ($(DEBUG), 1)
   ifneq (,$(findstring msvc,$(platform)))
      ifeq ($(STATIC_LINKING),1)
         CFLAGS   += -MTd
         CXXFLAGS += -MTd
      else
         CFLAGS   += -MDd
         CXXFLAGS += -MDd
      endif

      CFLAGS   += -Od -Zi -DDEBUG -D_DEBUG
      CXXFLAGS += -Od -Zi -DDEBUG -D_DEBUG
   else
      CFLAGS   += -O0 -g -DDEBUG -MMD -fno-strict-aliasing
      CXXFLAGS += -O0 -g -DDEBUG -MMD -fno-strict-aliasing
   endif
else
   ifneq (,$(findstring msvc,$(platform)))
      ifeq ($(STATIC_LINKING),1)
         CFLAGS   += -MT
         CXXFLAGS += -MT
      else
         CFLAGS   += -MD
         CXXFLAGS += -MD
      endif

      CFLAGS   += -O2 -DNDEBUG
      CXXFLAGS += -O2 -DNDEBUG
   else
      # The emulator aliases hardware memory through casts on nearly every
      # page; cmake has always built it with -fno-strict-aliasing
      # (pcsx2/CMakeLists.txt).  Without it, GCC's type-based alias analysis
      # miscompiles the core - newer GCC ever more eagerly - which is why
      # Makefile builds broke launch-era CDVD boots that the cmake builds
      # of the very same commit ran fine.
      CFLAGS   += -O3 -DNDEBUG -MMD -fno-strict-aliasing
      CXXFLAGS += -O3 -DNDEBUG -MMD -fno-strict-aliasing
   endif
endif

# Dead-code scan (GCSCAN=1): split every function/data item into its own
# section so the linker's --gc-sections can discard unreferenced ones and
# --print-gc-sections (on SHARED above) reports each discard. The link.T
# version script anchors the GC roots to the exported retro_* API, so
# everything reachable from the libretro entry points is kept. Measurement
# only -- do not ship a GCSCAN build.
ifeq ($(GCSCAN),1)
   CFLAGS   += -ffunction-sections -fdata-sections
   CXXFLAGS += -ffunction-sections -fdata-sections
endif

# Architecture / SIMD baseline.  cmake passes -march=native; for the static
# Makefile we keep the long-standing core-libretro convention of allowing the
# user to override but defaulting to a safe x86_64 baseline.  parallel-gs +
# Granite need at least SSE3 (cmake hardcodes -msse3 for granite-vulkan and
# parallel-gs); enable it globally rather than per-target.
ifeq ($(IS_X86),1)
   CFLAGS   += -msse -msse2 -msse4.1 -mfxsr
   CXXFLAGS += -msse -msse2 -msse4.1 -mfxsr -msse3
endif

# GCC >= 12 refuses always_inline on exported (non-hidden) functions when
# building PIC, because ELF semantic interposition could replace the body at
# link time; vtlb.cpp (vtlb_ReassignHandler) and Counters.cpp (rcntRcount)
# fail to build with GCC 13 as a result. A libretro core never relies on
# symbol interposition, so disable interposition semantics whenever we build
# PIC with gcc/clang. MSVC platforms never set fpic, so no gating is needed
# beyond this.
ifneq (,$(fpic))
   fpic += -fno-semantic-interposition
   # Hide everything the core does not deliberately export. The libretro entry
   # points carry visibility("default") through RETRO_API, so they survive.
   #
   # This is not primarily about call overhead -- -fno-semantic-interposition
   # already lets internal calls bind directly. It is about data: without it,
   # a global reached from another translation unit goes through the GOT, an
   # extra load before every access. Counted as GOTPCREL relocations in the
   # helpers that emitted code calls into constantly:
   #
   #     Counters.cpp   127 -> 67
   #     vtlb.cpp       144 -> 97
   #     IopMem.cpp      49 -> 31
   #
   # It also drops the dynamic symbol table from 450 exported symbols to the
   # libretro entry points alone, which is a smaller relocation pass at load.
   #
   # The cmake build has had -fvisibility=hidden all along; this brings the
   # Makefile build in line.
   fpic += -fvisibility=hidden -fvisibility-inlines-hidden
endif

# Route the x86 emitter's encoding through the C89 macro core in
# common/emitter/c89emit.h, behind the header-only shim, instead of the
# out-of-line C++ implementations. Off by default.
#
# Toggling this needs a clean build. It changes header content rather than any
# source file, so make will not rebuild translation units that only include
# the emitter transitively, and the switched objects define their instruction
# objects with internal linkage. A stale object then fails to link against
# x86Emitter::xMOV and friends -- loudly, which is the good outcome, but only
# after a full compile.
ifeq ($(C89_EMITTER), 1)
   CXXFLAGS += -DPCSX2_C89_EMITTER
endif

# XE_AB=1: compile both the C89 body and the replaced C++ call into every
# converted site, runtime-selected by PCSX2_XE_CPP=1, for the emission-hash
# oracle. Roughly +40% text and real compile time in the recompiler TUs;
# verification workflow only, never release.

LDFLAGS += $(fpic) $(SHARED)
# Opt-in EE recompiler profiling. Off by default and entirely inside
# #ifdef PCSX2_REC_PROFILE, so a normal build is unaffected.
ifeq ($(PCSX2_REC_PROFILE), 1)
   FLAGS += -DPCSX2_REC_PROFILE
endif

FLAGS   += $(fpic) $(INCFLAGS)

FLAGS += $(ENDIANNESS_DEFINES) \
         $(WARNINGS) \
         $(CORE_DEFINE) \
         -DSTDC_HEADERS \
         -D__STDC_LIMIT_MACROS \
         -D__LIBRETRO__ \
         $(EXTRA_INCLUDES) \
         -D_FILE_OFFSET_BITS=64 \
         -D__STDC_CONSTANT_MACROS

ifneq (,$(findstring windows_msvc2017,$(platform)))
   FLAGS += -D_CRT_SECURE_NO_WARNINGS \
            -D_CRT_NONSTDC_NO_DEPRECATE \
            -D__ORDER_LITTLE_ENDIAN__ \
            -D__BYTE_ORDER__=__ORDER_LITTLE_ENDIAN__ \
            -DNOMINMAX= \
            //utf-8 \
            //std:c++17
            ifeq (,$(findstring windows_msvc2017_uwp,$(platform)))
               LDFLAGS += opengl32.lib
            endif
endif

# C++17 for everything (cmake sets cxx_std_17 on PCSX2_FLAGS / common). Note
# parallel-gs and Granite are written against C++14; -std=c++17 is a strict
# superset, so the same flag works.  We disable RTTI/exceptions to match cmake.
CXXFLAGS += -std=c++17 -fno-rtti -fno-exceptions
CFLAGS   += -std=gnu99
CXXFLAGS += $(FLAGS) $(CXX_WARNINGS)
CFLAGS   += $(FLAGS)

ifneq ($(SANITIZER),)
   CFLAGS   := -fsanitize=$(SANITIZER) $(CFLAGS)
   CXXFLAGS := -fsanitize=$(SANITIZER) $(CXXFLAGS)
   LDFLAGS  := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif

OBJOUT  = -o
LINKOUT = -o

ifneq (,$(findstring msvc,$(platform)))
   OBJOUT = -Fo
   LINKOUT = -out:
ifeq ($(STATIC_LINKING),1)
   LD ?= lib.exe
   STATIC_LINKING=0
else
   LD = link.exe
endif
else
   LD = $(CXX)
endif

# Objects are built in the source tree, and nothing in their names records
# which toolchain produced them. Building for a second architecture in the
# same checkout therefore leaves the first one's objects behind, and the link
# fails with "Relocations in generic ELF (EM: 183) -- file in wrong format",
# which says nothing about the actual cause. The stamp below records the
# toolchain; when it changes, the stale objects are removed.
#
# This runs at PARSE time, via $(shell), not as a recipe. A recipe is too
# late: make reads the generated .d files before running anything, so a
# depfile from the previous build referring to a header that has since been
# deleted aborts the build with "No rule to make target" before the cleanup
# would ever fire. Parse-time removal happens before those files are read.
BUILD_TAG := $(CC)|$(CXX)|$(platform)|$(arch)|$(IS_ARM64)|$(IS_X86)|$(HAVE_VULKAN)|$(HAVE_OPENGL)

BUILD_TAG_STATUS := $(shell \
	if [ ! -f .build-tag ] || [ "`cat .build-tag 2>/dev/null`" != "$(BUILD_TAG)" ]; then \
		if [ -f .build-tag ]; then \
			find . -name '*.o' -delete; \
			find . -name '*.d' -delete; \
			echo cleaned; \
		fi; \
		echo "$(BUILD_TAG)" > .build-tag; \
	fi)

ifeq ($(BUILD_TAG_STATUS),cleaned)
   $(info toolchain changed, removed objects from the previous build)
endif


$(TARGET): $(OBJECTS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $@ $(OBJECTS)
else
	@$(LD) $(LINKOUT)$@ $^ $(LDFLAGS) $(GL_LIB) $(LIBS)
endif

%.o: %.cpp
	@if [ $(SILENT) -ne 1 ]; then\
		$(if $@, $(shell echo echo CXX $<),);\
	fi
	$(CXX) -c $(OBJOUT)$@ $< $(CXXFLAGS)

%.o: %.cc
	@if [ $(SILENT) -ne 1 ]; then\
		$(if $@, $(shell echo echo CXX $<),);\
	fi
	$(CXX) -c $(OBJOUT)$@ $< $(CXXFLAGS)

%.o: %.c
	@if [ $(SILENT) -ne 1 ]; then\
		$(if $@, $(shell echo echo CC $<),);\
	fi
	$(CC) -c $(OBJOUT)$@ $< $(CFLAGS)

%.o: %.S
	$(CC) -c $(OBJOUT)$@ $< $(CFLAGS)

clean:
	@find . -name '*.o' -delete
	@find . -name '*.d' -delete
	rm -f $(TARGET) $(TARGET_TMP) .build-tag

.PHONY: clean
