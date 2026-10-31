#pragma once

#include <hog/globe/globe.hpp>
#include <hog/globe/hex_cell.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hog::globe {

// A globe's hexels at one level, held sparsely. Each diamond's (i, j) and the shells are split as a
// tree, across or outward, only where what they hold differs, so a block that is one hexel
// throughout is held once however large it is. Blocks small enough are held in full as bricks.
class HexelStore {
public:
    HexelStore(int level, Shells shells, Hexel fill = {});

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] const Shells& shells() const noexcept { return shells_; }

    [[nodiscard]] Hexel at(HexCell cell, int shell) const;
    void set(HexCell cell, int shell, Hexel hexel);
    // Every column's shells from `from` up to but not including `to`.
    void fill(int from, int to, Hexel hexel);

    // Merges blocks that have come to hold one hexel throughout, and lets go of what was painted
    // over since.
    void compact();

    [[nodiscard]] std::size_t nodes() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t bytes() const noexcept;

private:
    enum class Kind : std::uint8_t { Whole, Across, Up, Brick };
    struct Node {
        Hexel hexel;
        Kind kind = Kind::Whole;
        std::uint32_t at = 0; // the first child, or the brick's first hexel
    };
    struct Box {
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        std::uint32_t side = 1;
        int shell = 0;
        int span = 1;
    };
    struct Region {
        std::uint32_t i0 = 0;
        std::uint32_t i1 = 0;
        std::uint32_t j0 = 0;
        std::uint32_t j1 = 0;
        int shell0 = 0;
        int shell1 = 0;
    };

    [[nodiscard]] Box root(int diamond) const noexcept;
    void paint(std::uint32_t node, const Box& box, const Region& region, Hexel hexel);
    void paint_brick(std::uint32_t node, const Box& box, const Region& region, Hexel hexel);
    [[nodiscard]] Node compacted(std::uint32_t node, const Box& box, std::vector<Node>& into,
                                 std::vector<Hexel>& bricks) const;

    int level_;
    Shells shells_;
    std::vector<Node> nodes_;
    std::vector<Hexel> bricks_;
};

// A whole globe into a store and back.
[[nodiscard]] HexelStore pack(const Globe& globe);
void unpack(const HexelStore& store, Globe& globe);

// A window onto the store's `cells` from shell `from` up to but not including `to`, to work on.
[[nodiscard]] Globe window(const HexelStore& store, std::vector<HexCell> cells, int from, int to);
// Writes a window's hexels back where they came from.
void put_back(const Globe& window, HexelStore& store);

} // namespace hog::globe
