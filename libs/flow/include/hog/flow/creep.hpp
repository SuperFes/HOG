#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace hog::flow {

// Loose cover creeping downhill over a set of columns, each one's bed and the cover on it told in
// metres. What crosses a face between two columns grows with the slope of the surface between
// them, ever faster as it nears the steepest the cover can lie, so cover thins on steep ground and
// gathers in hollows, and in time the surface comes smooth. Weathering makes new cover from the
// bed, less where there is already a lot.
class Creep {
public:
    // The sides of a column: +x, +y, -x, -y. A face is named by the column on its low side.
    static constexpr std::size_t sides = 4;
    static constexpr int none = -1;

    struct Node {
        double bed = 0.0;   // height of what isn't cover
        double cover = 0.0; // thickness of the cover over it
        std::array<int, sides> next = {none, none, none, none};
    };

    struct Law {
        double cell = 2.5;          // metres between columns
        double diffusivity = 0.005; // metres squared a year across a gentle slope
        double steepest = 1.0;      // slope at which cover can no longer lie, rise over run
        double production = 0.0002; // metres of new cover a year where there is none
        double deep = 1.5;          // metres of cover at which weathering has slowed by 1/e
        double share = 0.2;         // most of a surface gap a face may close in a step
    };

    Creep(std::size_t count, Law law) : nodes_(count), law_{law} {}

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] Node& operator[](std::size_t node) { return nodes_[node]; }
    [[nodiscard]] const Node& operator[](std::size_t node) const { return nodes_[node]; }
    [[nodiscard]] double surface(std::size_t node) const noexcept {
        return nodes_[node].bed + nodes_[node].cover;
    }
    [[nodiscard]] double total() const noexcept;

    // Moves the cover for up to `years`; returns the years stepped, shorter where it needs finer
    // steps to stay stable. Never makes or loses cover, or leaves a column under none.
    double step(double years);
    // Weathers new cover from the bed for `years`.
    void weather(double years);

private:
    std::vector<Node> nodes_;
    Law law_;
};

} // namespace hog::flow
