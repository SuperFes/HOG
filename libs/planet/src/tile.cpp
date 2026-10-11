#include <hog/core/assert.hpp>
#include <hog/planet/tile.hpp>

#include <cmath>

namespace hog::planet {

namespace {

// Leaves room for fbm to double the coordinates once per octave without overflowing.
constexpr double max_noise_coordinate = 1 << 24;

[[nodiscard]] std::int64_t to_fixed(double value) {
    HOG_ASSERT(std::abs(value) < max_noise_coordinate, "noise coordinate out of range");
    // Scaling by a power of two is exact, so llround is the only rounding step.
    return std::llround(value * static_cast<double>(fixed_one));
}

} // namespace

NoisePoint noise_point(DVec3 position, double frequency) {
    return {.x = to_fixed(position.x * frequency),
            .y = to_fixed(position.y * frequency),
            .z = to_fixed(position.z * frequency)};
}

} // namespace hog::planet
