#pragma once

#include <hog/core/math.hpp>

#include <cstdint>

namespace hog::globe {

// Rock is broken by joints, planes in three sets across each other; between them it stands in
// blocks. The widest set apart run the length of the planet along fixed directions, as far apart
// as extremely widely jointed rock is (ISRM's spacing classes, over 6 m); each block between them
// is split again along each direction as often as the rock is more closely jointed, down to
// closely jointed rock's spacing.
inline constexpr double widest_joints = 6.0;
inline constexpr double closest_joints = 0.75;

// One block of rock between joints, as its corners along the fixed directions, in metres.
struct Block {
    DVec3 low{0.0};
    DVec3 high{0.0};
    std::uint64_t id = 0;

    [[nodiscard]] DVec3 size() const noexcept { return high - low; }
    // Metres from `point` inside it to its nearest joint.
    [[nodiscard]] double inset(DVec3 point) const noexcept;
};

// The mean spacing of the joints in rock standing `strength`, 0 to 1: the weakest rock is closely
// jointed, the strongest widely, rock's strength being mostly how far apart its joints are.
[[nodiscard]] double joint_spacing(double strength) noexcept;

// The block of rock standing `strength` that holds `point`, metres from the planet's centre.
[[nodiscard]] Block block_at(DVec3 point, double strength, std::uint64_t seed) noexcept;

} // namespace hog::globe
