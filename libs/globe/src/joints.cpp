#include "joints.hpp"

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace hog::globe {

namespace {

// The widest joints stand off their even spacing by up to this share of it either way, so no
// two cross.
constexpr double wander = 0.3;
// A block is split no nearer its sides than this share of its width.
constexpr double split_from = 0.35;
// Splits from the widest spacing down to the closest.
constexpr int most_splits = 3; // 6 m down to 0.75 m

double unit(std::uint64_t bits) noexcept {
    return static_cast<double>(bits >> 11U) * 0x1.0p-53;
}

// Where the widest joint `k` along `axis` stands, metres along it.
double joint(std::uint64_t seed, glm::length_t axis, std::int64_t k) noexcept {
    const double off = ((2.0 * unit(hash_values(seed, axis, k))) - 1.0) * wander;
    return (static_cast<double>(k) + off) * widest_joints;
}

} // namespace

double Block::inset(DVec3 point) const noexcept {
    const DVec3 in = glm::min(point - low, high - point);
    return std::min({in.x, in.y, in.z});
}

double joint_spacing(double strength) noexcept {
    return closest_joints *
           maths::pow(widest_joints / closest_joints, std::clamp(strength, 0.0, 1.0));
}

Block block_at(DVec3 point, double strength, std::uint64_t seed) noexcept {
    Block block;
    std::uint64_t id = seed;
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        auto k = static_cast<std::int64_t>(std::floor(point[axis] / widest_joints));
        if (point[axis] < joint(seed, axis, k)) {
            --k;
        } else if (point[axis] >= joint(seed, axis, k + 1)) {
            ++k;
        }
        block.low[axis] = joint(seed, axis, k);
        block.high[axis] = joint(seed, axis, k + 1);
        id = hash_values(id, k);
    }
    // Split as often as makes the mean spacing the rock's, each split along each direction drawn
    // apart, so blocks come as slabs and columns as well as cubes.
    const double splits = maths::log(widest_joints / joint_spacing(strength)) / std::numbers::ln2;
    for (int level = 0; level < most_splits; ++level) {
        const double chance = std::clamp(splits - level, 0.0, 1.0);
        const std::uint64_t at_level = id;
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            const std::uint64_t bits = hash_values(at_level, level, axis);
            if (unit(bits) >= chance) {
                continue;
            }
            const double share =
                split_from + ((1.0 - (2.0 * split_from)) * unit(hash_values(bits)));
            const double middle = block.low[axis] + (share * (block.high[axis] - block.low[axis]));
            const bool upper = point[axis] >= middle;
            (upper ? block.low : block.high)[axis] = middle;
            id = hash_values(id, axis, upper);
        }
    }
    block.id = id;
    return block;
}

} // namespace hog::globe
