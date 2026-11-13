#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace hog::flow {

// Ice creeping downhill over a set of columns, each one's bed, ice and the rock riding on it told
// in metres. Shallow-ice flow: what crosses a face between two columns grows with the fifth power
// of the thickness of the one it leaves and the square of the slope, and the ice wears the bed
// where it moves. Debris rides the ice at the share the column it leaves holds.
class Glacier {
public:
    // The sides of a column: +x, +y, -x, -y. A face is named by the column on its low side, so a
    // face is only walked from +x and +y.
    static constexpr std::size_t sides = 4;
    static constexpr int none = -1;

    struct Node {
        double rock = 0.0;   // bed height
        double ice = 0.0;    // thickness over the bed
        double debris = 0.0; // rock lying in the ice, metres deep
        std::array<int, sides> next = {none, none, none, none};
    };

    struct Law {
        double cell = 10.0;   // metres between columns
        double flow = 2.2e-5; // Glen's law, temperate ice, per year
        double wear = 1.0e-5; // metres of bed worn per metre of ice moved
        // A step is kept so thick ice can't force it ever finer, and a face gives away at most
        // this share of what its donor holds.
        double shortest = 0.05;
        double share = 0.2;
    };

    Glacier(std::size_t count, Law law) : nodes_(count), law_{law} {}

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] Node& operator[](std::size_t node) { return nodes_[node]; }
    [[nodiscard]] const Node& operator[](std::size_t node) const { return nodes_[node]; }
    [[nodiscard]] double surface(std::size_t node) const noexcept {
        return nodes_[node].rock + nodes_[node].ice;
    }
    [[nodiscard]] double total() const noexcept;

    // Where ice ends at a drop: a column holding ice beside a bare one its surface stands at
    // least `least` metres over. Icicles may hang there.
    struct Hang {
        std::size_t node = 0;
        std::size_t side = 0;
        double drop = 0.0;
    };
    [[nodiscard]] std::vector<Hang> hangs(double least) const;

    // Moves the ice for up to `years`; returns the years stepped, shorter when it needs finer
    // steps to stay stable. Never makes ice, loses it, or leaves a column under none.
    double step(double years);

private:
    std::vector<Node> nodes_;
    Law law_;
};

} // namespace hog::flow
