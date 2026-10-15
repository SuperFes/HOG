#pragma once

#include <hog/planet/planet_config.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/calendar.hpp>

#include <filesystem>

namespace hog::client::testing {

// The planet as the client flies over it.
inline const planet::PlanetConfig& planet() {
    static const planet::PlanetConfig config =
        planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} / "config" / "planet.toml")
            .value();
    return config;
}

inline const planet::World& world() {
    static const planet::World made{planet()};
    return made;
}

inline sim::Calendar calendar() {
    return {planet().day_length, planet().axial_tilt};
}

} // namespace hog::client::testing
