#pragma once

#include <hog/core/math.hpp>

#include <array>
#include <cstdint>

namespace hog::globe {

// Rock is broken by joints, planes in three sets across each other; between them it stands in
// blocks. Its joints run from closely jointed rock's spacing to extremely widely jointed rock's
// (ISRM's spacing classes, 0.75 m to over 6 m). The rock lies in domains `joint_domain` metres
// apart, faults between them, each with its joint sets turned its own way; in each, the widest
// set apart run right across it, twice the widest spacing apart, and each block between them is
// split again along each direction as often as the rock is more closely jointed, more so in some
// stretches than others, as rock is broken more in some places than in others.
inline constexpr double closest_joints = 0.75;
inline constexpr double widely_jointed = 6.0;
inline constexpr double widest_joints = 2.0 * widely_jointed;
inline constexpr double joint_domain = 96.0;

// One block of rock between joints, as its corners in metres along its domain's joint sets from
// the domain's middle.
struct Block {
    DVec3 low{0.0};
    DVec3 high{0.0};
    std::uint64_t id = 0;
    DVec3 origin{0.0};           // the domain's middle, metres from the planet's centre
    std::array<DVec3, 3> sets{}; // unit, square to each of its joint sets

    [[nodiscard]] DVec3 size() const noexcept { return high - low; }
    // How wide it is as weathering sees it, by how much face it shows for its volume: a cube's
    // side, a slab's nearer its thickness.
    [[nodiscard]] double breadth() const noexcept;
    // Metres from `point` inside it to its nearest joint.
    [[nodiscard]] double inset(DVec3 point) const noexcept;
};

// How broken the rock at `point`, metres from the planet's centre, is by faults: 1 in a fault's
// crushed core, falling to nothing out past its damaged rock. Faults part the crust into blocks
// `fault_blocks` metres across and blocks four times as wide, their traces wandering as the rock
// gave way, each broken over a fiftieth of its blocks' width.
inline constexpr double fault_blocks = 800.0;
[[nodiscard]] double faulted(DVec3 point, std::uint64_t seed) noexcept;

// The mean spacing of the joints in rock standing `strength`, 0 to 1: the weakest rock is closely
// jointed, the strongest widely, rock's strength being mostly how far apart its joints are.
[[nodiscard]] double joint_spacing(double strength) noexcept;

// The block of rock standing `strength` that holds `point`, metres from the planet's centre.
[[nodiscard]] Block block_at(DVec3 point, double strength, std::uint64_t seed) noexcept;

// Metres water getting in along the joints has rotted rock from them by the time the ground,
// wearing down `worn` metres a year, brings it to the top: rotting `making` metres a year there,
// slower by `falloff` a metre the deeper it lay, for `years` at most.
[[nodiscard]] double rotted_by(double making, double worn, double falloff, double years) noexcept;

// What's left of a block of rock rotted `rotted` metres in from its joints, which water gets into
// from every face (Linton's tors): a core rounded where its faces meet, or nothing of the narrowest
// blocks. With the rotted rock about it stripped no deeper than it rotted, it stands over the
// ground no higher than that.
struct Core {
    double height = 0.0;  // metres over the ground
    DVec3 rise{0.0};      // which way its top climbs along the ground, metres per metre
    double breadth = 0.0; // metres across its thinnest way
};
// The core of `block` over the ground at `ground`, metres from the planet's centre, straight up
// along `up`; one standing clear of the ground there settled onto it.
[[nodiscard]] Core core_over(const Block& block, double rotted, DVec3 ground, DVec3 up) noexcept;

} // namespace hog::globe
