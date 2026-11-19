#pragma once

#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/tree.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::grid {

// A tree as a page holds it, with the version it's at, bumped whenever it changes.
template <typename Kind>
struct Held {
    std::uint32_t version = 0;
    Tree<Kind> tree;
};

// The trees of one page: its root's key, the generator version that made them, and each kind's
// tree over the page's levels; a page may lack any of them.
struct Page {
    static constexpr std::uint8_t format = 1;

    Key root;
    std::uint32_t generator = 0;
    std::optional<Held<Shape>> shape;
    std::optional<Held<Material>> material;
    std::optional<Held<Water>> water;
    std::optional<Held<Woods>> woods;

    // Little-endian: format, the root's depth and x, y, z, the generator version, a byte saying
    // which trees follow (1 shape, 2 material, 4 water, 8 woods), then each in that order: its
    // version, a
    // varint length and its bytes.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't a page, or a tree deeper than a page.
    [[nodiscard]] static Result<Page> decode(std::span<const std::byte> bytes);
};

} // namespace hog::grid
