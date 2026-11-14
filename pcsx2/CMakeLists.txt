# All flags, libraries, etc, that are shared between PCSX2 compilation files
# GameDatabaseBuiltin.c is generated from the shipped GameIndex.yaml at
# build time. The static Makefile uses tools/gen_gamedb_builtin.sh (POSIX
# shell + od + awk); CMake uses the byte-identical cmake -P twin so MSVC
# runners without a shell can build too.
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/GameDatabaseBuiltin.c
	COMMAND ${CMAKE_COMMAND}
		-DYAML=${CMAKE_SOURCE_DIR}/bin/resources/GameIndex.yaml
		-DOUT=${CMAKE_CURRENT_BINARY_DIR}/GameDatabaseBuiltin.c
		-P ${CMAKE_SOURCE_DIR}/tools/gen_gamedb_builtin.cmake
	DEPENDS ${CMAKE_SOURCE_DIR}/bin/resources/GameIndex.yaml
	        ${CMAKE_SOURCE_DIR}/tools/gen_gamedb_builtin.cmake
	COMMENT "Generating GameDatabaseBuiltin.c from GameIndex.yaml"
	VERBATIM)

add_library(PCSX2_FLAGS INTERFACE)

if(DISABLE_ADVANCE_SIMD OR LTO_PCSX2_CORE)
	# Fixes issues with some compiler + linker combinations
	add_library(PCSX2 OBJECT)
else()
	add_library(PCSX2)
endif()
target_link_libraries(PCSX2 PRIVATE PCSX2_FLAGS)

target_compile_features(PCSX2_FLAGS INTERFACE cxx_std_17)
target_compile_definitions(PCSX2_FLAGS INTERFACE "${PCSX2_DEFS}")
target_compile_options(PCSX2_FLAGS INTERFACE "${PCSX2_WARNINGS}")

# Check that people use the good file
if(NOT TOP_CMAKE_WAS_SOURCED)
	message(FATAL_ERROR "
	You did not 'cmake' the good CMakeLists.txt file. Use the one in the top dir.
	It is advice to delete all wrongly generated cmake stuff => CMakeFiles & CMakeCache.txt")
endif()

if(MSVC)
	if (NOT USE_CLANG_CL AND MSVC_VERSION GREATER_EQUAL 1930)
		target_compile_options(PCSX2_FLAGS INTERFACE /fp:contract)
	endif()
	target_compile_options(PCSX2_FLAGS INTERFACE /GS-)
else()
	target_compile_options(PCSX2_FLAGS INTERFACE
		-ffp-contract=fast
		-fno-strict-aliasing
		-Wstrict-aliasing # Allow to track strict aliasing issue.
		-Wno-parentheses
		-Wno-missing-braces
		-Wno-unknown-pragmas
	)
endif()

if(CCACHE_FOUND)
	set(CommonFlags ${CommonFlags} -Werror=date-time)
endif()

if(GCC_VERSION VERSION_EQUAL "8.0" OR GCC_VERSION VERSION_GREATER "8.0")
	# gs is pretty bad at this. -Wno-class-memaccess is C++-only; GCC's cc1
	# warns per C TU when it arrives unscoped (the aarch64 log spam), so
	# gate it by language.
	target_compile_options(PCSX2_FLAGS INTERFACE -Wno-packed-not-aligned
		"$<$<COMPILE_LANGUAGE:CXX>:-Wno-class-memaccess>")
endif()

if ("${PGO}" STREQUAL "generate")
	target_compile_options(PCSX2_FLAGS INTERFACE -fprofile-generate)
elseif("${PGO}" STREQUAL "use")
	target_compile_options(PCSX2_FLAGS INTERFACE -fprofile-use)
endif()

if(WIN32)
	set(MIN_WIN32 0x0A00)
	target_compile_definitions(PCSX2_FLAGS INTERFACE
		__SSE4_1__
		WINVER=${MIN_WIN32}
		_WIN32_WINNT=${MIN_WIN32}
		WIN32_LEAN_AND_MEAN
	)
	target_include_directories(PCSX2_FLAGS INTERFACE
		../3rdparty # GL headers
	)
endif(WIN32)

if(ANDROID)
	# bionic declares struct ifaddrs unconditionally but gates getifaddrs()
	# and freeifaddrs() on __INTRODUCED_IN(24), and the NDK lanes target 21,
	# so DEV9 fails to compile there. WANT_IFADDRS is libretro-common's own
	# switch for this: it redirects the DEV9 includes to
	# <compat/ifaddrs.h> and brings in the netlink reimplementation added to
	# libretro/CMakeLists.txt. Keep in step with Makefile.common.
	target_compile_definitions(PCSX2_FLAGS INTERFACE WANT_IFADDRS)
endif()

# Main pcsx2 source
set(pcsx2Sources
	Cache.c
	COP0.c
	Counters.c
	EmuOptions.c
	GameDatabase.c
	GameDatabaseFixes.c
	${CMAKE_CURRENT_BINARY_DIR}/GameDatabaseBuiltin.c
	Elfheader.c
	FW.c
	FiFo.c
	FPU.c
	ps2float.c
	IopGte.c
	COP2.c
	Sif.c
	Sif0.c
	Sif1.c
	sif2.c
	DmacChannel.c
	SPR.c
	Dmac.c
	Hw.c
	Vif0_Dma.c
	Vif1_Dma.c
	Vif1_MFIFO.c
	SaveStateBase.c
	SaveStateFreeze.c
	Gif.c
	Gif_Unit.c
	GS.c
	HostFS.c
	Interpreter.c
	IopBios.c
	IopCounters.c
	IopDma.c
	IopHw.c
	IopIrq.c
	IopMem.c
	Mdec.c
	Memory.c
	MemoryCardFile.c
	memcard_ecc.c
	NearPlace.c
	MemoryCardProtocol.c
	MMI.c
	MTGS.c
	MTVU.c
	MultitapProtocol.c
	Patch.c
	Pcsx2Config.c
	PerformanceMetrics.c
	R3000A.c
	R3000AInterpreter.c
	R3000AOpcodeTables.c
	R5900.c
	R5900OpcodeImpl.c
	R5900OpcodeTables.c
	Sio.c
	SioFifo.c
	Vif.c
	Vif_Codes.c
	Vif_Transfer.c
	Vif_Unpack.c
	VirtualMemory.c
	vtlb.c
	GoemonTlb.c
	VU0.c
	VUmicro.c
	VU0micro.c
	VU0microInterp.c
	VU1micro.c
	VU1microInterp.c
	VUmicroMem.c
	VUops.c)

# Main pcsx2 header
set(pcsx2Headers
	Cache.h
	Common.h
	Config.h
	COP0.h
	Counters.h
	Dmac.h
	EmuOptions.h
	GameDatabase.h
	Elfheader.h
	FW.h
	Gif.h
	Gif_hw.h
	Gif_Unit.h
	GS.h
	GSVideoMode.h
	VMHost.h
	HotConfig.h
	Hardware.h
	Host.h
	HostFS.h
	Hw.h
	IopBios.h
	IopCounters.h
	IopDma.h
	IopGte.h
	IopHw.h
	IopMem.h
	Mdec.h
	MTVU.h
	Memory.h
	MemoryCardFile.h
	memcard_ecc.h
	NearPlace.h
	GSmmio.h
	MemoryCardProtocol.h
	MemoryTypes.h
	MultitapProtocol.h
	ParseNumber.h
	Patch.h
	PerformanceMetrics.h
	R3000A.h
	R5900.h
	R5900OpcodeTables.h
	FreezeTypes.h
	SaveState.h
	ShaderCacheVersion.h
	Sif.h
	Sio.h
	SioFifo.h
	SPR.h
	Vif_Dma.h
	Vif.h
	Vif_Unpack.h
	Vif_Dynarec.h
	Vif_HashBucket.h
	VirtualMemory.h
	vtlb.h
	ps2float.h
	VUmicro.h
	VUops.h
	microVU/microVU_Types.h
	microVU/microVU_Const.h
	microVU/microVU_Structs.h
	microVU/microVU_BlockManager.inl
	microVU/microVU_Analyze.inl
	microVU/microVU_Pipeline.inl)

# CDVD sources
set(pcsx2CDVDSources
	CDVD/Ps1CD.c
	CDVD/CDVDcommon.c
	CDVD/CDVD.c
	CDVD/CDVDnvram.c
	CDVD/MechaMg.c
	CDVD/MgDes.c
	CDVD/CDVDisoReader.c
	CDVD/cdvd_reader.c
	CDVD/cdvd_flat.c
	CDVD/cdvd_cso.c
	CDVD/cdvd_chd.c
	CDVD/cdvd_gz.c
	CDVD/cdvd_iso.c
	CDVD/IsoFS/iso9660.c
	CDVD/IsoFS/IsoFSCDVD.c
	)

# CDVD headers
set(pcsx2CDVDHeaders
	CDVD/Ps1CD.h
	CDVD/CDVDcommon.h
	CDVD/CDVD.h
	CDVD/CDVDnvram.h
	CDVD/MechaMg.h
	CDVD/MgDes.h
	CDVD/CDVD_internal.h
	CDVD/IsoFS/iso9660.h
	CDVD/CDVD_host.h
	CDVD/zlib_indexed.h
	CDVD/cdvd_reader.h
	CDVD/cdvd_iso.h
	)

# SPU2 sources
set(pcsx2SPU2Sources
	SPU2/ADSR.c
	SPU2/Dma.c
	SPU2/Mixer.c
	SPU2/ReadInput.c
	SPU2/RegTable.c
	SPU2/Reverb.c
	SPU2/ReverbResample.c
	SPU2/spu2.c
	SPU2/spu2freeze.c
	SPU2/spu2sys.c
)

# No SPU2 source needs the MULTI_ISA per-ISA machinery: the reverb
# resampler is one 128-bit path, built once like the rest of SPU2.
set(pcsx2SPU2SourcesUnshared)

# SPU2 headers
set(pcsx2SPU2Headers
	SPU2/defs.h
	SPU2/Dma.h
	SPU2/Global.h
	SPU2/interpolate_table.h
	SPU2/Mixer.h
	SPU2/spu2.h
	SPU2/regs.h
	SPU2/SndOut.h
	SPU2/spdif.h
)

# Host PAD
set(pcsx2PADSources
	PAD/PAD.c
)
set(pcsx2PADHeaders
	PAD/PAD.h
	PAD/PADsio.h
)

# GS sources
set(pcsx2GSSourcesUnshared
	GS/GSBlock.cpp
	GS/GSLocalMemoryMultiISA.cpp
	GS/Renderers/Common/GSVertexTraceFMM.cpp
	GS/Renderers/HW/GSRendererHWMultiISA.cpp
	GS/Renderers/SW/GSDrawScanline.cpp
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.cpp
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp
	GS/Renderers/SW/GSRasterizer.cpp
	GS/Renderers/SW/GSRendererSW.cpp
	GS/Renderers/SW/GSSetupPrimCodeGenerator.cpp
	GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp
)

# The software rasterizer JITs scanlines with x86 xbyak (GS*CodeGenerator) and
# has no arm64 backend here, so drop the whole SW renderer on aarch64; the HW
# renderer and paraLLEl-GS cover it (GS.cpp gates the SW path on ARCH_ARM64).
# Only the xbyak code generators are x86-only; the rasterizer, the renderer
# and the scanline dispatcher build anywhere GSVector4/4i does, and
# GSDrawScanline uses its C++ reference path when the generators are absent.
if(NOT _M_X86)
	list(REMOVE_ITEM pcsx2GSSourcesUnshared
		GS/Renderers/SW/GSDrawScanlineCodeGenerator.cpp
		GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp
		GS/Renderers/SW/GSSetupPrimCodeGenerator.cpp
		GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp
	)
endif()

set(pcsx2GSSources
	GS/GS.cpp
	GS/GSClutGPU.cpp
	GS/GSClut.c
	GS/GSDrawingContext.c
	GS/GSLocalMemoryMaps.c
	GS/GSLocalMemory.cpp
	GS/GSRingHeap.c
	GS/GSState.cpp
	GS/GSOffset.c
	GS/GSPsm.c
	GS/GSTables.c
	GS/GSUtil.c
	GS/GSVector.cpp
	GS/Renderers/Common/GSDevice.cpp
	GS/Renderers/Common/GSDirtyRect.c
	GS/Renderers/Common/GSCodeReserve.c
	GS/Renderers/Common/GSRenderer.cpp
	GS/Renderers/Common/GSVertexTrace.cpp
	GS/Renderers/HW/GSHwHack.cpp
	GS/Renderers/HW/GSRendererHW.cpp
	GS/Renderers/HW/GSObjectPool.c
	GS/Renderers/Common/GSFlatMap.c
	GS/Renderers/Common/GSStableMap.c
	GS/Renderers/Common/GSTextureBase.c
	GS/Renderers/Common/GSTextureFormat.c
	GS/GSPage2Tile.c
	GS/Renderers/HW/GSHashTable.c
	GS/Renderers/HW/GSAlphaRange.h
	GS/Renderers/HW/GSHashTable.h
	GS/Renderers/HW/GSSpriteSnap.c
	GS/Renderers/HW/GSSpriteSnap.h
	GS/Renderers/HW/GSTargetBudget.c
	GS/Renderers/HW/GSTargetBudget.h
	GS/Renderers/Common/GSStableGroup.c
	GS/Renderers/Common/GSStableGroup.h
	GS/Renderers/Common/GSRetireRing.c
	GS/Renderers/Common/GSRetireRing.h
	GS/Renderers/Common/GSScratch.c
	GS/Renderers/Common/GSScratch.h
	GS/Renderers/HW/GSObjectPool.h
	GS/Renderers/HW/GSTextureCache.cpp
	GS/Renderers/SW/GSDeviceSW.cpp
	GS/Renderers/SW/GSHiresMem.c
	GS/Renderers/SW/GSHiresMem.h
	GS/Renderers/SW/GSTextureCacheSW.cpp
	GS/Renderers/SW/GSTextureSW.c
	)

# GS headers
set(pcsx2GSHeaders
	GS/GSAlignedClass.h
	GS/GSBlock.h
	GS/GSClut.h
	GS/GSDrawingContext.h
	GS/GSDrawingEnvironment.h
	GS/GSExtra.h
	GS/GSOptionEnums.h
	GS/GSRegs.h
	GS/GS.h
	GS/GSLocalMemory.h
	GS/GSRingHeap.h
	GS/GSState.h
	GS/GSLocalMemoryMaps.h
	GS/GSOffset.h
	GS/GSPsm.h
	GS/GSTables.h
	GS/GSUtil.h
	GS/GSVR.h
	GS/GSVector.h
	GS/GSVector4.h
	GS/GSVector4i.h
	GS/GSVector8.h
	GS/GSVector8i.h
	GS/MultiISA.h
	GS/Renderers/Common/GSDevice.h
	GS/Renderers/Common/GSDirtyRect.h
	GS/Renderers/Common/GSFastList.h
	GS/Renderers/Common/GSKeyHash.h
	GS/Renderers/Common/GSArray.h
	GS/Renderers/Common/GSString.h
	GS/Renderers/Common/GSFlatMap.h
	GS/Renderers/Common/GSStableMap.h
	GS/GSPage2Tile.h
	GS/Renderers/Common/GSCodeReserve.h
	GS/Renderers/Common/GSFunctionMap.h
	GS/Renderers/Common/GSRenderer.h
	GS/Renderers/Common/GSTexture.h
	GS/Renderers/Common/GSTextureBase.h
	GS/Renderers/Common/GSTextureFormat.h
	GS/Renderers/Common/GSVertex.h
	GS/Renderers/Common/gs_vertex.h
	GS/Renderers/Common/GSVertexTrace.h
	GS/Renderers/Common/vr_glsl_body.h
	GS/Renderers/HW/GSHwHack.h
	GS/Renderers/HW/GSRendererHW.h
	GS/Renderers/HW/GSTextureCache.h
	GS/Renderers/HW/GSVertexHW.h
	GS/Renderers/SW/GSDeviceSW.h
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.h
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.h
	GS/Renderers/SW/GSDrawScanline.h
	GS/Renderers/SW/GSNewCodeGenerator.h
	GS/Renderers/SW/GSRasterizer.h
	GS/Renderers/SW/GSRendererSW.h
	GS/Renderers/SW/GSScanlineEnvironment.h
	GS/Renderers/SW/GSSetupPrimCodeGenerator.h
	GS/Renderers/SW/GSSetupPrimCodeGenerator.all.h
	GS/Renderers/SW/GSTextureCacheSW.h
	GS/Renderers/SW/GSTextureSW.h
	GS/Renderers/SW/GSVertexSW.h
	)

if(USE_OPENGL)
	list(APPEND pcsx2GSSources
		GS/Renderers/OpenGL/GLState.c
		GS/Renderers/OpenGL/GSDeviceOGL.cpp
		GS/Renderers/OpenGL/GSTextureOGL.c
		GS/Renderers/OpenGL/GLContext.c
		GS/Renderers/OpenGL/GLProgram.c
		GS/Renderers/OpenGL/GLProgramCache.c
		GS/Renderers/OpenGL/GLShaderCache.c
		GS/Renderers/OpenGL/GLStreamBuffer.c
		GS/Renderers/OpenGL/GLTextureFormat.c
		GS/Renderers/OpenGL/GLVertexInput.c
	)
	list(APPEND pcsx2GSHeaders
		GS/Renderers/OpenGL/GLState.h
		GS/Renderers/OpenGL/GSDeviceOGL.h
		GS/Renderers/OpenGL/GSTextureOGL.h
		GS/Renderers/OpenGL/GLContext.h
		GS/Renderers/OpenGL/GLDevice.h
		GS/Renderers/OpenGL/GLProgram.h
		GS/Renderers/OpenGL/GLProgramCache.h
		GS/Renderers/OpenGL/GLShaderCache.h
		GS/Renderers/OpenGL/GLStreamBuffer.h
		GS/Renderers/OpenGL/GLTextureFormat.h
		GS/Renderers/OpenGL/GLVertexInput.h
	)
	target_link_libraries(PCSX2_FLAGS INTERFACE glad)
endif()

if(USE_VULKAN)
	list(APPEND pcsx2GSSources
		GS/Renderers/Vulkan/GSDeviceVK.cpp
		GS/Renderers/Vulkan/GSTextureVK.cpp
		GS/Renderers/Vulkan/VKLoader.cpp
		GS/Renderers/Vulkan/VKStreamBuffer.c
		GS/Renderers/Vulkan/VKBuilders.cpp
		GS/Renderers/Vulkan/GSVulkanHeap.c
		GS/Renderers/Vulkan/VKSpirvPatch.c
	)
	list(APPEND pcsx2GSHeaders
		GS/Renderers/Vulkan/GSVulkanHeap.h
		GS/Renderers/Vulkan/GSDeviceVK.h
		GS/Renderers/Vulkan/GSTextureVK.h
		GS/Renderers/Vulkan/VKBuilders.h
		GS/Renderers/Vulkan/VKStreamBuffer.h
		GS/Renderers/Vulkan/VKShaders.inc
		GS/Renderers/Vulkan/VKPipelineKey.h
		GS/Renderers/Vulkan/VKSpirvPatch.h
		GS/Renderers/Vulkan/VKEntryPoints.h
		GS/Renderers/Vulkan/VKLoader.h
		GS/Renderers/Vulkan/VKLoaderPlatformDefines.h
	)

	message("Adding support for paraLLEl-GS")
	add_subdirectory(GS/parallel-gs EXCLUDE_FROM_ALL)
	target_link_libraries(PCSX2_FLAGS INTERFACE parallel-gs)
	target_compile_definitions(PCSX2_FLAGS INTERFACE HAVE_PARALLEL_GS)
	list(APPEND pcsx2GSSources GS/Renderers/parallel-gs/GSRendererPGS.cpp)
	list(APPEND pcsx2GSHeaders GS/Renderers/parallel-gs/GSRendererPGS.h GS/Renderers/parallel-gs/PGSStateVRAM.h)

	target_link_libraries(common INTERFACE
		Vulkan-Headers
	)
	if(APPLE)
		# Needed for Metal surface creation.
		target_compile_options(common INTERFACE -fobjc-arc)
		target_link_options(common INTERFACE -fobjc-link-runtime)
	endif()
endif()

if(WIN32)

target_compile_options(PCSX2_FLAGS INTERFACE "-DHAVE_D3D11")
target_compile_options(PCSX2_FLAGS INTERFACE "-DHAVE_D3D12")

	list(APPEND pcsx2GSSources
		GS/Renderers/DX11/D3D.cpp
		GS/Renderers/DX11/GSDevice11.cpp
		GS/Renderers/DX11/GSTexture11.cpp
		GS/Renderers/DX11/D3D11ShaderCache.cpp
		GS/Renderers/DX12/GSDevice12.cpp
		GS/Renderers/DX12/GSTexture12.cpp
		GS/Renderers/DX12/D3D12Builders.cpp
		GS/Renderers/DX12/D3D12DescriptorHeapManager.cpp
		GS/Renderers/DX12/D3D12ShaderCache.cpp
		GS/Renderers/DX12/D3D12StreamBuffer.cpp
		GS/Renderers/DX12/GSD3D12Heap.c
		GS/Renderers/DX12/GSD3D12Warm.c
	)
	list(APPEND pcsx2GSHeaders
		GS/Renderers/DX11/D3D.h
		GS/Renderers/DX11/D3DDynamicPS.h
		GS/Renderers/DX11/GSDevice11.h
		GS/Renderers/DX11/GSTexture11.h
		GS/Renderers/DX11/D3D11ShaderCache.h
		GS/Renderers/DX12/GSDevice12.h
		GS/Renderers/DX12/GSTexture12.h
		GS/Renderers/DX12/GSD3D12Heap.h
		GS/Renderers/DX12/GSD3D12Warm.h
		GS/Renderers/DX12/D3D12Builders.h
		GS/Renderers/DX12/D3D12DescriptorHeapManager.h
		GS/Renderers/DX12/D3D12ShaderCache.h
		GS/Renderers/DX12/D3D12StreamBuffer.h
	)
elseif(Linux)
endif()

# IPU sources
set(pcsx2IPUSources
	IPU/IPU.c
	IPU/IPU_Fifo.c
	IPU/IPUdither.c
	IPU/ipu_csc.c
	IPU/ipu_idct.c
	IPU/ipu_tables.c
	IPU/IPUdma.c
	IPU/yuv2rgb.c
)

set(pcsx2IPUSourcesUnshared
	IPU/IPU_MultiISA.c
)

# IPU headers
set(pcsx2IPUHeaders
	IPU/IPU.h
	IPU/IPU_hw.h
	IPU/IPU_Fifo.h
	IPU/IPU_MultiISA.h
	IPU/IPUdma.h
	IPU/ipu_dct_codes.h
	IPU/ipu_dct_lut.h
	IPU/ipu_tables.h
	IPU/ipu_modes.h
	IPU/ipu_vlc.h
	IPU/ipu_vlc_codes.h
	IPU/ipu_bitstream.h
	IPU/ipu_bp.h
	IPU/ipu_csc.h
	IPU/ipu_decoder.h
	IPU/ipu_status.h
	IPU/ipu_idct.h
	IPU/yuv2rgb.h
)

# The multi-ISA tier build (sse4/avx/avx2 unshared libs) is x86-only: it bakes
# x86 SIMD feature flags into per-tier copies. On aarch64 there is a single NEON
# tier, so fall through to the else() branch and compile the unshared GS/IPU/SPU2
# sources once (mirrors the modern PCSX2/ARMSX2 layout).
if(DISABLE_ADVANCE_SIMD AND _M_X86)
	target_compile_definitions(PCSX2 PUBLIC MULTI_ISA_SHARED_COMPILATION)
	if(USE_GCC)
		target_link_options(PCSX2_FLAGS INTERFACE -Wno-odr)
	endif()
	if(WIN32)
		set(compile_options_avx2 /arch:AVX2)
		set(compile_options_avx  /arch:AVX)
	elseif(USE_GCC)
		# GCC can't inline into multi-isa functions if we use march and mtune, but can if we use feature flags
		set(compile_options_avx2 -msse4.1 -mavx -mavx2 -mbmi -mbmi2 -mfma)
		set(compile_options_avx  -msse4.1 -mavx)
		set(compile_options_sse4 -msse4.1)
	else()
		set(compile_options_avx2 -march=haswell -mtune=haswell)
		set(compile_options_avx  -march=sandybridge -mtune=sandybridge)
		set(compile_options_sse4 -msse4.1 -mtune=nehalem)
	endif()
	# ODR violation time!
	# Everything would be fine if we only defined things in cpp files, but C++ tends to like inline functions (STL anyone?)
	# Each ISA will bring with it its own copies of these inline header functions, and the linker gets to choose whichever one it wants!  Not fun if the linker chooses the avx2 version and uses it with everything
	# Thankfully, most linkers don't choose at random.  When presented with a bunch of .o files, most linkers seem to choose the first implementation they see, so make sure you order these from oldest to newest
	# Note: ld64 (macOS's linker) does not act the same way when presented with .a files, unless linked with `-force_load` (cmake WHOLE_ARCHIVE).
	set(is_first_isa "1")
	foreach(isa "sse4" "avx" "avx2")
		add_library(GS-${isa} STATIC ${pcsx2GSSourcesUnshared} ${pcsx2IPUSourcesUnshared} ${pcsx2SPU2SourcesUnshared})
		target_link_libraries(GS-${isa} PRIVATE PCSX2_FLAGS)
		target_compile_definitions(GS-${isa} PRIVATE MULTI_ISA_UNSHARED_COMPILATION=isa_${isa} MULTI_ISA_IS_FIRST=${is_first_isa} ${pcsx2_defs_${isa}})
		target_compile_options(GS-${isa} PRIVATE ${compile_options_${isa}})
		if (${CMAKE_VERSION} VERSION_GREATER_EQUAL 3.24)
			target_link_libraries(PCSX2 PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,GS-${isa}>)
		elseif(APPLE)
			message(FATAL_ERROR "MacOS builds with DISABLE_ADVANCE_SIMD=ON require CMake 3.24")
		else()
			target_link_libraries(PCSX2 PRIVATE GS-${isa})
		endif()
		set(is_first_isa "0")
	endforeach()
else()
	list(APPEND pcsx2GSSources ${pcsx2GSSourcesUnshared})
	list(APPEND pcsx2IPUSources ${pcsx2IPUSourcesUnshared})
	list(APPEND pcsx2SPU2Sources ${pcsx2SPU2SourcesUnshared})
endif()

list(APPEND pcsx2FrontendSources
	VMManager.c
)
list(APPEND pcsx2FrontendHeaders
	VMManager.h)

# Linux sources
set(pcsx2LinuxSources
	)

set(pcsx2OSXSources
	)

set(pcsx2FreeBSDSources
	)

# Linux headers
set(pcsx2LinuxHeaders
	)

# ps2 sources
set(pcsx2ps2Sources
	ps2/BiosTools.c
	ps2/pgif.c
	ps2/Iop/IopHwRead.c
	ps2/Iop/IopHwWrite.c
	ps2/Iop/PsxBios.c)

# ps2 headers
set(pcsx2ps2Headers
	ps2/BiosTools.h
	ps2/pgif.h
	ps2/pgifio.h
	ps2/HwInternal.h
	ps2/Iop/IopHw_Internal.h)

# Windows sources
set(pcsx2WindowsSources
)

# x86 sources
set(pcsx2x86Sources
	x86/CpuState.c
	x86/BaseblockEx.c
	x86/iCOP0.c
	x86/iCore.c
	x86/iFPU.c
	x86/iFPUd.c
	x86/iMMI.c
	x86/JitHash.c
	x86/iR3000A.c
	x86/iR3000Atables.c
	x86/iR5900Analysis.c
	x86/iR5900Misc.c
	x86/ix86-32/iCore-32.c
	x86/ix86-32/iR5900-32.c
	x86/ix86-32/iR5900Arit.c
	x86/ix86-32/iR5900AritImm.c
	x86/ix86-32/iR5900Branch.c
	x86/ix86-32/iR5900Jump.c
	x86/ix86-32/iR5900LoadStore.c
	x86/ix86-32/iR5900Move.c
	x86/ix86-32/iR5900MultDiv.c
	x86/ix86-32/iR5900Shift.c
	x86/ix86-32/iR5900Templates.c
	x86/ix86-32/recVTLB.c
	x86/newVif_Dynarec.c
	x86/newVif_UnpackSSE.c
	)

# x86 headers
set(pcsx2x86Headers
	x86/BaseblockEx.h
	x86/iCOP0.h
	x86/iCore.h
	x86/iFPU.h
	x86/iMMI.h
	x86/iR3000A.h
	x86/iR5900Arit.h
	x86/iR5900AritImm.h
	x86/iR5900Branch.h
	x86/iR5900.h
	x86/iR5900Analysis.h
	x86/iR5900Jump.h
	x86/iR5900LoadStore.h
	x86/iR5900Move.h
	x86/iR5900MultDiv.h
	x86/iR5900Shift.h
	x86/microVU_Alloc.inl
	x86/microVU_Branch.inl
	x86/microVU_Clamp.inl
	x86/microVU_Compile.inl
	x86/microVU.c
	x86/microVU_Execute.inl
	x86/microVU_Flags.inl
	x86/microVU.h
	x86/microVU_IR.h
	x86/microVU_Lower.inl
	x86/microVU_Macro.inl
	x86/microVU_Misc.h
	x86/microVU_Misc.inl
	x86/microVU_Tables.inl
	x86/microVU_Upper.inl
	x86/newVif.h
	x86/newVif_UnpackSSE.h
	)

# DEV9: the SPEED expansion (sockets ethernet + C ATA hard drive).
# The pcap backend is opt-in behind PCAP: it hard-links libpcap on
# POSIX, and the Sockets api covers ethernet without any system
# dependency.
set(pcsx2DEV9Sources
	DEV9/AdapterUtils.c
	DEV9/DEV9.c
	DEV9/InternalServers/DHCP_Server.c
	DEV9/InternalServers/DNS_Server.c
	DEV9/PacketReader/netpkt.c
	DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.c
	DEV9/Sessions/Session.c
	DEV9/Sessions/ICMP_Session/ICMP_Session.c
	DEV9/Sessions/TCP_Session/TCP_Session.c
	DEV9/Sessions/UDP_Session/UDP_Session.c
	DEV9/flash.c
	DEV9/net.c
	DEV9/smap.c
	DEV9/sockets.c
	DEV9/ATA/ATA_Info.c
	DEV9/ATA/ATA_State.c
	DEV9/ATA/ATA_Transfer.c
	DEV9/ATA/Commands/ATA_CmdDMA.c
	DEV9/ATA/Commands/ATA_CmdExecuteDeviceDiag.c
	DEV9/ATA/Commands/ATA_CmdNoData.c
	DEV9/ATA/Commands/ATA_CmdPIOData.c
	DEV9/ATA/Commands/ATA_CmdSMART.c
	DEV9/ATA/Commands/ATA_Command.c
	DEV9/ATA/Commands/ATA_SCE.c
	DEV9/ATA/HddCreate.c
	DEV9/ATA/HddImage.c
)
set(pcsx2DEV9Headers
	DEV9/ATA/ATA.h
	DEV9/ATA/HddCreate.h
	DEV9/ATA/HddImage.h
	DEV9/AdapterUtils.h
	DEV9/DEV9.h
	DEV9/DEV9irq.h
	DEV9/DEV9mmio.h
	DEV9/InternalServers/DHCP_Server.h
	DEV9/InternalServers/DNS_Server.h
	DEV9/PacketReader/netpkt.h
	DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.h
	DEV9/Sessions/Session.h
	DEV9/net.h
	DEV9/netio.h
	DEV9/smap.h
	DEV9/sockets.h
)
if(PCAP)
	list(APPEND pcsx2DEV9Sources DEV9/pcap_io.c)
	target_compile_definitions(PCSX2_FLAGS INTERFACE HAVE_PCAP)
	target_link_libraries(PCSX2_FLAGS INTERFACE pcap)
endif()
set(pcsx2USBSources
	USB/libretro-usb/bus.c
	USB/libretro-usb/core.c
	USB/libretro-usb/desc.c
	USB/libretro-usb/hid.c
	USB/libretro-usb/usb-hid.c
	USB/libretro-usb/usb-ohci.c
	USB/libretro-usb/ohci_dma.c
	USB/libretro-usb/ohci_dma.h
	USB/usb-python2/p2io.c
	USB/usb-python2/py2desc.c
	USB/usb-python2/usb-python2.c
	USB/usb-perfect-pool/ppcam.c
	FireWire/fatimg.c
	FireWire/p1io.c
	FireWire/py1desc.c
	USB/usb-eyetoy/cam-frame.c
	USB/usb-eyetoy/cam-jpeg.c
	USB/usb-eyetoy/eyetoy-frontend.c
	USB/usb-eyetoy/eyetoy-host.c
	USB/usb-eyetoy/jo_mpeg.c
	USB/usb-eyetoy/usb-eyetoy-webcam.c
)
set(pcsx2USBHeaders
	USB/USB.h
	USB/libretro-usb/USBinternal.h
	USB/libretro-usb/desc.h
	USB/libretro-usb/hid.h
	USB/libretro-usb/qusb.h
	USB/libretro-usb/queue.h
	USB/libretro-usb/usb-hid.h
	USB/libretro-usb/usb-state.h
	USB/usb-host.h
	USB/usb-eyetoy/eyetoy.h
	USB/usb-python2/p2io.h
	USB/usb-python2/py2desc.h
	USB/usb-python2/usb-python2.h
	USB/usb-perfect-pool/ppcam.h
	FireWire/fatimg.h
	FireWire/fw_device.h
	FireWire/p1io.h
	FireWire/py1desc.h
	USB/usb-eyetoy/cam-frame.h
	USB/usb-eyetoy/cam-jpeg.h
	USB/usb-eyetoy/eyetoy-frontend.h
	USB/usb-eyetoy/eyetoy-host.h
	USB/usb-eyetoy/jo_mpeg.h
	USB/usb-eyetoy/ov519.h
	USB/usb-eyetoy/usb-eyetoy-webcam.h
	USB/usb-eyetoy/videodev.h
)
set(pcsx2ZipToolsSources)
set(pcsx2ZipToolsHeaders)

# aarch64 (arm64 port, Phase B): drop the x86 recompiler sources (EE/IOP/VU
# dynarec, recVTLB, SSE VIF unpack). They have no arm64 backend yet and the
# interpreters are used instead; a hardware renderer (paraLLEl-GS) avoids the
# x86 GS SW rasterizer codegen. Emptied here so the lists below expand to
# nothing on aarch64 while x86_64 is unaffected.
if(NOT _M_X86)
	# CpuState.c lives under x86/ but only defines the EE/IOP/FPU register
	# state (cpuRegs/psxRegs/fpuRegs) that the interpreter needs too, so keep it.
	# Everything else here is the x86 recompiler; arm64 provides its own
	# recompilers (recR3000A/recR5900/recVU1) plus the NEON VIF unpack dynarec
	# transplanted from ARMSX2 (C.19: AsmHelpers + Vif_UnpackNEON +
	# Vif_Dynarec_arm64, replacing the former RecStubs.cpp reference routing).
	# JitHash.c is portable: on non-x86 it compiles to the exported
	# pcsx2_jithash_dump stub that libretro/main.c calls unconditionally
	# from retro_deinit. Dropping it from this list is an aarch64 link error.
	set(pcsx2x86Sources x86/CpuState.c x86/JitHash.c arm64/recR3000A_arm64.cpp arm64/recR5900_arm64.cpp arm64/aVU.cpp
		arm64/AsmHelpers.cpp arm64/Vif_UnpackNEON.cpp arm64/Vif_Dynarec_arm64.cpp)
	set(pcsx2x86Headers)
	# VIXL is the AArch64 code emitter, the foundation for the arm64 recompiler
	# (Phase C). Wire it through the flags interface (like parallel-gs/glad above)
	# so its PUBLIC include dir and static lib reach both PCSX2 and PCSX2_LTO.
	target_link_libraries(PCSX2_FLAGS INTERFACE vixl)
endif()

# These ones benefit a lot from LTO
set(pcsx2LTOSources
	${pcsx2Sources}
	${pcsx2Headers}
	${pcsx2IPUSources}
	${pcsx2IPUHeaders}
	${pcsx2x86Sources}
	${pcsx2x86Headers}
	${pcsx2SPU2Sources}
	${pcsx2SPU2Headers}
	${pcsx2GSSources}
	${pcsx2GSHeaders}
)

if(LTO_PCSX2_CORE)
	add_library(PCSX2_LTO ${pcsx2LTOSources})
	if (WIN32)
		target_compile_definitions(PCSX2_LTO PRIVATE _M_SSE=0x401)
	endif()
	if (DISABLE_ADVANCE_SIMD)
		target_compile_definitions(PCSX2_LTO PRIVATE MULTI_ISA_SHARED_COMPILATION)
	endif()
	target_link_libraries(PCSX2_LTO PRIVATE PCSX2_FLAGS)
	target_link_libraries(PCSX2 PRIVATE PCSX2_LTO)
	set_target_properties(PCSX2_LTO PROPERTIES INTERPROCEDURAL_OPTIMIZATION true)
	fixup_file_properties(PCSX2_LTO)
else()
	target_sources(PCSX2 PRIVATE ${pcsx2LTOSources})
endif()

# common Sources
target_sources(PCSX2 PRIVATE
	${pcsx2CDVDSources}
	${pcsx2CDVDHeaders}
	${pcsx2DEV9Sources}
	${pcsx2DEV9Headers}
	${pcsx2PADHeaders}
	${pcsx2PADSources}
	${pcsx2FrontendSources}
	${pcsx2FrontendHeaders}
	${pcsx2ps2Sources}
	${pcsx2ps2Headers}
)

# platform sources
# Linux
if(Linux)
	target_sources(PCSX2 PRIVATE
		${pcsx2LinuxSources}
		${pcsx2LinuxHeaders}
		)

	target_link_libraries(PCSX2_FLAGS INTERFACE
	)
endif()

# Windows
if(Windows)
	target_sources(PCSX2 PRIVATE
		${pcsx2WindowsSources}
	)
endif()

target_sources(PCSX2 PRIVATE ${pcsx2USBSources} ${pcsx2USBHeaders})

# x86emitter sources: x86-only JIT code emitter. Excluded on aarch64, where the
# recompilers that use it are gated out and the interpreters run instead (arm64
# port, Phase B). There is no arm64 emitter yet (that is Phase C).
# The reference emitter implementation lives with the verification harness in
# tests/emitter/reference; the byte oracles compare the C89 core against it.
# With C89_EMITTER enabled the build emits through the header-only core in
# emitter/c89emit.h and compiles none of these.
if(_M_X86 AND NOT C89_EMITTER)
	target_sources(PCSX2 PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/avx.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/groups.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/jmp.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/legacy.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/legacy_sse.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/movs.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/simd.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/x86emitter.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/internal.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/legacy_internal.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/avx.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/group1.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/group2.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/group3.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/incdec.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/jmpcall.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/movs.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_arithmetic.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_comparisons.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_helpers.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_moremovs.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_shufflepack.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/test.h
	)
endif()


# MacOSX/BSD
if(UNIX AND NOT Linux)
	if(APPLE)
		target_sources(PCSX2 PRIVATE
			${pcsx2OSXSources})
	else()
		target_sources(PCSX2 PRIVATE
			${pcsx2FreeBSDSources})
	endif()
	target_sources(PCSX2 PRIVATE
		${pcsx2LinuxHeaders})
endif()

target_link_libraries(PCSX2_FLAGS INTERFACE
	common
	${LIBC_LIBRARIES}
)

# Outside Windows there is nothing to link for GL: the renderer gets its
# entry points from the frontend.
if(WIN32)
	target_link_libraries(PCSX2_FLAGS INTERFACE
		d3dcompiler.lib
		d3d11.lib
		d3d12.lib
		dxgi.lib
		opengl32.lib
		OneCore.lib
	)
endif()

# additonal include directories
target_include_directories(PCSX2_FLAGS INTERFACE
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
	.
	x86
	${CMAKE_BINARY_DIR}/pcsx2
	${CMAKE_BINARY_DIR}/common/include/
	"${CMAKE_SOURCE_DIR}/3rdparty/xbyak/"
	"${FFMPEG_INCLUDE_DIRS}"
)

if (NOT APPLE)
	set_target_properties(PCSX2 PROPERTIES
		OUTPUT_NAME pcsx2
	)
endif()

fixup_file_properties(PCSX2)

if (APPLE)
	find_library(QUARTZCORE_LIBRARY QuartzCore)
	target_link_libraries(PCSX2_FLAGS INTERFACE ${METAL_LIBRARY} ${QUARTZCORE_LIBRARY})

	# MacOS defaults to having a maximum protection of the __DATA segment of rw (non-executable)
	# We have a bunch of page-sized arrays in bss that we use for jit
	# Obviously not being able to make those arrays executable would be a problem
	# -- x86 only: the exec-from-BSS arrays live in x86 code, and arm64 forbids
	# rwx segments outright (the arm64 recompilers mmap their caches instead).
	if(NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
		target_link_options(PCSX2_FLAGS INTERFACE -Wl,-segprot,__DATA,rwx,rw)
	endif()
endif()

set_property(GLOBAL PROPERTY PCSX2_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

source_groups_from_vcxproj_filters(pcsx2core.vcxproj.filters)

# Unix-only files aren't in the vcxproj.filters
source_group(System/Ps2/DEV9 REGULAR_EXPRESSION DEV9/*)
source_group(System/Ps2/PAD FILES ${pcsx2PADSources} ${pcsx2PADHeaders})
source_group(System/Ps2/SPU2 REGULAR_EXPRESSION SPU2/*)
source_group(System/Ps2/USB REGULAR_EXPRESSION USB/*)

# The subsystem profiler is opt-in at configure time: -DPROFILER=ON. When off,
# neither the source nor the definition is part of the build, so the header's
# macros expand to nothing and no accumulators, clock reads or branches exist
# anywhere in the binary.
if(PROFILER)
	target_sources(PCSX2 PRIVATE Profiler.c Profiler.h)
	target_compile_definitions(PCSX2 PRIVATE ENABLE_PCSX2_PROFILER)
endif()
