#include <hog/globe/cube.hpp>
#include <hog/globe/maths.hpp>

#include <algorithm>
#include <cmath>

namespace hog::globe {

namespace {

std::uint8_t level_of(std::uint16_t amount) noexcept {
    const double share = static_cast<double>(amount) / static_cast<double>(Voxel::full);
    return static_cast<std::uint8_t>(std::lround(std::clamp(share, 0.0, 1.0) * Cube::levels));
}

std::uint16_t amount_of_level(std::uint8_t level) noexcept {
    const double share = static_cast<double>(level) / static_cast<double>(Cube::levels);
    return static_cast<std::uint16_t>(std::lround(share * static_cast<double>(Voxel::full)));
}

} // namespace

Cube cube_of(const Voxel& voxel, double baseline) noexcept {
    Cube cube;
    cube.solid_level = level_of(voxel.solid_amount);
    cube.solid = cube.solid_level > 0 ? voxel.solid : Material::Air;
    cube.grain_level = level_of(voxel.grain_amount);
    cube.grain = cube.grain_level > 0 ? voxel.grain : Material::Air;
    cube.liquid_level = level_of(voxel.liquid_amount);
    cube.liquid = cube.liquid_level > 0 ? voxel.liquid : Material::Air;
    cube.condition = voxel.condition;
    const double over = kelvin(voxel.temperature) - baseline;
    cube.heat = over >= Cube::heat_step
                    ? static_cast<std::uint8_t>(std::min(std::lround(over / Cube::heat_step), 255L))
                    : 0;
    return cube;
}

Voxel voxel_of(const Cube& cube, double baseline) noexcept {
    Voxel voxel;
    voxel.solid = cube.solid;
    voxel.solid_amount = amount_of_level(cube.solid_level);
    voxel.grain = cube.grain;
    voxel.grain_amount = amount_of_level(cube.grain_level);
    voxel.liquid = cube.liquid;
    voxel.liquid_amount = amount_of_level(cube.liquid_level);
    voxel.condition = cube.condition;
    voxel.temperature = temperature_in_eighths(baseline + (cube.heat * Cube::heat_step));
    return voxel;
}

double place_temperature(const Planet& planet, const Layers& layers, Site site) {
    const DVec3 middle = middle_of(site, layers);
    const double radius = glm::length(middle);
    const double latitude =
        maths::atan2(middle.z, std::sqrt((middle.x * middle.x) + (middle.y * middle.y)));
    return temperature_at(planet, latitude) -
           (climate_lapse_rate(planet) * std::max(0.0, radius - planet.radius));
}

} // namespace hog::globe
