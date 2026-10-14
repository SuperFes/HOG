#pragma once

#include <hog/render/device.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace hog::render::testing {

// A validated device preferring a software one, or a skipped test where there's none.
inline std::unique_ptr<Device> test_device(DeviceOptions options = {}) {
    if (HOG_GPU_TESTS == 0) {
        SKIP("GPU tests are off in this build");
    }
    options.validation = true;
    options.prefer_software = true;
    auto device = Device::create(options);
    if (!device) {
        SKIP("no Vulkan device: " << device.error().message);
    }
    if (!(*device)->validating()) {
        WARN("the Vulkan validation layer isn't installed, so it isn't checked");
    }
    return std::move(device).value();
}

} // namespace hog::render::testing
