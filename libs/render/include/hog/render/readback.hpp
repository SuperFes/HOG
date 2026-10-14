#pragma once

#include <hog/core/result.hpp>
#include <hog/render/barrier.hpp>

#include <cstdint>
#include <vector>

namespace hog::render {

class Device;
class Image;

// Pixels of a 4-byte-per-pixel colour image, row by row from the top, for tests and captures.
// The image is left as a copy source.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_pixels(Device& device, const Image& image,
                                                            ImageUse use);

} // namespace hog::render
