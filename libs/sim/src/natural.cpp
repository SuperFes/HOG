#include <hog/core/hash.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/tile.hpp>
#include <hog/sim/natural.hpp>

#include <numbers>

namespace hog::sim {

namespace {

constexpr int octaves = 3;
// Noise rarely strays far from zero, so it's scaled up to move points about `reach`.
constexpr double swing = 2.0;
// Reaches per wavelength of the broadest octave: wide enough that neighbours move together.
constexpr double reaches_across = 4.0;

} // namespace

double natural_reach(int level) noexcept {
    // A cube face spans a quarter turn, halved at each level.
    return 0.5 * (std::numbers::pi / 2.0) / static_cast<double>(std::uint64_t{1} << level);
}

DVec3 natural_point(DVec3 direction, double reach, std::uint64_t seed) {
    const planet::NoisePoint point = planet::noise_point(direction, 1.0 / (reaches_across * reach));
    DVec3 shift{};
    for (DVec3::length_type axis = 0; axis < 3; ++axis) {
        shift[axis] =
            planet::fixed_to_double(planet::fbm(hash_combine(seed, axis), point, octaves));
    }
    // Only sideways moves count; the rest would vanish in normalising.
    shift -= glm::dot(shift, direction) * direction;
    return glm::normalize(direction + (shift * (swing * reach)));
}

} // namespace hog::sim
