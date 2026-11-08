#pragma once

#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::formation {

// A value held at the sphere's points read smoothly anywhere between them, with its slope: each
// point near counts by a smooth bump falling to nothing `reach` spacings away (Wendland's), shared
// out so the counts sum to 1. Unlike reading across a triangle, there are no creases at its edges.
class Smooth {
public:
    explicit Smooth(const Sphere& sphere, double reach = 1.25);

    // The points near `direction` and how much each counts, from where it was last read.
    struct Weights {
        std::vector<std::uint32_t> points;
        std::vector<double> weights;  // summing to 1
        std::vector<DVec3> gradients; // of each weight, per radian across the sphere
        std::uint32_t nearest = 0;
    };
    void weigh(const DVec3& direction, Weights& into, std::uint32_t start = 0) const;

    struct Read {
        double value = 0.0;
        DVec3 gradient{0.0}; // per radian across the sphere, along it
    };
    [[nodiscard]] static Read read(std::span<const double> field, const Weights& weights);

    [[nodiscard]] const Sphere& sphere() const noexcept { return *sphere_; }
    [[nodiscard]] double reach() const noexcept { return reach_; } // radians

private:
    const Sphere* sphere_;
    double reach_;
};

} // namespace hog::formation
