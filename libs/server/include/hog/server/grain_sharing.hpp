#pragma once

#include <hog/core/math.hpp>
#include <hog/flow/flight.hpp>
#include <hog/protocol/messages.hpp>

#include <cstdint>
#include <span>

namespace hog::server {

// Grain in flight, told to the players it can be seen from.
class GrainSharing {
public:
    // Metres from a player within which grain is told.
    static constexpr double reach = 300.0;

    // The grain among `parcels` within reach of `at`, the nearest protocol::most_grains of it,
    // in order of id.
    [[nodiscard]] static protocol::Grains near(std::span<const flow::Parcel> parcels,
                                               WorldPosition at, std::uint32_t tick);
};

} // namespace hog::server
