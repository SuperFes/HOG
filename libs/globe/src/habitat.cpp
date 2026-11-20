#include "formed.hpp"
#include "terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>
#include <hog/formation/ice.hpp>
#include <hog/globe/cell.hpp>
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

// The habitat at the unit direction `up` on `ground` read there.
flora::Habitat habitat_on(const Generator& generator, DVec3 up, const terms::Ground& ground) {
    const Formed& formed = *generator.shape().formed;
    const Formed::Climate climate = formed.climate_at(up, ground.height);
    const double latitude = maths::atan2(up.z, std::sqrt((up.x * up.x) + (up.y * up.y)));
    const Strata& layers = ground.layers;
    flora::Habitat made{.temperature = climate.temperature,
                        .swing = formation::seasonal_swing(latitude, false, formed.world().body),
                        .rain = climate.rain,
                        .over_drainage = std::max(0.0, ground.above_floor),
                        .flow = climate.flow,
                        .soil = layers.organic + layers.topsoil + layers.subsoil,
                        .bare = ground.bare,
                        .slope = glm::length(ground.rise),
                        .ice = layers.ice};
    if (ground.lake) {
        made.water = std::max(0.0, *ground.lake - ground.height);
    } else if (ground.river) {
        made.water = std::max(0.0, *ground.river - ground.height);
    } else if (formed.has_sea() && ground.height < 0.0) {
        made.water = -ground.height;
        made.sea = true;
    }
    return made;
}

} // namespace

flora::Habitat habitat(const Generator& generator, DVec3 direction) {
    return habitat(generator, direction, voxel_lod(generator));
}

flora::Habitat habitat(const Generator& generator, DVec3 direction, double lod) {
    const terms::Context context{.planet = &generator.planet(),
                                 .derived = &generator.derived(),
                                 .shape = &generator.shape()};
    const DVec3 up = glm::normalize(direction);
    return habitat_on(generator, up,
                      terms::ground(context, up, std::max(lod, voxel_lod(generator))));
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
    const DVec3 up = glm::normalize(direction);
    const terms::Context context{.planet = &generator.planet(),
                                 .derived = &generator.derived(),
                                 .shape = &generator.shape(),
                                 .crags = false,
                                 .cells = generator.ground_cells()};
    const int level = terms::cell_level(generator.planet(), lod);
    const terms::Ground ground =
        terms::ground(context, cell_under(up, level), terms::cell_lod(generator.planet(), level));
    return flora::woods_of(habitat_on(generator, up, ground), lean(generator, up, lod));
}

} // namespace hog::globe
