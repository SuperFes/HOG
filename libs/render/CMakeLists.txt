# Vulkan headers pinned rather than taken from the system, so every platform builds against the
# same API; Apache 2 / MIT.
CPMAddPackage(
    NAME VulkanHeaders
    GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
    GIT_TAG vulkan-sdk-1.4.363.0
    SYSTEM YES)

# Loads Vulkan at run time, so builds don't link against a loader and run without one; MIT.
CPMAddPackage(
    NAME volk
    GITHUB_REPOSITORY zeux/volk
    GIT_TAG 1.4.350
    SYSTEM YES
    OPTIONS "VOLK_INSTALL OFF" "VULKAN_HEADERS_INSTALL_DIR ${VulkanHeaders_SOURCE_DIR}")

# GPU memory; MIT.
CPMAddPackage(
    NAME VulkanMemoryAllocator
    GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
    GIT_TAG v3.4.0
    DOWNLOAD_ONLY YES)

# Instance, device and swapchain setup; MIT.
CPMAddPackage(
    NAME vk-bootstrap
    GITHUB_REPOSITORY charles-lunarg/vk-bootstrap
    GIT_TAG v1.4.363
    SYSTEM YES
    OPTIONS "VK_BOOTSTRAP_TEST OFF" "VK_BOOTSTRAP_INSTALL OFF")

include(HogShaders)

add_library(hog_render
    src/atmosphere.cpp
    src/barrier.cpp
    src/billboard.cpp
    src/camera.cpp
    src/cube_sphere.cpp
    src/device.cpp
    src/globe.cpp
    src/memory.cpp
    src/ocean.cpp
    src/orbit.cpp
    src/pipeline.cpp
    src/presenter.cpp
    src/readback.cpp
    src/rendering.cpp
    src/sampler.cpp
    src/sky.cpp
    src/streamed_image.cpp
    src/swapchain.cpp
    src/terrain.cpp
    src/vma.cpp
    src/vulkan.cpp)
add_library(hog::render ALIAS hog_render)

target_include_directories(hog_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(hog_render SYSTEM PUBLIC "${VulkanMemoryAllocator_SOURCE_DIR}/include")
# Every Vulkan function comes from volk, the allocator's too, rather than from a linked loader.
target_compile_definitions(hog_render PUBLIC
    VK_NO_PROTOTYPES
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
set_source_files_properties(src/vma.cpp PROPERTIES COMPILE_DEFINITIONS VMA_IMPLEMENTATION)
target_link_libraries(hog_render
    PUBLIC hog::atmosphere hog::core hog::terrain volk::volk
    PRIVATE hog::planet vk-bootstrap::vk-bootstrap)
hog_target_defaults(hog_render)
hog_add_shaders(hog_render shaders/aerial.slang shaders/air_tables.slang shaders/billboard.slang shaders/globe.slang shaders/ocean.slang shaders/sky.slang shaders/sky_view.slang
                shaders/terrain.slang)

hog_add_tests(hog_render
    tests/atmosphere_tests.cpp
    tests/barrier_tests.cpp
    tests/billboard_tests.cpp
    tests/camera_tests.cpp
    tests/cube_sphere_tests.cpp
    tests/device_tests.cpp
    tests/globe_tests.cpp
    tests/ocean_tests.cpp
    tests/orbit_tests.cpp
    tests/pipeline_tests.cpp
    tests/presenter_tests.cpp
    tests/sky_tests.cpp
    tests/streamed_image_tests.cpp
    tests/swapchain_tests.cpp
    tests/terrain_tests.cpp)
if(TARGET hog_render_tests)
    target_link_libraries(hog_render_tests PRIVATE hog::planet)
    hog_add_shaders(hog_render_tests tests/shaders/aerial_probe.slang tests/shaders/air_probe.slang tests/shaders/dispatch.slang tests/shaders/fill.slang tests/shaders/layer.slang
                    tests/shaders/triangle.slang)
    # GPU drivers are opaque to sanitizers, so those runs leave the GPU out.
    if(HOG_SANITIZE)
        target_compile_definitions(hog_render_tests PRIVATE HOG_GPU_TESTS=0)
    else()
        target_compile_definitions(hog_render_tests PRIVATE HOG_GPU_TESTS=1)
    endif()
endif()
