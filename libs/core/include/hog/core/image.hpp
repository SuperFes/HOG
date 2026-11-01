#pragma once

#include <hog/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hog {

// A picture as written to disk: RGB, a byte each, row by row from the top.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

[[nodiscard]] Result<void> write_png(const std::filesystem::path& path, const Image& image);

} // namespace hog
