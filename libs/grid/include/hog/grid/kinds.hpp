#pragma once

#include <hog/core/bytes.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace hog::grid {

// The ground's surface through a node, as a plane: which way it faces out of the solid, how far
// out from it the node's middle lies, in node sides, and the material it shows, as a palette
// index, 0 where unknown. A node the plane misses is all solid or all open, and holds no plane.
class Surface {
public:
    // Distances are held in these parts of a side.
    static constexpr double scale = 0x1p24;

    [[nodiscard]] static constexpr Surface solid() noexcept {
        return Surface{std::numeric_limits<std::int32_t>::min(), 0, 0, 0};
    }
    [[nodiscard]] static constexpr Surface open() noexcept {
        return Surface{std::numeric_limits<std::int32_t>::max(), 0, 0, 0};
    }
    // Through a node: all solid or all open where the plane misses it. Precondition: `normal`
    // a unit vector.
    [[nodiscard]] static Surface plane(DVec3 normal, double distance,
                                       std::uint16_t material = 0) noexcept;

    constexpr Surface() noexcept = default;

    [[nodiscard]] constexpr bool is_solid() const noexcept { return *this == solid(); }
    [[nodiscard]] constexpr bool is_open() const noexcept { return *this == open(); }
    [[nodiscard]] constexpr bool crosses() const noexcept { return !is_solid() && !is_open(); }
    // Of a node it crosses.
    [[nodiscard]] DVec3 normal() const noexcept;
    [[nodiscard]] double distance() const noexcept { return distance_ / scale; }
    [[nodiscard]] constexpr std::uint16_t material() const noexcept { return material_; }

    // The raw parts, for storing.
    [[nodiscard]] constexpr std::int32_t raw_distance() const noexcept { return distance_; }
    [[nodiscard]] constexpr std::uint16_t raw_u() const noexcept { return u_; }
    [[nodiscard]] constexpr std::uint16_t raw_v() const noexcept { return v_; }
    // Fails on a node all solid or open whose normal or material isn't unset.
    [[nodiscard]] static Result<Surface> from_raw(std::int32_t distance, std::uint16_t u,
                                                  std::uint16_t v, std::uint16_t material);

    friend constexpr bool operator==(const Surface&, const Surface&) noexcept = default;

private:
    constexpr Surface(std::int32_t distance, std::uint16_t u, std::uint16_t v,
                      std::uint16_t material) noexcept
        : distance_{distance}, u_{u}, v_{v}, material_{material} {}

    std::int32_t distance_ = std::numeric_limits<std::int32_t>::max();
    // The normal folded onto an octahedron and flattened.
    std::uint16_t u_ = 0;
    std::uint16_t v_ = 0;
    std::uint16_t material_ = 0;
};

// Each kind of data a tree holds says how eight nodes sum up into the one over them, when one
// value stands for all eight so they needn't be split, and how its values are stored.

// Where the ground is and what it shows. A coarse node holds the plane through its eight, showing
// the material most of those crossed show, so it looks as they do from afar; eight merge where
// one plane runs within `tolerance` finest nodes of theirs, so flat ground stays coarse.
struct Shape {
    using Value = Surface;
    static constexpr double tolerance = 0.25;

    [[nodiscard]] static Value summary(const std::array<Value, 8>& children) noexcept;
    // Of eight each `side` finest nodes across.
    [[nodiscard]] static std::optional<Value> merged(const std::array<Value, 8>& children,
                                                     double side) noexcept;
    [[nodiscard]] static std::uint64_t hash(const Value& value) noexcept;
    static void write(ByteWriter& out, const Value& value);
    [[nodiscard]] static Result<Value> read(ByteReader& in);
};

// Values that are one thing or another, never between, 0 for none: a coarse node holds the most
// common of its eight but none, the lowest of those tied, so it shows what's in it; none only
// where all are. Eight merge only when alike.
template <std::unsigned_integral Integer>
struct Discrete {
    using Value = Integer;

    [[nodiscard]] static Value summary(const std::array<Value, 8>& children) noexcept {
        Value best = 0;
        int best_count = 0;
        for (const Value candidate : children) {
            if (candidate == 0) {
                continue;
            }
            int count = 0;
            for (const Value other : children) {
                count += other == candidate ? 1 : 0;
            }
            if (count > best_count || (count == best_count && candidate < best)) {
                best = candidate;
                best_count = count;
            }
        }
        return best;
    }
    [[nodiscard]] static std::optional<Value> merged(const std::array<Value, 8>& children,
                                                     double /*side*/) noexcept {
        for (const Value child : children) {
            if (child != children[0]) {
                return std::nullopt;
            }
        }
        return children[0];
    }
    [[nodiscard]] static std::uint64_t hash(Value value) noexcept { return value; }
    static void write(ByteWriter& out, Value value) { out.write(value); }
    [[nodiscard]] static Result<Value> read(ByteReader& in) { return in.read<Value>(); }
};

// An index into the world's palette of materials, each a named mix of what it's made of (a loam
// of sand, silt and clay, say), so one value says what digging there gives.
struct Material : Discrete<std::uint16_t> {};

// The water body a node's basin belongs to, 0 for none.
struct Water : Discrete<std::uint32_t> {};

} // namespace hog::grid
