#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/volume_cell.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hog::planet {

// Stored by value, so new materials go on the end.
enum class Material : std::uint8_t {
    Air,
    Water,
    Snow,
    Ice,
    Soil,
    Sediment,
    Sandstone,
    Mudstone,
    Limestone,
    Granite,
    Basalt,
    Metamorphic,
    Magma,
};

// Where a surface crosses a leaf, as one plane through it: the leaf's material lies on the side the
// normal points to and `under` on the other. Sharper shapes than a plane split the leaf.
struct Surface {
    Material under = Material::Air;
    Vec3 normal{0.0F, 0.0F, 1.0F}; // unit, planet-centred
    float offset = 0.0F;           // metres from the cell's centre along the normal to the plane

    friend bool operator==(const Surface&, const Surface&) = default;
};

struct Contents {
    Material material = Material::Air;
    float temperature = 288.0F; // kelvin
    std::optional<Surface> surface;

    friend bool operator==(const Contents&, const Contents&) = default;
};

// The world as one sparse tree of cells over the six cube faces. Every cell is either a leaf,
// filled with one material, or split across into four or outward into two; leaves are as large as
// their contents allow. Splitting goes across before outward, so the same changes always make the
// same tree.
class Volume {
public:
    struct Leaf {
        VolumeCell cell;
        Contents contents;
    };

    explicit Volume(Contents everywhere);

    // The leaf holding all of `cell`, unless it spans more than one.
    [[nodiscard]] std::optional<Leaf> leaf_holding(VolumeCell cell) const;

    // Fills `cell` with `contents`, splitting the leaves it covers part of and dropping those it
    // covers whole.
    void set(VolumeCell cell, Contents contents);

    // Joins a cell's children back into one leaf if they're all leaves with the same contents;
    // says whether it did.
    bool merge(VolumeCell cell);
    // Joins every cell it can, from the finest up.
    void settle();

    [[nodiscard]] std::size_t leaf_count() const noexcept { return leaves_; }

    // Leaves in the tree's order, which the same changes always give.
    template <typename Visit>
    void for_each_leaf(Visit visit) const {
        std::vector<Pending> pending;
        for (int face = face_count - 1; face >= 0; --face) {
            pending.push_back({.node = static_cast<std::uint32_t>(face),
                               .at = VolumeCell::from(CellId::from_face(face))});
        }
        while (!pending.empty()) {
            const Pending next = pending.back();
            pending.pop_back();
            const Node& here = nodes_[next.node];
            if (here.split == Split::Leaf) {
                visit(next.at, here.contents);
                continue;
            }
            for (int index = child_count(here.split) - 1; index >= 0; --index) {
                pending.push_back({.node = here.children + static_cast<std::uint32_t>(index),
                                   .at = child(next.at, here.split, index)});
            }
        }
    }

private:
    enum class Split : std::uint8_t { Leaf, Across, Outward };

    struct Node {
        Split split = Split::Leaf;
        std::uint32_t children = 0;
        Contents contents;
    };

    struct Pending {
        std::uint32_t node;
        VolumeCell at;
    };

    // The node exactly at `cell`, if the tree reaches it.
    [[nodiscard]] std::optional<std::uint32_t> find(VolumeCell cell) const;
    void fill(std::uint32_t node, VolumeCell at, VolumeCell cell, Contents contents);
    void split(std::uint32_t node, Split split);
    void prune(std::uint32_t node);
    bool join(std::uint32_t node);

    [[nodiscard]] static int child_count(Split split) noexcept {
        return split == Split::Across ? 4 : 2;
    }
    [[nodiscard]] static VolumeCell child(VolumeCell at, Split split, int index);
    // Which of the children of `at` holds all of `cell`, unless it spans more than one.
    [[nodiscard]] static std::optional<int> child_holding(VolumeCell at, Split split,
                                                          VolumeCell cell);

    // The first six nodes are the faces'; the rest come in runs of four or two siblings, and runs
    // freed by merging are reused.
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> free_quads_;
    std::vector<std::uint32_t> free_pairs_;
    std::size_t leaves_ = face_count;
};

// The leaf at a position, unless it's outside the volume.
[[nodiscard]] std::optional<Volume::Leaf> leaf_at(const Volume& volume, double radius,
                                                  WorldPosition position);

// The material at a position in a leaf, by which side of its surface it's on.
[[nodiscard]] Material material_at(const Volume::Leaf& leaf, double radius, WorldPosition position);

// What a ray passes through in a leaf, from a point in it on to where it leaves: the material there
// and, if the ray crosses the leaf's surface first, the material past it and how far along.
struct LeafCrossing {
    Material first = Material::Air;
    std::optional<Material> then;
    double surface = 0.0;
    double exit = 0.0;
};
[[nodiscard]] LeafCrossing cross_leaf(const Volume::Leaf& leaf, double radius, WorldPosition from,
                                      DVec3 direction);

struct RayHit {
    Volume::Leaf leaf;
    Material material = Material::Air;
    double distance = 0.0; // metres from the ray's start to where it meets the material
};

// The first material a ray reaches, within `reach` metres, that `stops` it, whether at a leaf's
// side or its surface. A ray starting above the volume starts where it enters it; one leaving it
// stops nowhere.
// Precondition: `direction` is unit length.
template <typename Stops>
[[nodiscard]] std::optional<RayHit> cast_ray(const Volume& volume, double radius,
                                             WorldPosition from, DVec3 direction, double reach,
                                             Stops stops) {
    // Past a side by enough to land in the next cell, but under the finest shell's thickness.
    constexpr double nudge = 1.0e-4;
    double travelled = 0.0;
    const double top = radius + (volume_span / 2.0);
    const double b = glm::dot(from.metres(), direction);
    const double above = glm::dot(from.metres(), from.metres()) - (top * top);
    if (above >= 0.0) {
        const double meets = (b * b) - above;
        if (b >= 0.0 || meets < 0.0) {
            return std::nullopt;
        }
        travelled = (-b) - std::sqrt(meets) + nudge;
    }
    while (travelled <= reach) {
        const WorldPosition at = from + (direction * travelled);
        const auto leaf = leaf_at(volume, radius, at);
        if (!leaf.has_value()) {
            return std::nullopt;
        }
        const Volume::Leaf here = leaf.value_or(Volume::Leaf{});
        const LeafCrossing crossing = cross_leaf(here, radius, at, direction);
        if (stops(crossing.first)) {
            return RayHit{.leaf = here, .material = crossing.first, .distance = travelled};
        }
        if (crossing.then.has_value()) {
            const Material then = crossing.then.value_or(Material::Air);
            if (stops(then) && travelled + crossing.surface <= reach) {
                return RayHit{
                    .leaf = here, .material = then, .distance = travelled + crossing.surface};
            }
        }
        travelled += crossing.exit + nudge;
    }
    return std::nullopt;
}

} // namespace hog::planet
