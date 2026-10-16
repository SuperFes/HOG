#pragma once

#include <chrono>
#include <cstddef>

namespace hog::net {

// How a connection is doing, as the library measures it.
struct Stats {
    std::chrono::milliseconds ping{0};
    // The fraction of packets arriving at this end and at the other, from 0 to 1; below 0 until
    // it's known.
    float delivered_here = -1.0F;
    float delivered_there = -1.0F;
    // Over the library's last interval of 5 seconds, packet headers and all.
    float bytes_in_per_second = 0.0F;
    float bytes_out_per_second = 0.0F;
    // Queued and not yet sent.
    std::size_t queued_bytes = 0;
};

} // namespace hog::net
