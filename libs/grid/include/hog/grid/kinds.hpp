#pragma once

#include <hog/core/bytes.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace hog::grid {

// The ground's surface through a node, as a plane: which way it faces out of the solid, how far
// out from it the node's middle lies, in node sides, the material it shows, as a palette index, 0
// where unknown, and how rough it is: how far the ground finer than the node may stray from the
// plane, in node sides, where known. A node the plane misses is all solid or all open, and holds
// no plane.
class Surface {
public:
    // Distances are held in these parts of a side.
    static constexpr double scale = 0x1p24;
    // Roughness is held as a step of a sixteenth of a doubling from `smoothest` sides, 0 for not
    // known; none rougher than the last step.
    static constexpr double smoothest = 0x1p-10;
    static constexpr int roughness_steps = 16;

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
    [[nodiscard]] std::optional<double> roughness() const noexcept;
    // The same with `roughness` in node sides, rounded to a step; a node it misses holds none.
    [[nodiscard]] Surface with_roughness(std::optional<double> roughness) const noexcept;

    // The raw parts, for storing.
    [[nodiscard]] constexpr std::int32_t raw_distance() const noexcept { return distance_; }
    [[nodiscard]] constexpr std::uint16_t raw_u() const noexcept { return u_; }
    [[nodiscard]] constexpr std::uint16_t raw_v() const noexcept { return v_; }
    [[nodiscard]] constexpr std::uint8_t raw_roughness() const noexcept { return roughness_; }
    // Fails on a node all solid or open whose normal, material or roughness isn't unset.
    [[nodiscard]] static Result<Surface> from_raw(std::int32_t distance, std::uint16_t u,
                                                  std::uint16_t v, std::uint16_t material,
                                                  std::uint8_t roughness = 0);

    friend constexpr bool operator==(const Surface&, const Surface&) noexcept = default;

private:
    constexpr Surface(std::int32_t distance, std::uint16_t u, std::uint16_t v,
                      std::uint16_t material, std::uint8_t roughness = 0) noexcept
        : distance_{distance}, u_{u}, v_{v}, material_{material}, roughness_{roughness} {}

    std::int32_t distance_ = std::numeric_limits<std::int32_t>::max();
    // The normal folded onto an octahedron and flattened.
    std::uint16_t u_ = 0;
    std::uint16_t v_ = 0;
    std::uint16_t material_ = 0;
    std::uint8_t roughness_ = 0;
};

// Two of the palette's first `blendable` entries showing together, the first over at least half
// of it, as palette entries of their own past those: what finer nodes show, seen from afar.
struct Blend {
    static constexpr std::uint16_t blendable = 64;
    static constexpr int steps = 16; // the share is held in these parts

    std::uint16_t first = 0;
    std::uint16_t second = 0;
    double share = 1.0; // of the first

    // The entry: the first alone where the share rounds to all of it, or either isn't blendable.
    [[nodiscard]] std::uint16_t entry() const noexcept;
    // Of any entry; one that isn't a blend is itself alone.
    [[nodiscard]] static Blend of(std::uint16_t entry) noexcept;
};

// Tallies entries by how much of each shows, a blend counting its two by their shares, and
// blends the two that show most, the lowest of those tied first. None counts nothing.
class Blending {
public:
    void add(std::uint16_t entry, double weight = 1.0) noexcept;
    [[nodiscard]] std::uint16_t entry() const noexcept;

private:
    void tally(std::uint16_t entry, double weight) noexcept;

    std::array<std::uint16_t, 16> entries_{};
    std::array<double, 16> weights_{};
    std::size_t count_ = 0;
};

// Each kind of data a tree holds says how eight nodes sum up into the one over them, when one
// value stands for all eight so they needn't be split, and how its values are stored.

// Where the ground is and what it shows. A coarse node holds the plane through its eight, showing
// the blend of what those crossed show, so it looks as they do from afar, as rough as the most
// any of them strays from that plane; eight merge where one plane runs within `tolerance` finest
// nodes of theirs, so flat ground stays coarse.
struct Shape {
    using Value = Surface;
    static constexpr double tolerance = 0.25;
    // 2 added roughness.
    static constexpr std::uint8_t format = 2;

    [[nodiscard]] static Value summary(const std::array<Value, 8>& children) noexcept;
    // Of eight each `side` finest nodes across.
    [[nodiscard]] static std::optional<Value> merged(const std::array<Value, 8>& children,
                                                     double side) noexcept;
    [[nodiscard]] static std::uint64_t hash(const Value& value) noexcept;
    static void write(ByteWriter& out, const Value& value);
    // As written in `version` of the format.
    [[nodiscard]] static Result<Value> read(ByteReader& in, std::uint8_t version = format);
};

// Values that are one thing or another, never between, 0 for none: a coarse node holds the most
// common of its eight but none, the lowest of those tied, so it shows what's in it; none only
// where all are. Eight merge only when alike.
template <std::unsigned_integral Integer>
struct Discrete {
    using Value = Integer;
    static constexpr std::uint8_t format = 1;

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
    [[nodiscard]] static Result<Value> read(ByteReader& in, std::uint8_t /*version*/ = format) {
        return in.read<Value>();
    }
};

// An index into the world's palette of materials, each a named mix of what it's made of (a loam
// of sand, silt and clay, say), so one value says what digging there gives.
struct Material : Discrete<std::uint16_t> {};

// The water body a node's basin belongs to, 0 for none.
struct Water : Discrete<std::uint32_t> {};

// The woods on the ground through a node: whether any ground passes through it, the share of that
// ground under crowns, and each kind of tree's share of those crowns, in parts of `whole`, the
// shares summing to it where any stand. A node no ground passes through holds none.
struct Stand {
    static constexpr std::size_t kinds = 6;
    static constexpr int whole = 255;

    bool ground = false;
    std::uint8_t cover = 0;
    std::array<std::uint8_t, kinds> shares{};

    // Of ground with `cover` and `shares`, each 0 to 1, the shares rounded to sum to the whole
    // where they sum to any.
    [[nodiscard]] static Stand of(double cover, const std::array<double, kinds>& shares) noexcept;

    [[nodiscard]] double cover_share() const noexcept { return cover / static_cast<double>(whole); }
    [[nodiscard]] double share(std::size_t kind) const noexcept {
        return shares.at(kind) / static_cast<double>(whole);
    }

    friend constexpr bool operator==(const Stand&, const Stand&) noexcept = default;
};

// Woods sum up as the ground under them does: a coarse node's cover is the mean of its eight's
// that hold ground, and its shares theirs weighed by their cover. Eight merge where those holding
// ground are all within `tolerance` parts of that, whatever those holding none are, so even woods
// stay coarse.
struct Woods {
    using Value = Stand;
    static constexpr int tolerance = 13;
    static constexpr std::uint8_t format = 1;

    [[nodiscard]] static Value summary(const std::array<Value, 8>& children) noexcept;
    [[nodiscard]] static std::optional<Value> merged(const std::array<Value, 8>& children,
                                                     double side) noexcept;
    [[nodiscard]] static std::uint64_t hash(const Value& value) noexcept;
    // A byte, 0 for none, else 0x80 for ground and a bit for each kind with a share, the first
    // kind lowest; then the cover and those shares.
    static void write(ByteWriter& out, const Value& value);
    // Fails on a stand with shares summing to neither none nor the whole, or a value with no ground
    // and woods.
    [[nodiscard]] static Result<Value> read(ByteReader& in, std::uint8_t version = format);
};

} // namespace hog::grid
