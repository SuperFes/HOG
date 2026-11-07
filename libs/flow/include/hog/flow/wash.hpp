#pragma once

#include <hog/globe/material.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace hog::flow {

// Loose ground washed downhill by the water running off it, over a set of columns each draining
// into one other. Running water carries as much as its discharge and slope let it, Q^m S^n, and
// a material the more easily the less drag it withstands: topsoil and sand go first, clay holds,
// and the stones of rocky dirt move only where much water runs steeply. Where it can carry more
// than it brings it takes from the top of the ground, never past the rock; where less, it drops
// the coarsest first, as rocky dirt, then sand, then the fines as silt. Water standing still,
// in a lake or the sea, drops all it brings.
class Wash {
public:
    static constexpr int none = -1;

    struct Piece {
        globe::Material material = globe::Material::Soil;
        double depth = 0.0; // metres
        bool laid = false;  // dropped by the water, not weathered in place
    };

    struct Node {
        double ground = 0.0; // metres, the top of everything over the rock and the rock
        // The loose ground over the rock, bottom first.
        std::vector<Piece> loose;
        double discharge = 0.0; // cubic metres a year running on out
        // Or, where `gathers`, what runs off it and all that drains into it, this being what
        // runs off it alone.
        bool gathers = false;
        double runs_off = 0.0;
        int drains = none;      // where it runs to; none where it leaves the set
        bool still = false;     // water stands over it
    };

    struct Law {
        double cell = 1.0;            // metres between columns
        double capacity = 8.0e-5;     // cubic metres of topsoil a year for unit Q^m S^n
        double discharge_power = 1.5; // m
        double slope_power = 1.4;     // n
    };

    Wash(std::vector<Node> nodes, Law law) : nodes_(std::move(nodes)), law_{law} {
        taken_.assign(nodes_.size(), 0.0);
    }

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] const Node& operator[](std::size_t node) const { return nodes_[node]; }
    // Metres of the ground first given that the water has taken from the column.
    [[nodiscard]] double taken(std::size_t node) const { return taken_[node]; }

    // Washes for `years` in `passes`.
    void run(double years, int passes);

private:
    // What the water carries, by how it drops: the fines, sand, and stones.
    using Load = std::array<double, 3>;

    // Every column after the one it drains into. Columns whose water runs round in a ring are
    // taken as still.
    [[nodiscard]] std::vector<std::size_t> downhill_last();
    void pass(std::span<const std::size_t> order, double years);
    // Carries what of `load` the capacity `can` lets on and drops the rest; the share of the
    // capacity left.
    double carry(std::size_t node, double can, Load& load);
    void drop(std::size_t node, std::size_t kind, double volume);
    // Takes up to `share` of the capacity `can` from the top of the node, but not past `most`
    // cubic metres; what it took goes into `load`.
    double take(std::size_t node, double share, double can, double most, Load& load);

    std::vector<Node> nodes_;
    Law law_;
    std::vector<double> taken_;
};

} // namespace hog::flow
