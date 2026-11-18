#include "formed.hpp"

#include <hog/core/portable_maths.hpp>
#include <hog/formation/ice.hpp>
#include <hog/globe/habitat.hpp>

#include <algorithm>
#include <cmath>

namespace hog::globe {

flora::Habitat habitat(const Generator& generator, DVec3 direction) {
    const Formed& formed = *generator.shape().formed;
    const DVec3 up = glm::normalize(direction);
    const Formed::Here here = formed.at(up, voxel_lod(generator));
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

} // namespace hog::globe
