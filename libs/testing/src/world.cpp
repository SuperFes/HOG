#include <hog/testing/world.hpp>

#include <format>

namespace hog::testing {

globe::Planet small_planet() {
    globe::Planet planet;
    planet.body.seed = 5;
    planet.body.radius = 12'000.0;
    planet.voxel = 0.5;
    planet.floor_depth = 500.0;
    planet.sky_height = 2'000.0;
    return planet;
}

std::string small_planet_file() {
    const globe::Planet planet = small_planet();
    return std::format("[planet]\nseed = {}\nradius = {}\nvoxel = {}\nfloor_depth = {}\n"
                       "sky_height = {}\nformed_detail = {}\n",
                       planet.body.seed, planet.body.radius, planet.voxel, planet.floor_depth,
                       planet.sky_height, planet.formed_detail);
}

const globe::Generator& small_world() {
    static const globe::Generator generator{small_planet()};
    return generator;
}

} // namespace hog::testing
