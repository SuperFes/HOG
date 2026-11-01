#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>

#include <optional>
#include <set>
#include <span>
#include <vector>

namespace hog::globe {

// How far from the eye a chunk is: from where the eye stands, `height` over the ground under the
// unit direction `up`, to the nearest of the chunk's ground, all taken on the planet's radius.
struct Eye {
    DVec3 up{0.0, 0.0, 1.0};
    double height = 0.0; // metres
};

[[nodiscard]] double distance_to(const Generator& generator, Eye eye, Cell chunk);

// The chunks drawn, by which each finds the detail its neighbours are drawn at, to meet them.
class Drawn {
public:
    // With none, each chunk's neighbours are taken as drawn at its own detail.
    Drawn() = default;
    explicit Drawn(std::span<const Cell> chunks) : chunks_{chunks.begin(), chunks.end()} {}

    // The drawn chunk a cell lies in, if any.
    [[nodiscard]] std::optional<Cell> over(Cell cell) const;
    [[nodiscard]] bool contains(Cell chunk) const { return chunks_.contains(chunk); }
    [[nodiscard]] bool empty() const noexcept { return chunks_.empty(); }

private:
    std::set<Cell> chunks_;
};

// Chunks tiling the ground within `reach` metres of the eye, each as coarse as keeps it `ratio`
// times its width away, nearest first. Chunks beside each other, across their sides and corners,
// are at most one detail apart, as seams are drawn only between neighbours a detail apart; across
// the cube's edges, where the faces' lattices meet unevenly, they're at the same detail.
// Precondition: ratio > 1.
[[nodiscard]] std::vector<Cell> chunks_around(const Generator& generator, Eye eye, double reach,
                                              double ratio);

} // namespace hog::globe
