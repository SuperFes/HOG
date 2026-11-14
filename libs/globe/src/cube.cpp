#include <hog/globe/cube.hpp>
#include <hog/globe/maths.hpp>

#include <algorithm>
#include <cmath>

namespace hog::globe {

Cube cube_of(const Voxel& voxel, double baseline) noexcept {
    Cube cube;
    cube.solid_amount = voxel.solid_amount;
    cube.solid = cube.solid_amount > 0 ? voxel.solid : Material::Air;
    cube.grain_amount = voxel.grain_amount;
    cube.grain = cube.grain_amount > 0 ? voxel.grain : Material::Air;
    cube.liquid_amount = voxel.liquid_amount;
    cube.liquid = cube.liquid_amount > 0 ? voxel.liquid : Material::Air;
    cube.condition = voxel.condition;
    if (cube.liquid_amount > 0) {
        cube.rise = voxel.rise;
        cube.peak = voxel.peak;
    }
    const double over = kelvin(voxel.temperature) - baseline;
    cube.heat = over >= Cube::heat_step
                    ? static_cast<std::uint8_t>(std::min(std::lround(over / Cube::heat_step), 255L))
                    : 0;
    return cube;
}

Voxel voxel_of(const Cube& cube, double baseline) noexcept {
    Voxel voxel;
    voxel.solid = cube.solid;
    voxel.solid_amount = cube.solid_amount;
    voxel.grain = cube.grain;
    voxel.grain_amount = cube.grain_amount;
    voxel.liquid = cube.liquid;
    voxel.liquid_amount = cube.liquid_amount;
    voxel.condition = cube.condition;
    voxel.rise = cube.rise;
    voxel.peak = cube.peak;
    voxel.temperature = temperature_in_eighths(baseline + (cube.heat * Cube::heat_step));
    return voxel;
}

double place_temperature(const Planet& planet, const Layers& layers, Site site) {
    return place_temperature(planet, center(column_of(site, layers)),
                             layers.middle(static_cast<int>(site.layer)));
}

double place_temperature(const Planet& planet, DVec3 up, double radius) {
    const double latitude = maths::atan2(up.z, std::sqrt((up.x * up.x) + (up.y * up.y)));
    return temperature_at(planet, latitude) -
           (climate_lapse_rate(planet) * std::max(0.0, radius - planet.body.radius));
}

} // namespace hog::globe
