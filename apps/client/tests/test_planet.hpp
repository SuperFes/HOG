#pragma once

#include <hog/planet/places.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/testing/worlds.hpp>

#include <filesystem>
#include <functional>

namespace hog::client::testing {

// The planet as the client flies over it, worn coarsely.
inline const planet::PlanetConfig& planet() {
    static const planet::PlanetConfig config = [] {
        planet::PlanetConfig loaded =
            planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} / "config" /
                                       "planet.toml")
                .value();
        loaded.erosion_cell = ::hog::testing::coarse_erosion_cell;
        return loaded;
    }();
    return config;
}

inline const planet::World& world() {
    static const planet::World made{planet()};
    return made;
}

// The middle of the cell some 700 m across nearest the start where `fits`, wherever the seed put
// it.
inline DVec3 nearest_place(const std::function<bool(const planet::CellState&)>& fits) {
    return planet::cell_center(
        planet::nearest_cell(planet::start_direction, 8, [&](planet::CellId cell) {
            return fits(world().state(cell));
        }).value());
}

inline DVec3 nearest_biome(planet::Biome biome) {
    return nearest_place([&](const planet::CellState& state) { return state.biome == biome; });
}

inline sim::Calendar calendar() {
    return {planet().day_length, planet().axial_tilt};
}

} // namespace hog::client::testing
