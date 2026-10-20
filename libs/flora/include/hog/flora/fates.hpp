#pragma once

#include <hog/flora/stand.hpp>

#include <cstdint>
#include <vector>

namespace hog::flora {

// How one cell's woods fare, as the forest sim keeps them: Habitat's shares of its trees, and the
// share of its ground a fire left black.
struct Fate {
    float living = 1.0F;
    float established = 1.0F;
    float failing = 0.0F;
    float dead = 0.0F;
    float charred = 0.0F;
    float scar = 0.0F;

    friend bool operator==(const Fate&, const Fate&) = default;
};

// How the woods fare over the whole planet: a fate for each cube-sphere cell at one level, face by
// face and row by row as sim::CellGrid orders them, untouched where none was told. Read between
// the cells' middles in plain float arithmetic, so every machine stands the same trees in the
// same state.
class Fates {
public:
    // Untouched everywhere.
    Fates() = default;
    // Precondition: 6 × 4^level fates.
    Fates(int level, std::vector<Fate> cells);

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] bool untouched() const noexcept { return cells_.empty(); }
    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(cells_.size());
    }
    [[nodiscard]] const Fate& cell(std::uint32_t index) const { return cells_.at(index); }

    // The fate at a spot given by its face and its s and t, 0 to 1 across it: eased linearly
    // between the middles of the four cells about it, and held at the face's edges.
    [[nodiscard]] Fate at(std::uint32_t face, double s, double t) const;

private:
    int level_ = 0;
    std::uint32_t side_ = 1;
    std::vector<Fate> cells_;
};

// The habitat, with the woods' fate there taken in.
[[nodiscard]] Habitat fared(Habitat habitat, const Fate& fate) noexcept;

} // namespace hog::flora
