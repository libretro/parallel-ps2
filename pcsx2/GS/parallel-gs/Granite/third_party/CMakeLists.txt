# Vulkan::Headers comes from the core's 3rdparty/vulkan-headers.
add_library(granite-volk-headers INTERFACE)
target_include_directories(granite-volk-headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/volk)
target_link_libraries(granite-volk-headers INTERFACE Vulkan::Headers)

# volk must be STATIC.
add_library(granite-volk STATIC volk/volk.c volk/volk.h)
if (WIN32)
    target_compile_definitions(granite-volk PRIVATE VK_USE_PLATFORM_WIN32_KHR)
else()
    target_link_libraries(granite-volk PRIVATE dl)
endif()
target_link_libraries(granite-volk PRIVATE granite-volk-headers)
