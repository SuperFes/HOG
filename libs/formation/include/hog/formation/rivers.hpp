#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::formation {

// The land as running water leaves it once wear has come to balance the uplift: worked out from
// the coasts upwards along where water flows, not stepped through time. Each point's slope is
// the one at which its river wears rock away as fast as the rock rises (stream power, n = 1).
struct Rivers {
    std::vector<double> height; // metres from the liquid's level; sea floor as the crust left it
    std::vector<double> ground; // the same with hollows left unfilled: where lakes may lie
    std::vector<std::uint32_t> receiver; // where each point's water goes next; itself in the sea
    std::vector<double> discharge;       // cubic metres of water a year passing through
    std::vector<double> flood;           // the same as wear and carrying feel it, deserts' flash
                                         // floods counted in
    std::vector<double> wear;            // metres a year the ground is worn down: the uplift where
                                         // balanced, less on flats held down by the crust
    std::vector<double> cover;           // share of the ground plants first hold, 0 to 1
    int rounds = 0;                      // times the flow was routed before it settled
};

[[nodiscard]] Rivers rivers_on(const Sphere& sphere, const Crust& crust, const Climate& climate,
                               const Body& body);
// The same over rock of `strength` at each point, 0 to 1: strong rock wears slower and stands
// steeper, soft rock faster; empty is rock of middling strength everywhere.
[[nodiscard]] Rivers rivers_on(const Sphere& sphere, const Crust& crust, const Climate& climate,
                               std::span<const double> strength, const Body& body);

// The steepest land stands on the whole over a run of `length` metres, rise over run.
[[nodiscard]] double steepest_over(double length, const Body& body);

} // namespace hog::formation
