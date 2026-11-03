#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace hog::flow {

// Loose grain settling over a set of columns, each one's ground and grain told as heights in
// units where `Voxel::full` is one voxel's height. Grain runs from a column to any of the eight
// about it that lies lower than the heap can hold, by what the true distance across (diagonals
// longer by root two) allows at the angle of repose, so a heap comes to a cone, not a pyramid.
// Whole units only, and every column reads the state the pass began with, so the result doesn't
// depend on order and repeats.
class Slump {
public:
    // The sides of a column, axes first, then diagonals; `Node::next` is indexed by them.
    static constexpr std::size_t sides = 8;
    static constexpr int none = -1;

    struct Node {
        std::int64_t ground = 0;
        std::int64_t grain = 0;
        // Height a side lets the heap fall across an axis step, by the grain's repose there.
        std::int64_t rise = 0;
        // The most grain it can take; a closed ceiling over it, or no column at all.
        std::int64_t room = std::numeric_limits<std::int64_t>::max();
        // The most surface a side may drop and still be slid to. Grain asked over a longer drop
        // leaves the column as a fall, to land where it will, not at the neighbour now.
        std::int64_t reach = std::numeric_limits<std::int64_t>::max();
        std::array<int, sides> next = {none, none, none, none, none, none, none, none};
    };

    explicit Slump(std::size_t count) : nodes_(count) {}

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] Node& operator[](std::size_t node) { return nodes_[node]; }
    [[nodiscard]] const Node& operator[](std::size_t node) const { return nodes_[node]; }
    [[nodiscard]] std::int64_t surface(std::size_t node) const {
        return nodes_[node].ground + nodes_[node].grain;
    }
    [[nodiscard]] std::int64_t total() const;

    // Grain that went over a drop longer than its column's reach, and has not arrived anywhere.
    struct Fall {
        std::size_t from = 0;
        std::size_t side = 0;
        std::int64_t amount = 0;
    };
    [[nodiscard]] const std::vector<Fall>& falls() const noexcept { return falls_; }

    // One pass. Returns the units that moved or fell; none means it is at rest.
    std::int64_t step();

private:
    std::vector<Node> nodes_;
    std::vector<Fall> falls_;
};

} // namespace hog::flow
