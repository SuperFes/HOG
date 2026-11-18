#include "formed.hpp"

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>
#include <hog/formation/ice.hpp>
#include <hog/globe/habitat.hpp>
#include <hog/globe/noise.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hog::globe {
namespace {

constexpr std::uint64_t stands_seed = 0x7374'616e'6473ULL;
// Kinds wander into groves a few hundred metres across, outliers rarer and further apart, and
// glades open smaller still.
constexpr Octaves wandering{.wavelength = 700.0, .gain = 0.5, .finest = 60.0};
constexpr double wander_gain = 2.0;
constexpr Octaves outlying{.wavelength = 1'500.0, .gain = 0.5, .finest = 100.0};
constexpr Octaves glades{.wavelength = 350.0, .gain = 0.5, .finest = 40.0};

double rise(double value, double from, double to) noexcept {
    const double t = std::clamp((value - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

} // namespace

flora::Habitat habitat(const Generator& generator, DVec3 direction) {
    return habitat(generator, direction, voxel_lod(generator));
}

flora::Habitat habitat(const Generator& generator, DVec3 direction, double lod) {
    const Formed& formed = *generator.shape().formed;
    const DVec3 up = glm::normalize(direction);
    const Formed::Here here = formed.at(up, std::max(lod, voxel_lod(generator)));
    const double ground = here.top.value;
    const Formed::Climate climate = formed.climate_at(up, ground);
    const double latitude = maths::atan2(up.z, std::sqrt((up.x * up.x) + (up.y * up.y)));
    const Strata& layers = here.layers;
    flora::Habitat made{.temperature = climate.temperature,
                        .swing = formation::seasonal_swing(latitude, false, formed.world().body),
                        .rain = climate.rain,
                        .over_drainage = std::max(0.0, here.above_floor),
                        .flow = climate.flow,
                        .soil = layers.organic + layers.topsoil + layers.subsoil,
                        .bare = here.weathering.bare,
                        .slope = glm::length(here.top.gradient),
                        .ice = layers.ice};
    if (here.lake) {
        made.water = std::max(0.0, *here.lake - ground);
    } else if (here.river) {
        made.water = std::max(0.0, here.river->level - ground);
    } else if (formed.has_sea() && ground < 0.0) {
        made.water = -ground;
        made.sea = true;
    }
    return made;
}

flora::Lean lean(const Generator& generator, DVec3 direction, double lod) {
    const DVec3 metres = glm::normalize(direction) * generator.planet().body.radius;
    const std::uint64_t seed = hash_values(generator.planet().body.seed, stands_seed);
    flora::Lean leaning;
    for (std::size_t kind = 0; kind < flora::kinds; ++kind) {
        leaning.wander.at(kind) =
            wander_gain * fbm(metres, wandering, lod, hash_values(seed, 1, kind)).value;
        leaning.outlier.at(kind) =
            rise(fbm(metres, outlying, lod, hash_values(seed, 2, kind)).value, 0.35, 0.55);
    }
    leaning.glade = rise(fbm(metres, glades, lod, hash_values(seed, 3)).value, 0.1, 0.45);
    return leaning;
}

flora::Woods woods(const Generator& generator, DVec3 direction, double lod) {
    return flora::woods_of(habitat(generator, direction, lod), lean(generator, direction, lod));
}

} // namespace hog::globe
