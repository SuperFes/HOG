#pragma once

#include <hog/mapgen/render.hpp>

#include <cstddef>
#include <vector>

namespace hog::viewer {

// An RGB image as opaque RGBA texels, for the GPU, which rarely samples 3-byte formats.
[[nodiscard]] std::vector<std::byte> rgba_texels(const mapgen::Image& image);

} // namespace hog::viewer
