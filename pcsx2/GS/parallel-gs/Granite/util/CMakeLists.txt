# Vendored libretro subset: the string_helpers / thread_priority / cli_parser /
# dynamic_library / message_queue TUs are excluded (their consumers are not
# built in the libretro core; verified via nm). Header-only utilities that the
# compiled TUs include are retained.
add_granite_internal_lib(granite-util
        logging.cpp logging.hpp
        aligned_alloc.cpp aligned_alloc.hpp
        bitops.hpp
        enum_cast.hpp
        hash.hpp
        intrusive.hpp
        intrusive_list.hpp
        object_pool.hpp
        slot_pool.hpp
        deferred_list.hpp
        owned.hpp
        stack_allocator.hpp
        temporary_hashmap.hpp
        unstable_remove_if.hpp
        intrusive_hash_map.hpp
        timer.cpp timer.hpp
        small_vector.hpp
        thread_id.cpp thread_id.hpp
        thread_name.cpp thread_name.hpp
        dynamic_array.hpp
        arena_allocator.cpp arena_allocator.hpp
        environment.cpp environment.hpp
        slab_allocator.cpp slab_allocator.hpp
        thread_prims.hpp
       )
target_include_directories(granite-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# thread_prims.hpp is Granite's threading on rthreads rather than <thread>,
# so libretro-common's headers have to be reachable. PUBLIC because
# granite-vulkan links granite-util and parallel-gs links granite-vulkan, and
# both include thread_prims.hpp. The static Makefile already has
# -I$(LIBRETRO_COMM_DIR)/include globally, so only CMake needed telling.
target_include_directories(granite-util PUBLIC
        ${CMAKE_SOURCE_DIR}/libretro/libretro-common/include)
if (NOT WIN32)
    target_link_libraries(granite-util PUBLIC dl)
    # bionic has no librt (clock_*/timer_* live in libc); linking it
    # unconditionally fails the NDK lanes at final link with
    # "unable to find library -lrt".
    if (NOT APPLE AND NOT ANDROID)
        target_link_libraries(granite-util PUBLIC rt)
    endif()
endif()
