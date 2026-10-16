#include <hog/core/assert.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>

namespace hog::planet {

namespace {

// Leaves room for fbm to double the coordinates once per octave without overflowing.
template <int Bits>
constexpr double max_noise_coordinate =
    static_cast<double>(std::int64_t{1} << std::min(24, 62 - (max_octaves - 1) - Bits));

template <int Bits>
[[nodiscard]] std::int64_t to_fixed(double value) {
    HOG_ASSERT(std::abs(value) < max_noise_coordinate<Bits>, "noise coordinate out of range");
    // Scaling by a power of two is exact, so llround is the only rounding step.
    return std::llround(value * static_cast<double>(fixed_unit<Bits>));
}

} // namespace

template <int Bits>
BasicNoisePoint<Bits> noise_point(DVec3 position, double frequency) {
    return {.x = to_fixed<Bits>(position.x * frequency),
            .y = to_fixed<Bits>(position.y * frequency),
            .z = to_fixed<Bits>(position.z * frequency)};
}

template NoisePoint noise_point<fixed_fraction_bits>(DVec3 position, double frequency);
template BasicNoisePoint<fine_fraction_bits> noise_point<fine_fraction_bits>(DVec3 position,
                                                                             double frequency);

} // namespace hog::planet
