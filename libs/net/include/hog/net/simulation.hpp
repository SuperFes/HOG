#pragma once

#include <hog/core/result.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace hog::net {

// A bad network to try the game on, added to every packet this process sends and every one it
// receives, so two ends in one process both add it to what passes between them.
struct Simulation {
    std::chrono::milliseconds lag{0};
    // The mean of a random extra delay, which keeps packets in order.
    std::chrono::milliseconds jitter{0};
    int loss = 0;    // percent of packets dropped
    int reorder = 0; // percent of packets held back past later ones

    [[nodiscard]] bool off() const noexcept { return *this == Simulation{}; }

    friend bool operator==(const Simulation&, const Simulation&) = default;
};

// Applies to the whole process from now on; a default Simulation turns it off.
void simulate(const Simulation& simulation);

// Reads "lag=150,jitter=20,loss=5,reorder=1", in milliseconds and percent, any of them left out.
[[nodiscard]] Result<Simulation> parse_simulation(std::string_view text);
// As parse_simulation reads it.
[[nodiscard]] std::string describe(const Simulation& simulation);

} // namespace hog::net
