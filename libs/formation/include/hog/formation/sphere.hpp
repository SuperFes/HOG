#pragma once

#include <hog/core/math.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::formation {

// Points spread evenly over the unit sphere, each joined to its five or six nearest: the graph the
// planet's large-scale stages work on. Made by splitting an icosahedron's faces, then nudging each
// point aside by up to `jitter` of the spacing, so paths over it don't follow lines of the split.
class Sphere {
public:
    static constexpr int max_subdivisions = 10;

    // Precondition: 0 <= subdivisions <= max_subdivisions, 0 <= jitter <= 0.3.
    Sphere(int subdivisions, std::uint64_t seed, double jitter = 0.3);

    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(points_.size());
    }
    [[nodiscard]] const DVec3& point(std::uint32_t index) const { return points_[index]; }
    [[nodiscard]] std::span<const std::uint32_t> neighbors(std::uint32_t index) const {
        return std::span{neighbors_}.subspan(offsets_[index], offsets_[index + 1] - offsets_[index]);
    }
    // Steradians, summing to 4π.
    [[nodiscard]] double area(std::uint32_t index) const { return areas_[index]; }
    // The mean distance between neighbours, in radians.
    [[nodiscard]] double spacing() const noexcept { return spacing_; }
    // Each wound anticlockwise seen from outside.
    [[nodiscard]] std::span<const std::array<std::uint32_t, 3>> triangles() const noexcept {
        return triangles_;
    }

    // The point nearest `direction`, walked to from `start`; cheap when `start` is near already.
    [[nodiscard]] std::uint32_t nearest(const DVec3& direction, std::uint32_t start = 0) const;

    // The triangle `direction` falls in and how much each corner counts there, summing to 1: how
    // a field held at the points is read anywhere between them.
    struct Spot {
        std::array<std::uint32_t, 3> corners{};
        DVec3 weights{};
    };
    [[nodiscard]] Spot locate(const DVec3& direction, std::uint32_t start = 0) const;

    template <typename Field>
    [[nodiscard]] double sample(const Field& field, const Spot& spot) const {
        return (spot.weights.x * field[spot.corners[0]]) + (spot.weights.y * field[spot.corners[1]]) +
               (spot.weights.z * field[spot.corners[2]]);
    }

private:
    std::vector<DVec3> points_;
    std::vector<std::uint32_t> offsets_;
    std::vector<std::uint32_t> neighbors_;
    // The triangles round each point, by the same offsets' kind of table.
    std::vector<std::uint32_t> touching_offsets_;
    std::vector<std::uint32_t> touching_;
    std::vector<double> areas_;
    std::vector<std::array<std::uint32_t, 3>> triangles_;
    double spacing_ = 0.0;
};

// Points a sphere split `subdivisions` times has.
[[nodiscard]] constexpr std::uint32_t sphere_size(int subdivisions) noexcept {
    return (10U << (2U * static_cast<unsigned>(subdivisions))) + 2U;
}

} // namespace hog::formation
