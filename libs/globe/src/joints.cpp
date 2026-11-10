#include "joints.hpp"

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>
#include <hog/globe/noise.hpp>

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
constexpr int most_splits = 4; // 12 m down to 0.75 m
// How many more or fewer splits a stretch of rock between the widest joints has than the mean,
// at most, either way.
constexpr double broken_unevenly = 1.0;

double unit(std::uint64_t bits) noexcept {
    return static_cast<double>(bits >> 11U) * 0x1.0p-53;
}

// Where the widest joint `k` along `axis` stands, metres along it.
double joint(std::uint64_t seed, glm::length_t axis, std::int64_t k) noexcept {
    const double off = ((2.0 * unit(hash_values(seed, axis, k))) - 1.0) * wander;
    return (static_cast<double>(k) + off) * widest_joints;
}

// The middle of the domain lattice's cell `cell`, scattered about it.
DVec3 domain_middle(std::uint64_t seed, glm::i64vec3 cell) noexcept {
    const std::uint64_t bits = hash_values(seed, cell.x, cell.y, cell.z);
    const DVec3 scatter{unit(hash_values(bits, 0)), unit(hash_values(bits, 1)),
                        unit(hash_values(bits, 2))};
    return (DVec3{cell} + 0.25 + (0.5 * scatter)) * joint_domain;
}

// A rotation drawn evenly from all of them (Shoemake's), as the unit vectors it turns the axes to.
std::array<DVec3, 3> turned(std::uint64_t bits) noexcept {
    const double u = unit(hash_values(bits, 0));
    const double v = 2.0 * std::numbers::pi * unit(hash_values(bits, 1));
    const double w = 2.0 * std::numbers::pi * unit(hash_values(bits, 2));
    const double a = std::sqrt(1.0 - u);
    const double b = std::sqrt(u);
    const double x = a * maths::sin(v);
    const double y = a * maths::cos(v);
    const double z = b * maths::sin(w);
    const double s = b * maths::cos(w);
    return {DVec3{1.0 - (2.0 * ((y * y) + (z * z))), 2.0 * ((x * y) + (s * z)),
                  2.0 * ((x * z) - (s * y))},
            DVec3{2.0 * ((x * y) - (s * z)), 1.0 - (2.0 * ((x * x) + (z * z))),
                  2.0 * ((y * z) + (s * x))},
            DVec3{2.0 * ((x * z) + (s * y)), 2.0 * ((y * z) - (s * x)),
                  1.0 - (2.0 * ((x * x) + (y * y)))}};
}

// Metres from `point` to the nearest boundary between the blocks of a lattice `size` across, each
// about a middle scattered in its cell.
double to_boundary(DVec3 point, double size, std::uint64_t seed) noexcept {
    const glm::i64vec3 home{glm::floor(point / size)};
    DVec3 nearest{0.0};
    DVec3 second{0.0};
    double best = 1e300;
    double next = 1e300;
    for (std::int64_t dx = -1; dx <= 1; ++dx) {
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
            for (std::int64_t dz = -1; dz <= 1; ++dz) {
                const glm::i64vec3 cell = home + glm::i64vec3{dx, dy, dz};
                const std::uint64_t bits = hash_values(seed, cell.x, cell.y, cell.z);
                const DVec3 middle =
                    (DVec3{cell} + 0.15 +
                     (0.7 * DVec3{unit(hash_values(bits, 0)), unit(hash_values(bits, 1)),
                                  unit(hash_values(bits, 2))})) *
                    size;
                const DVec3 off = point - middle;
                const double apart = glm::dot(off, off);
                if (apart < best) {
                    next = best;
                    second = nearest;
                    best = apart;
                    nearest = middle;
                } else if (apart < next) {
                    next = apart;
                    second = middle;
                }
            }
        }
    }
    // To the plane halfway between the two nearest middles.
    return (next - best) / (2.0 * glm::length(second - nearest));
}

} // namespace

double faulted(DVec3 point, std::uint64_t seed) noexcept {
    // Traces wander about as far as a quarter of the smaller blocks, at every scale finer.
    constexpr Octaves wandering{.wavelength = fault_blocks / 2.0, .gain = 0.55, .finest = 8.0};
    const DVec3 wandered = point + (DVec3{fbm(point, wandering, 0.0, hash_values(seed, 0)).value,
                                          fbm(point, wandering, 0.0, hash_values(seed, 1)).value,
                                          fbm(point, wandering, 0.0, hash_values(seed, 2)).value} *
                                    (fault_blocks / 4.0));
    double broken = 0.0;
    for (const int scale : {1, 4}) {
        const double size = scale * fault_blocks;
        const double zone = size / 50.0;
        const double off = to_boundary(wandered, size, hash_values(seed, 3, scale)) / zone;
        broken = std::max(broken, off < 1.0 ? (1.0 - off) * (1.0 - off) : 0.0);
    }
    return broken;
}

double Block::inset(DVec3 point) const noexcept {
    const DVec3 offset = point - origin;
    const DVec3 local{glm::dot(offset, sets[0]), glm::dot(offset, sets[1]),
                      glm::dot(offset, sets[2])};
    const DVec3 in = glm::min(local - low, high - local);
    return std::min({in.x, in.y, in.z});
}

double Block::breadth() const noexcept {
    const DVec3 across = size();
    return 3.0 / ((1.0 / across.x) + (1.0 / across.y) + (1.0 / across.z));
}

double joint_spacing(double strength) noexcept {
    return closest_joints *
           maths::pow(widely_jointed / closest_joints, std::clamp(strength, 0.0, 1.0));
}

Block block_at(DVec3 world_point, double strength, std::uint64_t seed) noexcept {
    // The domain whose middle is nearest.
    const glm::i64vec3 home{glm::floor(world_point / joint_domain)};
    glm::i64vec3 nearest = home;
    double best = 1e300;
    for (std::int64_t dx = -1; dx <= 1; ++dx) {
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
            for (std::int64_t dz = -1; dz <= 1; ++dz) {
                const glm::i64vec3 cell = home + glm::i64vec3{dx, dy, dz};
                const DVec3 off = world_point - domain_middle(seed, cell);
                if (const double apart = glm::dot(off, off); apart < best) {
                    best = apart;
                    nearest = cell;
                }
            }
        }
    }
    Block block;
    block.origin = domain_middle(seed, nearest);
    const std::uint64_t domain = hash_values(seed, nearest.x, nearest.y, nearest.z, 1);
    block.sets = turned(domain);
    const DVec3 offset = world_point - block.origin;
    const DVec3 point{glm::dot(offset, block.sets[0]), glm::dot(offset, block.sets[1]),
                      glm::dot(offset, block.sets[2])};
    std::uint64_t id = domain;
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        auto k = static_cast<std::int64_t>(std::floor(point[axis] / widest_joints));
        if (point[axis] < joint(domain, axis, k)) {
            --k;
        } else if (point[axis] >= joint(domain, axis, k + 1)) {
            ++k;
        }
        block.low[axis] = joint(domain, axis, k);
        block.high[axis] = joint(domain, axis, k + 1);
        id = hash_values(id, k);
    }
    // Split as often as makes the mean spacing the rock's, each split along each direction drawn
    // apart, so blocks come as slabs and columns as well as cubes.
    const double splits =
        (maths::log(widest_joints / joint_spacing(strength)) / std::numbers::ln2) +
        (((2.0 * unit(hash_values(id))) - 1.0) * broken_unevenly);
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

double rotted_by(double making, double worn, double falloff, double years) noexcept {
    if (making <= 0.0 || years <= 0.0) {
        return 0.0;
    }
    // Rock at the top now lay worn·t deep t years ago, rotting at making·exp(-falloff·worn·t).
    const double slowing = falloff * worn;
    if (slowing * years < 1e-9) {
        return making * years;
    }
    return making / slowing * (1.0 - maths::exp(-slowing * years));
}

namespace {

// A box `inner` from its middle each way, grown `rounding` all round, as seen from `point`.
struct RoundedBox {
    DVec3 inner{0.0};
    double rounding = 0.0;

    [[nodiscard]] double distance(DVec3 point) const noexcept {
        const DVec3 out = glm::abs(point) - inner;
        return glm::length(glm::max(out, 0.0)) + std::min(std::max({out.x, out.y, out.z}), 0.0) -
               rounding;
    }
    // Unit, outward, at a point on its surface.
    [[nodiscard]] DVec3 normal(DVec3 point) const noexcept {
        const DVec3 out = glm::abs(point) - inner;
        const DVec3 past = glm::max(out, 0.0);
        const DVec3 side{point.x < 0.0 ? -1.0 : 1.0, point.y < 0.0 ? -1.0 : 1.0,
                         point.z < 0.0 ? -1.0 : 1.0};
        if (const double length = glm::length(past); length > 0.0) {
            return past / length * side;
        }
        glm::length_t axis = out.x >= out.y ? 0 : 1;
        if (out.z > out[axis]) {
            axis = 2;
        }
        DVec3 normal{0.0};
        normal[axis] = side[axis];
        return normal;
    }
};

// Where along a line a distance convex along it is least, between `low` and `high`.
template <typename Distance>
double least_along(const Distance& distance, double low, double high) noexcept {
    constexpr double golden = 0.6180339887498949;
    for (int step = 0; step < 64 && high - low > 1e-6; ++step) {
        const double left = high - (golden * (high - low));
        const double right = low + (golden * (high - low));
        if (distance(left) < distance(right)) {
            high = right;
        } else {
            low = left;
        }
    }
    return 0.5 * (low + high);
}

// Where along a line a distance crosses nothing, between a point inside and one outside.
template <typename Distance>
double crossing(const Distance& distance, double inside, double outside) noexcept {
    for (int step = 0; step < 64 && std::abs(outside - inside) > 1e-6; ++step) {
        const double middle = 0.5 * (inside + outside);
        (distance(middle) < 0.0 ? inside : outside) = middle;
    }
    return 0.5 * (inside + outside);
}

} // namespace

Core core_over(const Block& block, double rotted, DVec3 ground, DVec3 up) noexcept {
    const DVec3 half = block.size() / 2.0;
    const double thinnest = std::min({half.x, half.y, half.z});
    if (rotted <= 0.0 || rotted >= thinnest) {
        return {};
    }
    // Corners rot from three faces at once and edges from two, so the core comes out rounded.
    const double rounding = std::min(rotted, thinnest - rotted);
    const RoundedBox core{.inner = half - rotted - rounding, .rounding = rounding};
    const auto local = [&](DVec3 along) {
        return DVec3{glm::dot(along, block.sets[0]), glm::dot(along, block.sets[1]),
                     glm::dot(along, block.sets[2])};
    };
    const DVec3 from = local(ground - block.origin) - ((block.low + block.high) / 2.0);
    const DVec3 upward = local(up);
    const auto distance = [&](double t) { return core.distance(from + (upward * t)); };
    const double reach = glm::length(block.size()) + glm::length(from);
    const double nearest = least_along(distance, -reach, reach);
    if (distance(nearest) >= 0.0) {
        return {};
    }
    const double top = crossing(distance, nearest, reach);
    if (top <= 0.0) {
        return {};
    }
    // Which way a crossing climbs along the ground, from the surface's normal there.
    const auto climbing = [&](double t) {
        const DVec3 normal = core.normal(from + (upward * t));
        const DVec3 world =
            (normal.x * block.sets[0]) + (normal.y * block.sets[1]) + (normal.z * block.sets[2]);
        const double facing = glm::dot(world, up);
        // Its sides stand sheer where they turn straight down.
        constexpr double sheer = 1e-3;
        return -(world - (facing * up)) / std::copysign(std::max(std::abs(facing), sheer), facing);
    };
    const double bottom = crossing(distance, nearest, -reach);
    // Standing clear of the ground, the rotted rock under it gone, it settles onto it.
    const bool settled = bottom > 0.0;
    double height = settled ? top - bottom : top;
    DVec3 rise = settled ? climbing(top) - climbing(bottom) : climbing(top);
    // Stripped no deeper than it rotted, merged smoothly into that.
    const double blend = 0.25 * rotted;
    if (const double near = blend - std::abs(height - rotted); near > 0.0) {
        const double share = near / blend;
        const bool lower = height < rotted;
        rise *= lower ? 1.0 - (0.5 * share) : 0.5 * share;
        height = std::min(height, rotted) - (share * share * blend / 4.0);
    } else if (height > rotted) {
        height = rotted;
        rise = DVec3{0.0};
    }
    return {.height = height, .rise = rise, .breadth = 2.0 * (thinnest - rotted)};
}

} // namespace hog::globe
