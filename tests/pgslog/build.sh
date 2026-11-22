#!/bin/sh
# Granite's logs to the frontend's (GS/Renderers/parallel-gs/PGSLog.c),
# from any thread; and the Granite subset stays what it is: the util
# pieces libretro-common or the core replace are not back. Plain,
# sanitized where the host runs it, and for mingw-w64 and AArch64 when CC
# is those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
LC="$ROOT/libretro/libretro-common"
G="$ROOT/pcsx2/GS/parallel-gs/Granite"
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
LIBS="-lpthread"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; LIBS=""; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
TMP=${TMPDIR:-/tmp}/pgslog.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# Gone from Granite, for libretro-common or the core's own: the aligned
# allocator (memalign), logging (pgs_log), thread names (sthread_setname),
# the timer (cpu_features_get_time_usec) and the environment lookups
for f in util/aligned_alloc.cpp util/logging.cpp util/thread_name.cpp util/thread_name.hpp \
	util/timer.cpp util/timer.hpp util/environment.cpp util/environment.hpp; do
	if [ -e "$G/$f" ]; then echo "FAIL: Granite/$f is back"; exit 1; fi
done
if grep -rqE "LoggingInterface|set_thread_logging_interface|get_current_time_nsecs|set_current_thread_name|get_environment_" \
	"$ROOT/pcsx2/GS/parallel-gs" "$ROOT/pcsx2/GS/Renderers/parallel-gs" "$ROOT/Makefile.common" "$ROOT/pcsx2/CMakeLists.txt"; then
	echo "FAIL: a replaced Granite piece is used again"
	exit 1
fi

# Gone as dead weight: descriptor buffers and heaps (the core asks only
# for push descriptors), with the slab and slice allocators behind them
for f in util/slab_allocator.cpp util/slab_allocator.hpp; do
	if [ -e "$G/$f" ]; then echo "FAIL: Granite/$f is back"; exit 1; fi
done
if grep -rqE "VK_EXT_descriptor_buffer|VK_EXT_descriptor_heap|DescriptorBufferAllocator|SliceAllocator|SlabAllocator|DESCRIPTOR_HEAP_BIT|DESCRIPTOR_BUFFER_BIT|descriptorBuffer" \
	"$ROOT/pcsx2/GS/parallel-gs" "$ROOT/pcsx2/GS/Renderers/parallel-gs" "$ROOT/Makefile.common" "$ROOT/pcsx2/CMakeLists.txt"; then
	echo "FAIL: descriptor buffer or heap support is back in Granite"
	exit 1
fi

# Gone the same way: the context creation flags and the paths behind them
# (video queues, pipeline binaries, robustness2, presentation extensions),
# the frontend create-info inheritance, mesh and task stages, YCbCr
# conversions, ASTC decode modes, events and the never-set quirks, then
# what only presentation needed: surface prerotation and its internal
# specialisation constants, swapchain layouts, consumed semaphores; and
# Granite's own Vulkan loader, the core's VKLoader.c serving both; and
# memory imported or exported through OS handles; and Granite's memory
# allocator, device memory coming from the core's GSVulkanHeap.c; and
# render passes past one subpass of colour attachments; and the pipeline
# state no pass varies (depth, stencil, culling, topology, multisample
# coverage, vertex input) with the depth and stencil views behind it
for f in vulkan/pipeline_cache.cpp vulkan/pipeline_cache.hpp vulkan/vulkan_prerotate.hpp \
	vulkan/type_to_string.hpp util/hashmap.hpp util/unstable_remove_if.hpp \
	vulkan/vulkan_loader.c vulkan/vulkan_loader.h vulkan/vulkan_loader.inl \
	util/arena_allocator.cpp util/arena_allocator.hpp util/stack_allocator.hpp; do
	if [ -e "$G/$f" ]; then echo "FAIL: Granite/$f is back"; exit 1; fi
done
if grep -rqE "CONTEXT_CREATION_|VK_KHR_VIDEO_|PipelineBinar|VK_EXT_ROBUSTNESS_2|VK_KHR_PRESENT_(ID|WAIT)|VK_NV_LOW_LATENCY_2|get_existing_create_info|QUEUE_INDEX_VIDEO|ShaderStage::(Mesh|Task)|conservative_raster|indirect_bindable|YcbcrConversion|ASTCDecode|Event2|ImplementationQuirks|surface_transform|SystemHandles|INTERNAL_SPEC_CONSTANTS|swapchain_layout|consume_semaphore|granite_vk_|GRANITE_VK_|ExternalHandle|supports_external|EXTERNAL_MEMORY_BIT|ClassAllocator|LegionHeap|MiniHeap|DeviceAllocationOwner|memory_aliases|RenderPassOp|RENDER_PASS_OP_|num_subpasses|subpass_index|ImageDomain::Transient|TransientAttachmentAllocator|set_stencil_|set_depth_bias|depth_bias_enable|stencil_test|VertexAttribState|VertexBindingState|vbo_block|ibo_block|request_vertex_block|request_index_block|set_opaque_state|set_separate_depth_stencil_views|get_float_view|get_integer_view|image\\.integer|attribute_mask|VULKAN_NUM_VERTEX_|COMMAND_BUFFER_DIRTY_STATIC_VERTEX_BIT|alpha_to_coverage|primitive_restart" \
	"$ROOT/pcsx2/GS/parallel-gs" "$ROOT/pcsx2/GS/Renderers/parallel-gs" "$ROOT/Makefile.common" "$ROOT/pcsx2/CMakeLists.txt"; then
	echo "FAIL: a path behind a context flag the core never passes is back in Granite"
	exit 1
fi

C89="-std=gnu89 -pedantic -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"
"$CC" $C89 -O2 -g $SAN -I"$LC/include" -c "$ROOT/pcsx2/GS/Renderers/parallel-gs/PGSLog.c" -o "$TMP/log.o"
"$CC" $C89 -O2 -g $SAN -I"$LC/include" -c "$DIR/main.c" -o "$TMP/main.o"
"$CC" -w -O2 -g -DHAVE_THREADS $SAN -I"$LC/include" -c "$LC/rthreads/rthreads.c" -o "$TMP/rthreads.o"
"$CC" $SAN $STATIC -o "$TMP/pgslog$EXE" "$TMP/main.o" "$TMP/log.o" "$TMP/rthreads.o" $LIBS
$RUN "$TMP/pgslog$EXE"
