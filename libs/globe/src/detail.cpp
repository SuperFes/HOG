#include <hog/core/assert.hpp>
#include <hog/globe/detail.hpp>
#include <hog/globe/surface_mesh.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// Metres across a chunk's side.
double width_of(const Generator& generator, Cell chunk) {
    const std::array<DVec3, 4> corner = corners(chunk);
    return glm::length(corner[1] - corner[0]) * generator.planet().radius;
}

// Whether `beside` lies across a side of `cell`, not only at a corner.
bool sides_on(Cell cell, Cell beside) {
    const CellNeighbors near = sides(cell);
    return std::ranges::find(near.all(), beside) != near.all().end();
}

// The chunk held in `leaves` that covers `cell`: it or the nearest coarser one over it.
std::optional<Cell> covering(const std::set<Cell>& leaves, Cell cell) {
    for (Cell over = cell;; over = parent(over)) {
        if (leaves.contains(over)) {
            return over;
        }
        if (over.level == 0) {
            return std::nullopt;
        }
    }
}

// A chunk beside this one more than a detail coarser, or across the cube's edge, coarser at all.
std::optional<Cell> too_coarse_beside(const std::set<Cell>& leaves, Cell chunk) {
    const CellNeighbors near = around(chunk);
    for (const Cell beside : near.all()) {
        const std::optional<Cell> coarse = covering(leaves, beside);
        const int most_apart = beside.face != chunk.face && sides_on(chunk, beside) ? 0 : 1;
        if (coarse && coarse->level + most_apart < chunk.level) {
            return coarse;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<Cell> Drawn::over(Cell cell) const {
    return covering(chunks_, cell);
}

double distance_to(const Generator& generator, Eye eye, Cell chunk) {
    const double radius = generator.planet().radius;
    const DVec3 from = eye.up * (radius + eye.height);
    const DVec3 middle = center(chunk);
    double across = 0.0;
    for (const DVec3 corner : corners(chunk)) {
        across = std::max(across, glm::length(corner - middle));
    }
    return std::max(0.0, glm::length(from - (middle * radius)) - (across * radius));
}

std::vector<Cell> chunks_around(const Generator& generator, Eye eye, double reach, double ratio) {
    HOG_ASSERT(ratio > 1.0, "a chunk is drawn further off than it's wide");
    const Layers& layers = generator.layers();
    const int finest = layers.level - chunk_levels;
    const auto within = [&](Cell chunk) { return distance_to(generator, eye, chunk) <= reach; };
    std::set<Cell> leaves;
    std::vector<Cell> open;
    open.reserve(6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        open.push_back({.level = 0, .face = face, .i = 0, .j = 0});
    }
    while (!open.empty()) {
        const Cell chunk = open.back();
        open.pop_back();
        if (!within(chunk)) {
            continue;
        }
        if (std::cmp_less(chunk.level, finest) &&
            distance_to(generator, eye, chunk) < ratio * width_of(generator, chunk)) {
            const std::array<Cell, 4> finer = children(chunk);
            open.insert(open.end(), finer.begin(), finer.end());
        } else {
            leaves.insert(chunk);
        }
    }
    // Where a neighbour is too coarse, it's split, until none is.
    std::vector<Cell> unchecked{leaves.begin(), leaves.end()};
    while (!unchecked.empty()) {
        const Cell chunk = unchecked.back();
        unchecked.pop_back();
        if (!leaves.contains(chunk)) {
            continue;
        }
        const std::optional<Cell> coarse = too_coarse_beside(leaves, chunk);
        if (!coarse) {
            continue;
        }
        leaves.erase(*coarse);
        for (const Cell finer : children(*coarse)) {
            if (within(finer)) {
                leaves.insert(finer);
                unchecked.push_back(finer);
            }
        }
        // Split one, it may need splitting again; and this one checked again.
        unchecked.push_back(chunk);
    }
    std::vector<Cell> found{leaves.begin(), leaves.end()};
    std::ranges::sort(found, {}, [&](Cell chunk) { return distance_to(generator, eye, chunk); });
    return found;
}

} // namespace hog::globe
