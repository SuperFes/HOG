#pragma once

#include <hog/core/math.hpp>

#include <optional>
#include <random>

namespace hog::bot {

struct WanderSettings {
    double furthest = 150.0;  // metres along the ground to a fresh target
    double nearest = 20.0;    // no point going somewhere it's already arrived
    double near_enough = 2.0; // metres that count as arrived
    double patience = 60.0;   // seconds before somewhere else looks better
};

// Where a bot is headed: a direction from the planet's centre it walks under, and how long it
// keeps trying before it picks elsewhere, in case the ground won't let it through.
struct Wander {
    DVec3 target{1.0, 0.0, 0.0}; // unit
    double patience = 0.0;       // seconds left
};

// A fresh target, `nearest` to `furthest` metres along the ground from under `at` (unit), any
// way round.
[[nodiscard]] Wander pick(DVec3 at, double planet_radius, std::mt19937_64& random,
                          const WanderSettings& settings = {});

// A frame of walking: which way along the ground, or nothing once it's arrived or has lost its
// patience, when a fresh target is wanted.
struct Walked {
    Wander wander;
    std::optional<DVec3> heading; // unit, along the ground under the feet
};

[[nodiscard]] Walked walk(Wander wander, WorldPosition feet, double planet_radius, double seconds,
                          const WanderSettings& settings = {});

} // namespace hog::bot
