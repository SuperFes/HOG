#pragma once

#include <hog/core/math.hpp>

#include <cstdint>
#include <vector>

namespace hog::terrain {

// How a biome's ground looks up close, in linear light.
struct Look {
    DVec3 colour{0.5};         // what it averages to, as it looks from afar
    DVec3 accent{0.5};         // laid over it in patches
    double accent_share = 0.0; // of the ground the patches cover, 0 to 1
    double grain = 0.0;        // how far brightness strays from texel to texel, relative
    double mottle = 0.0;       // how far it strays over broad patches, relative
    std::uint64_t seed = 0;
};

// A texture that tiles seamlessly, with its mips down to a single texel.
struct MaterialImage {
    int size = 0; // texels across the largest level
    // sRGB texels with alpha, 4 bytes each, rows packed tightly from the top; level i is size >> i
    // across.
    std::vector<std::vector<std::uint8_t>> levels;
};

// `size` must be a power of two of at least 64. Each level averages the one above in linear
// light, so the last is the look's colour.
[[nodiscard]] MaterialImage material_image(const Look& look, int size);

} // namespace hog::terrain
