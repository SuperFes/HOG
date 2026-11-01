#pragma once

#include <hog/client/globe_view.hpp>
#include <hog/globe/generator.hpp>
#include <hog/planet/places.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/testing/worlds.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>

namespace hog::client::testing {

// The planet as the client flies over it, worn coarsely, from the generator these tests' places
// were found on whichever the config picks.
inline const planet::PlanetConfig& planet() {
    static const planet::PlanetConfig config = [] {
        planet::PlanetConfig loaded =
            planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} / "config" /
                                       "planet.toml")
                .value();
        loaded.erosion_cell = ::hog::testing::coarse_erosion_cell;
        loaded.generator = planet::Generator::Stamped;
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

// The voxel world the client draws in place of the planet, as --globe makes it by default.
inline const GlobeWorld& globe() {
    static const GlobeWorld made{
        globe_planet(std::nullopt, planet().seed, planet().radius).value()};
    return made;
}

// Level land near the start on the globe, where there's ground to walk on without sliding:
// the gentlest slope among spots some metres apart round the nearest land.
inline DVec3 on_land() {
    static const DVec3 found = [] {
        const globe::Generator& generator = globe().generator;
        const double radius = generator.planet().radius;
        const double lod = globe::voxel_lod(generator);
        const DVec3 near = globe::land_near(generator, planet::start_direction).value();
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, near));
        const DVec3 north = glm::cross(near, east);
        const auto height = [&](DVec3 at) {
            return globe::ground_below(generator, at, lod).value_or(0.0);
        };
        DVec3 best = near;
        double gentlest = 1e9;
        for (int y = -8; y <= 8; ++y) {
            for (int x = -8; x <= 8; ++x) {
                const DVec3 at = glm::normalize(near + (east * (x * 25.0 / radius)) +
                                                (north * (y * 25.0 / radius)));
                const double middle = height(at);
                if (middle <= generator.sea_level().value_or(0.0) + 2.0) {
                    continue;
                }
                double slope = 0.0;
                for (const DVec3 along : {east, north}) {
                    const DVec3 beside = glm::normalize(at + (along * (2.0 / radius)));
                    slope = std::max(slope, std::abs(height(beside) - middle) / 2.0);
                }
                if (slope < gentlest) {
                    gentlest = slope;
                    best = at;
                }
            }
        }
        return best;
    }();
    return found;
}

inline sim::Calendar calendar() {
    return {planet().day_length, planet().axial_tilt};
}

} // namespace hog::client::testing
