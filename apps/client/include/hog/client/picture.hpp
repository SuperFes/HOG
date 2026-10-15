#pragma once

#include <hog/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace hog::client {

// sRGB texels with alpha, 4 bytes each, rows packed tightly from the top.
struct Picture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> texels;
};

[[nodiscard]] Result<Picture> load_picture(const std::filesystem::path& path);
// As a PNG, without its alpha.
[[nodiscard]] Result<void> save_picture(const std::filesystem::path& path, const Picture& picture);

// A drawing in ink on white paper, with the paper made see-through: each texel takes the least
// opacity that keeps its ink a colour, so white goes clear and blends of ink and white go back to
// the ink.
void lift_ink(Picture& picture);

} // namespace hog::client
