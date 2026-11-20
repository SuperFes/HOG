#pragma once

#include "kept_cells.hpp"
#include "terms.hpp"

#include <hog/globe/cave_piece.hpp>
#include <hog/globe/cell.hpp>

#include <cstddef>
#include <memory>
#include <vector>

namespace hog::globe {

struct GroundKey {
    Cell cell;
    double lod = 0.0;
    bool crags = true;
    friend bool operator==(const GroundKey&, const GroundKey&) noexcept = default;
};

struct GroundKeyHash {
    std::size_t operator()(const GroundKey& key) const noexcept;
};

// The ground at the middles of the cube's cells, as terms::ground reads it.
class GroundCells : public KeptCells<GroundKey, terms::Ground, GroundKeyHash> {
public:
    using Key = GroundKey;
    static constexpr std::size_t default_most = std::size_t{1} << 17U;
    explicit GroundCells(std::size_t most = default_most) : KeptCells{most} {}
};

struct CellHash {
    std::size_t operator()(const Cell& cell) const noexcept;
};

using CavePieces = std::shared_ptr<const std::vector<CavePiece>>;

// The caves laid out in each cave cell, as terms::caves_in lays them out.
class CaveCells : public KeptCells<Cell, CavePieces, CellHash> {
public:
    static constexpr std::size_t default_most = std::size_t{1} << 14U;
    explicit CaveCells(std::size_t most = default_most) : KeptCells{most} {}
};

} // namespace hog::globe
