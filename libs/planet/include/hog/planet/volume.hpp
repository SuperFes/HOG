#pragma once

#include <hog/planet/volume_cell.hpp>

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

struct Contents {
    Material material = Material::Air;
    float temperature = 288.0F; // kelvin

    friend constexpr bool operator==(const Contents&, const Contents&) = default;
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

} // namespace hog::planet
