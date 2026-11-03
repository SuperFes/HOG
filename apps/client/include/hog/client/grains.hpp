#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/render/capsules.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hog::client {

// The grain the server has in flight near the player. Each lump is carried on under gravity from
// where it was last told, so it travels between messages instead of jumping from one to the next.
class GrainField {
public:
    // The longest a lump is carried on without word of it, in seconds.
    static constexpr double longest_carry = 0.3;

    // Takes in what the server says is flying at `now`. Word older than what's held is ignored,
    // as unreliable messages may arrive out of order.
    void receive(const protocol::Grains& grains, MonotonicClock::time_point now);

    [[nodiscard]] std::size_t size() const noexcept { return flying_.size(); }

    // The lumps as spheres of the grain's own colour at `now`. A voxel is `thickness` metres
    // deep, and `gravity` pulls along the radius.
    [[nodiscard]] std::vector<render::Capsule> capsules(MonotonicClock::time_point now,
                                                        double thickness, double gravity) const;

private:
    std::vector<protocol::Flying> flying_;
    std::uint32_t tick_ = 0;
    bool any_ = false;
    MonotonicClock::time_point told_;
};

} // namespace hog::client
