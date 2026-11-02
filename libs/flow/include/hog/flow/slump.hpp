#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hog::flow {

// Loose grain settling in a patch of square columns, each column's ground and grain told as
// heights in units where `Voxel::full` is one voxel's height. Grain runs from a column to any of
// the eight about it that lies lower than the heap can hold, by what the true distance across
// (diagonals longer by root two) allows at the angle of repose; so a heap comes to a cone, not a
// pyramid. Whole units only, in an order that doesn't matter, so the result repeats.
class Slump {
public:
    Slump(int width, int depth);

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int depth() const noexcept { return depth_; }

    [[nodiscard]] std::int64_t& ground(int i, int j) { return ground_[index(i, j)]; }
    [[nodiscard]] std::int64_t& grain(int i, int j) { return grain_[index(i, j)]; }
    [[nodiscard]] std::int64_t ground(int i, int j) const { return ground_[index(i, j)]; }
    [[nodiscard]] std::int64_t grain(int i, int j) const { return grain_[index(i, j)]; }
    [[nodiscard]] std::int64_t surface(int i, int j) const { return ground(i, j) + grain(i, j); }
    [[nodiscard]] std::int64_t total() const;

    // One pass at `rise` height units of fall per column width the heap holds (the tangent of
    // the angle of repose times a column's width over a voxel's height, times `Voxel::full`).
    // Returns the units that moved; none means it is at rest.
    std::int64_t step(std::int64_t rise);

private:
    [[nodiscard]] std::size_t index(int i, int j) const {
        return (static_cast<std::size_t>(j) * static_cast<std::size_t>(width_)) +
               static_cast<std::size_t>(i);
    }

    int width_;
    int depth_;
    std::vector<std::int64_t> ground_;
    std::vector<std::int64_t> grain_;
};

} // namespace hog::flow
