#pragma once

#include <hog/core/math.hpp>
#include <hog/flow/simulation.hpp>
#include <hog/flow/wetting.hpp>
#include <hog/globe/world.hpp>
#include <hog/protocol/messages.hpp>

#include <span>
#include <vector>

namespace hog::server {

// Where the sky wets or dries the ground: around the players, who are the only ones to see it.
class GroundWetting {
public:
    // Columns either side of a player's, along each axis of the cube-sphere's face.
    static constexpr int reach = 24;
    // Share of what falls that has to be rain for it to wet the ground; snow lies on it instead.
    static constexpr float rainy = 0.5F;

    // What the weather over a cell does to the ground.
    [[nodiscard]] static flow::Sky sky_of(const protocol::CellWeather& weather);

    struct Place {
        DVec3 up; // unit, from the planet's centre
        flow::Sky sky;
    };
    // The columns within reach of each place, under its sky, each looked down from the highest
    // layer that may hold matter; where places overlap a column is taken once, by the first.
    // Columns on the edge of a cube face are bent in to stay on it.
    [[nodiscard]] static std::vector<flow::Simulation::Wetted>
    around(const globe::World& world, std::span<const Place> places);
};

} // namespace hog::server
