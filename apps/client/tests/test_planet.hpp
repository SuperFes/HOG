#pragma once

#include <hog/client/globe_view.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/sim/calendar.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace hog::client::testing {

// The development planet the client flies over, held in a coarser shell and formed less finely
// than the shipped one, so it's quick to make.
inline const globe::Planet& planet() {
    static const globe::Planet shipped = [] {
        globe::Planet planet =
            globe::load_planet(std::filesystem::path{HOG_SOURCE_DIR} / "config" / "planet.toml")
                .value();
        planet.voxel = 2.5;
        planet.floor_depth = 4'000.0;
        planet.sky_height = 10'000.0;
        planet.formed_detail = 5.0;
        return planet;
    }();
    return shipped;
}

// The voxel world the client draws in place of the planet, as --globe makes it by default, its
// ground made about the land nearest the start.
inline const GlobeWorld& globe() {
    static const GlobeWorld& made = [] -> const GlobeWorld& {
        static GlobeWorld world{planet()};
        const DVec3 land = globe::land_near(world.generator, planet::start_direction).value();
        world.volume =
            globe::generate_area(world.generator, globe::around(world.generator, land, 5));
        return world;
    }();
    return made;
}

// Level land near the start on the globe, where there's ground to walk on without sliding:
// the gentlest slope among spots some metres apart round the nearest land.
inline DVec3 on_land() {
    static const DVec3 found = [] {
        const globe::Generator& generator = globe().generator;
        const double radius = generator.planet().body.radius;
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
    return {globe::day_length(planet()), planet().body.tilt};
}

} // namespace hog::client::testing
