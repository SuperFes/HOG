#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/material.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace hog::globe {

// Solid where `distance` is negative: inside the ball, under the plane, inside the box. Each
// distance is a true one, so it changes no faster than a metre a metre.
struct Ball {
    DVec3 middle{0.0};
    double radius = 0.0;
};
struct HalfSpace {
    DVec3 normal{0.0, 0.0, 1.0}; // unit, out of the solid
    double offset = 0.0;         // the plane's metres from the origin along `normal`
};
struct Box {
    DVec3 middle{0.0};
    std::array<DVec3, 3> axes{DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.0}, DVec3{0.0, 0.0, 1.0}};
    DVec3 half{0.0}; // metres along each axis
};
using Form = std::variant<Ball, HalfSpace, Box>;

[[nodiscard]] double distance(const Form& form, DVec3 point) noexcept;

// A primitive laid on what's before it: adding its solid, or carving it out as open space.
struct Primitive {
    Form form;
    Material material = Material::Granite;
    bool carves = false;
};

// What a point holds, as the primitives laid in order have it: solid where the distance is
// negative, of the material last laid there, open elsewhere.
struct Inside {
    double distance = std::numeric_limits<double>::infinity(); // metres, negative in solid
    Material material = Material::Air;
};

struct Interval {
    double low = 0.0;
    double high = 0.0;
};

// A node of the tree: the cell `i`, `j` on `face` at `level`, and over it the `k`th span of radius
// up from the tree's inner radius, each span as tall as the level's cells are wide on average.
struct NodeKey {
    std::uint8_t level = 0;
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    std::uint32_t k = 0;

    [[nodiscard]] Cell cell() const noexcept {
        return {.level = level, .face = face, .i = i, .j = j};
    }
    friend constexpr auto operator<=>(const NodeKey&, const NodeKey&) noexcept = default;
};

// What a node holds, as one whole: summed from its children's where it's split.
struct Summary {
    double volume = 0.0; // cubic metres
    // Bounds of the distance anywhere in it.
    Interval distance{.low = std::numeric_limits<double>::infinity(),
                      .high = std::numeric_limits<double>::infinity()};
    std::array<double, material_count> of{}; // cubic metres of each solid
    // Whether the volumes are guessed from its primitives rather than known.
    bool estimated = false;

    [[nodiscard]] bool all_open() const noexcept { return distance.low > 0.0; }
    [[nodiscard]] bool all_solid() const noexcept { return distance.high < 0.0; }
    [[nodiscard]] double solid() const noexcept;
};

// Parts' summaries as one, the bounds taken over them all.
[[nodiscard]] Summary combine(std::span<const Summary> parts) noexcept;

// The shell between two radii as a tree over the cube-sphere's cells, built down until each node
// is all open, solid of one material, or holds no more than a few primitives, or is as small as
// asked. Each node keeps only the primitives that can matter anywhere in it, so a point is read
// from those of the node it's in.
class ShapeTree {
public:
    struct Limits {
        double smallest = 1.0;      // metres across, the finest nodes split down to
        std::size_t primitives = 4; // at most in a node left whole
    };
    ShapeTree(std::vector<Primitive> primitives, double inner, double outer, Limits limits);

    [[nodiscard]] double inner() const noexcept { return inner_; }
    [[nodiscard]] double outer() const noexcept { return outer_; }
    // Metres across the cells at `level` on average, and so how tall its spans are.
    [[nodiscard]] double width_at(int level) const noexcept;
    // The node at `level` holding `point`; within the shell.
    [[nodiscard]] NodeKey key_under(DVec3 point, int level) const;

    // What's at `point` from every primitive.
    [[nodiscard]] Inside at(DVec3 point) const noexcept;
    // The same from those the deepest node holding it keeps; or the deepest built over `key`,
    // which must hold it.
    [[nodiscard]] Inside read(DVec3 point) const;
    [[nodiscard]] Inside read(NodeKey key, DVec3 point) const;

    // The deepest node built over the place of `key`, at `key` or coarser.
    [[nodiscard]] NodeKey built_over(NodeKey key) const;
    [[nodiscard]] const Summary& summary(NodeKey key) const;
    [[nodiscard]] std::span<const std::uint32_t> primitives_in(NodeKey key) const;
    [[nodiscard]] std::vector<NodeKey> leaves() const;
    [[nodiscard]] std::size_t nodes() const noexcept { return nodes_; }

private:
    struct Node {
        NodeKey key;
        std::vector<std::uint32_t> primitives;
        // Whether they're laid over solid rather than open space: where a carve opens all of it,
        // nothing before the carve is kept.
        bool filled = false;
        Summary summary;
        std::vector<Node> children;
    };
    struct Region {
        Cell cell;
        double low = 0.0;
        double high = 0.0;
    };
    [[nodiscard]] Region region_of(NodeKey key) const noexcept;
    // The farthest any of `region` lies from `toward` at `middle` metres from the centre.
    [[nodiscard]] static double reach_of(const Region& region, DVec3 toward,
                                         double middle) noexcept;
    // Into `kept`, those of `from` that can matter within `reach` of `at`, and bounds of the
    // distance there: one that can't change it anywhere there is left out, and one that sets it
    // everywhere there leaves out all before it.
    [[nodiscard]] Interval prune(std::span<const std::uint32_t> from, DVec3 at, double reach,
                                 std::vector<std::uint32_t>& kept, bool& filled) const;
    [[nodiscard]] std::optional<Material> only_material(std::span<const std::uint32_t> kept) const;
    [[nodiscard]] Material nearest_solid(std::span<const std::uint32_t> kept, DVec3 at) const;
    // The node at `key` keeping what matters in it of `from`, laid over solid if `filled`.
    [[nodiscard]] Node make(NodeKey key, std::span<const std::uint32_t> from, bool filled) const;
    // Whether `node` is left unsplit.
    [[nodiscard]] bool whole(const Node& node) const noexcept;
    void split(Node& node) const;
    void build(Node& root);
    [[nodiscard]] const Node& deepest(NodeKey key) const;
    [[nodiscard]] Inside read(std::span<const std::uint32_t> primitives, bool filled,
                              DVec3 point) const noexcept;

    std::vector<Primitive> primitives_;
    double inner_;
    double outer_;
    Limits limits_;
    std::array<Node, 6> roots_;
    std::size_t nodes_ = 0;
};

} // namespace hog::globe
