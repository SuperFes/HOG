#include <hog/core/assert.hpp>
#include <hog/planet/volume.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <vector>

namespace hog::planet {

namespace {

[[nodiscard]] bool overlaps(VolumeCell a, VolumeCell b) {
    const CellId column = a.column();
    const bool across = column.contains(b.column()) || b.column().contains(column);
    const VolumeCell a_shells = VolumeCell::from(column, a.shell_level(), a.shell());
    const VolumeCell b_shells = VolumeCell::from(column, b.shell_level(), b.shell());
    return across && (a_shells.contains(b_shells) || b_shells.contains(a_shells));
}

} // namespace

Volume::Volume(Contents everywhere)
    : nodes_(face_count, Node{.split = Split::Leaf, .children = 0, .contents = everywhere}) {}

VolumeCell Volume::child(VolumeCell at, Split split, int index) {
    return split == Split::Across ? at.across_child(index) : at.shell_child(index);
}

std::optional<int> Volume::child_holding(VolumeCell at, Split split, VolumeCell cell) {
    for (int index = 0; index < child_count(split); ++index) {
        if (child(at, split, index).contains(cell)) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> Volume::find(VolumeCell cell) const {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    auto node = static_cast<std::uint32_t>(cell.column().face());
    VolumeCell at = VolumeCell::from(CellId::from_face(cell.column().face()));
    while (at != cell) {
        const Node& here = nodes_[node];
        if (here.split == Split::Leaf) {
            return std::nullopt;
        }
        const auto index = child_holding(at, here.split, cell);
        if (!index) {
            return std::nullopt;
        }
        node = here.children + static_cast<std::uint32_t>(*index);
        at = child(at, here.split, *index);
    }
    return node;
}

std::optional<Volume::Leaf> Volume::leaf_holding(VolumeCell cell) const {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    auto node = static_cast<std::uint32_t>(cell.column().face());
    VolumeCell at = VolumeCell::from(CellId::from_face(cell.column().face()));
    while (nodes_[node].split != Split::Leaf) {
        const Node& here = nodes_[node];
        const auto index = child_holding(at, here.split, cell);
        if (!index) {
            return std::nullopt;
        }
        node = here.children + static_cast<std::uint32_t>(*index);
        at = child(at, here.split, *index);
    }
    return Leaf{.cell = at, .contents = nodes_[node].contents};
}

void Volume::set(VolumeCell cell, Contents contents) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const int face = cell.column().face();
    fill(static_cast<std::uint32_t>(face), VolumeCell::from(CellId::from_face(face)), cell,
         contents);
}

void Volume::fill(std::uint32_t node, VolumeCell at, VolumeCell cell, Contents contents) {
    std::vector<Pending> pending{{.node = node, .at = at}};
    while (!pending.empty()) {
        const Pending next = pending.back();
        pending.pop_back();
        if (cell.contains(next.at)) {
            prune(next.node);
            nodes_[next.node].contents = contents;
            continue;
        }
        if (nodes_[next.node].split == Split::Leaf) {
            if (nodes_[next.node].contents == contents) {
                continue;
            }
            split(next.node, next.at.column().level() < cell.column().level() ? Split::Across
                                                                              : Split::Outward);
        }
        const Node& here = nodes_[next.node];
        for (int index = 0; index < child_count(here.split); ++index) {
            const VolumeCell under = child(next.at, here.split, index);
            if (overlaps(under, cell)) {
                pending.push_back(
                    {.node = here.children + static_cast<std::uint32_t>(index), .at = under});
            }
        }
    }
}

void Volume::split(std::uint32_t node, Split split) {
    const int count = child_count(split);
    auto& free = split == Split::Across ? free_quads_ : free_pairs_;
    std::uint32_t first = 0;
    if (free.empty()) {
        first = static_cast<std::uint32_t>(nodes_.size());
        nodes_.resize(nodes_.size() + static_cast<std::size_t>(count));
    } else {
        first = free.back();
        free.pop_back();
    }
    const Contents contents = nodes_[node].contents;
    for (int index = 0; index < count; ++index) {
        nodes_[first + static_cast<std::uint32_t>(index)] =
            Node{.split = Split::Leaf, .children = 0, .contents = contents};
    }
    nodes_[node].split = split;
    nodes_[node].children = first;
    leaves_ += static_cast<std::size_t>(count) - 1;
}

void Volume::prune(std::uint32_t node) {
    std::vector<std::uint32_t> pending{node};
    while (!pending.empty()) {
        const std::uint32_t next = pending.back();
        pending.pop_back();
        const Node here = nodes_[next];
        if (here.split == Split::Leaf) {
            continue;
        }
        const int count = child_count(here.split);
        for (int index = 0; index < count; ++index) {
            pending.push_back(here.children + static_cast<std::uint32_t>(index));
        }
        (here.split == Split::Across ? free_quads_ : free_pairs_).push_back(here.children);
        nodes_[next].split = Split::Leaf;
        leaves_ -= static_cast<std::size_t>(count) - 1;
    }
}

bool Volume::merge(VolumeCell cell) {
    const auto node = find(cell);
    return node && join(*node);
}

bool Volume::join(std::uint32_t node) {
    const Node here = nodes_[node];
    if (here.split == Split::Leaf) {
        return false;
    }
    const Node& first = nodes_[here.children];
    for (int index = 0; index < child_count(here.split); ++index) {
        const Node& under = nodes_[here.children + static_cast<std::uint32_t>(index)];
        if (under.split != Split::Leaf || under.contents != first.contents) {
            return false;
        }
    }
    const Contents contents = first.contents;
    prune(node);
    nodes_[node].contents = contents;
    return true;
}

void Volume::settle() {
    // Parents come before their children, so joining from the back joins the finest first.
    std::vector<std::uint32_t> split;
    std::vector<std::uint32_t> pending{0, 1, 2, 3, 4, 5};
    while (!pending.empty()) {
        const std::uint32_t next = pending.back();
        pending.pop_back();
        const Node& here = nodes_[next];
        if (here.split == Split::Leaf) {
            continue;
        }
        split.push_back(next);
        for (int index = 0; index < child_count(here.split); ++index) {
            pending.push_back(here.children + static_cast<std::uint32_t>(index));
        }
    }
    for (const std::uint32_t node : std::views::reverse(split)) {
        join(node);
    }
}

std::optional<Volume::Leaf> leaf_at(const Volume& volume, double radius, WorldPosition position) {
    const VolumeCell finest = volume_cell_at(position, radius, max_level, max_shell_level);
    if (!finest.is_valid()) {
        return std::nullopt;
    }
    return volume.leaf_holding(finest);
}

} // namespace hog::planet
