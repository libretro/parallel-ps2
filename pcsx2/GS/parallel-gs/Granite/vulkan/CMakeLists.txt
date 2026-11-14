add_granite_internal_lib(granite-vulkan
        context.cpp context.hpp
        vulkan_headers.hpp vulkan_prerotate.hpp
        vulkan_loader.c vulkan_loader.h vulkan_loader.inl
        device.cpp device.hpp
        buffer_pool.cpp buffer_pool.hpp
        image.cpp image.hpp
        cookie.cpp cookie.hpp
        sampler.cpp sampler.hpp
        command_pool.cpp command_pool.hpp
        fence_manager.cpp fence_manager.hpp
        descriptor_set.cpp descriptor_set.hpp
        semaphore_manager.cpp semaphore_manager.hpp
        command_buffer.cpp command_buffer.hpp
        shader.cpp shader.hpp
        render_pass.cpp render_pass.hpp
        buffer.cpp buffer.hpp
        pipeline_cache.cpp pipeline_cache.hpp
        semaphore.cpp semaphore.hpp
        memory_allocator.cpp memory_allocator.hpp
        fence.hpp fence.cpp
        format.hpp
        limits.hpp
        type_to_string.hpp
        quirks.hpp
        vulkan_common.hpp
        texture/texture_format.cpp texture/texture_format.hpp)

target_include_directories(granite-vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (${CMAKE_BUILD_TYPE} MATCHES "Debug")
    target_compile_definitions(granite-vulkan PUBLIC VULKAN_DEBUG)
endif()

target_link_libraries(granite-vulkan
        PUBLIC granite-util Vulkan::Headers)
if (NOT WIN32)
    target_link_libraries(granite-vulkan PRIVATE ${CMAKE_DL_LIBS})
endif()
