#include <hog/testing/world.hpp>

namespace hog::testing {

globe::Planet small_planet() {
    globe::Planet planet;
    planet.body.seed = 1;
    planet.body.radius = 12'000.0;
    planet.voxel = 0.5;
    planet.floor_depth = 500.0;
    planet.sky_height = 2'000.0;
    return planet;
}

const globe::Generator& small_world() {
    static const globe::Generator generator{small_planet()};
    return generator;
}

} // namespace hog::testing
