#pragma once

// Vulkan through volk, which loads it at run time: include Vulkan only through this header.
#include <hog/core/result.hpp>

#include <volk.h>

#include <string_view>

namespace hog::render {

// The error for a failed Vulkan call, naming what was being done.
[[nodiscard]] std::unexpected<Error> vulkan_error(VkResult result, std::string_view doing);

} // namespace hog::render
