#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <vector>

namespace hog::formation {

// The land as running water leaves it once wear has come to balance the uplift: worked out from
// the coasts upwards along where water flows, not stepped through time. Each point's slope is
// the one at which its river wears rock away as fast as the rock rises (stream power, n = 1).
struct Rivers {
    std::vector<double> height;          // metres from the liquid's level; sea floor as the crust left it
    std::vector<std::uint32_t> receiver; // where each point's water goes next; itself in the sea
    std::vector<double> discharge;       // cubic metres of water a year passing through
    std::vector<double> wear;            // metres a year the ground is worn down
    std::vector<double> cover;           // share of the ground plants first hold, 0 to 1
    int rounds = 0;                      // times the flow was routed before it settled
};

[[nodiscard]] Rivers rivers_on(const Sphere& sphere, const Crust& crust, const Climate& climate,
                               const Body& body);

} // namespace hog::formation
