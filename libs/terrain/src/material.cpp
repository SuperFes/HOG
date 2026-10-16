#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/terrain/material.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <vector>

namespace hog::terrain {

namespace {

// Broad patches span a quarter of the tile; grain reaches down to single texels.
constexpr int accent_cells = 2;
constexpr int mottle_cells = 4;
constexpr int grain_cells = 32;
// Of the accent field's range, over which a patch's edge fades in.
constexpr double accent_edge = 0.05;

double fade(double t) {
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

double lattice(std::uint64_t seed, int cells, int x, int y) {
    const auto wrapped = [cells](int at) { return ((at % cells) + cells) % cells; };
    const std::uint64_t hash = hash_values(seed, cells, wrapped(x), wrapped(y));
    return (static_cast<double>(hash >> 11) * 0x1.0p-53 * 2.0) - 1.0;
}

// Value noise from -1 to 1 repeating over the tile, `cells` lattice cells across it, at `u` and
// `v` from 0 to 1 across the tile.
double tiled_noise(std::uint64_t seed, int cells, double u, double v) {
    const double x = u * cells;
    const double y = v * cells;
    const int i = static_cast<int>(std::floor(x));
    const int j = static_cast<int>(std::floor(y));
    const double s = fade(x - i);
    const double t = fade(y - j);
    const double low = std::lerp(lattice(seed, cells, i, j), lattice(seed, cells, i + 1, j), s);
    const double high =
        std::lerp(lattice(seed, cells, i, j + 1), lattice(seed, cells, i + 1, j + 1), s);
    return std::lerp(low, high, t);
}

// Octaves from `cells` across up to one per texel, each half as strong, scaled back to -1 to 1.
double tiled_fbm(std::uint64_t seed, int cells, int size, double u, double v) {
    double sum = 0.0;
    double strength = 1.0;
    double total = 0.0;
    for (int octave = cells; octave <= size; octave *= 2) {
        sum += strength * tiled_noise(hash_values(seed, octave), octave, u, v);
        total += strength;
        strength /= 2.0;
    }
    return sum / total;
}

using Level = std::vector<DVec3>;

std::uint8_t encode(double linear) {
    const double clamped = std::clamp(linear, 0.0, 1.0);
    const double encoded =
        clamped <= 0.0031308 ? clamped * 12.92 : (1.055 * std::pow(clamped, 1.0 / 2.4)) - 0.055;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0));
}

double decode(std::uint8_t byte) {
    const double encoded = byte / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

std::vector<std::uint8_t> encoded(const Level& level) {
    std::vector<std::uint8_t> texels;
    texels.reserve(level.size() * 4);
    for (const DVec3& texel : level) {
        texels.insert(texels.end(), {encode(texel.r), encode(texel.g), encode(texel.b), 255});
    }
    return texels;
}

Level halved(const Level& level, int size) {
    const int half = size / 2;
    Level smaller(static_cast<std::size_t>(half) * static_cast<std::size_t>(half));
    const auto at = [&](int x, int y) {
        return level[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) +
                     static_cast<std::size_t>(x)];
    };
    for (int y = 0; y < half; ++y) {
        for (int x = 0; x < half; ++x) {
            smaller[(static_cast<std::size_t>(y) * static_cast<std::size_t>(half)) +
                    static_cast<std::size_t>(x)] =
                (at(2 * x, 2 * y) + at((2 * x) + 1, 2 * y) + at(2 * x, (2 * y) + 1) +
                 at((2 * x) + 1, (2 * y) + 1)) /
                4.0;
        }
    }
    return smaller;
}

} // namespace

MaterialImage material_image(const Look& look, int size) {
    HOG_ASSERT(size >= 64 && std::has_single_bit(static_cast<unsigned>(size)),
               "a material's size must be a power of two of at least 64");
    const std::size_t count = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    // Texel centres across the tile, from 0 to 1.
    std::vector<double> centres(static_cast<std::size_t>(size));
    for (std::size_t at = 0; at < centres.size(); ++at) {
        centres[at] = (static_cast<double>(at) + 0.5) / size;
    }
    const auto u_of = [&](std::size_t texel) { return centres[texel % centres.size()]; };
    const auto v_of = [&](std::size_t texel) { return centres[texel / centres.size()]; };

    // The accent covers its share exactly: its edge lies at that quantile of the field.
    const double share = std::clamp(look.accent_share, 0.0, 1.0);
    std::vector<double> accent(count, 0.0);
    if (share > 0.0) {
        const std::uint64_t seed = hash_values(look.seed, 1);
        for (std::size_t texel = 0; texel < count; ++texel) {
            accent[texel] =
                tiled_fbm(seed, accent_cells, mottle_cells * 4, u_of(texel), v_of(texel));
        }
        std::vector<double> sorted = accent;
        const auto edge = static_cast<std::ptrdiff_t>(
            std::min(static_cast<double>(count - 1), (1.0 - share) * static_cast<double>(count)));
        std::ranges::nth_element(sorted, sorted.begin() + edge);
        const double threshold = sorted[static_cast<std::size_t>(edge)];
        for (double& value : accent) {
            const double t =
                std::clamp((value - threshold + accent_edge) / (2.0 * accent_edge), 0.0, 1.0);
            value = share >= 1.0 ? 1.0 : t * t * (3.0 - (2.0 * t));
        }
    }

    Level level(count);
    DVec3 sum{0.0};
    for (std::size_t texel = 0; texel < count; ++texel) {
        const double u = u_of(texel);
        const double v = v_of(texel);
        const double mottle = tiled_fbm(hash_values(look.seed, 2), mottle_cells, grain_cells, u, v);
        const double grain = tiled_fbm(hash_values(look.seed, 3), grain_cells, size, u, v);
        const double bright = std::max(1.0 + (look.mottle * mottle) + (look.grain * grain), 0.0);
        level[texel] = glm::mix(look.colour, look.accent, accent[texel]) * bright;
        sum += level[texel];
    }
    const DVec3 mean = sum / static_cast<double>(count);
    const DVec3 scale{mean.r > 0.0 ? look.colour.r / mean.r : 0.0,
                      mean.g > 0.0 ? look.colour.g / mean.g : 0.0,
                      mean.b > 0.0 ? look.colour.b / mean.b : 0.0};
    for (DVec3& texel : level) {
        texel = glm::clamp(texel * scale, DVec3{0.0}, DVec3{1.0});
    }

    MaterialImage image{.size = size, .levels = {}};
    image.levels.reserve(static_cast<std::size_t>(std::countr_zero(static_cast<unsigned>(size))) +
                         1);
    for (int across = size;; across /= 2) {
        image.levels.push_back(encoded(level));
        if (across == 1) {
            break;
        }
        level = halved(level, across);
    }
    return image;
}

DVec3 material_mean(const MaterialImage& image) {
    HOG_ASSERT(!image.levels.empty() && image.levels.back().size() >= 3,
               "a material's mean is in its last level");
    const std::vector<std::uint8_t>& last = image.levels.back();
    return {decode(last[0]), decode(last[1]), decode(last[2])};
}

} // namespace hog::terrain
