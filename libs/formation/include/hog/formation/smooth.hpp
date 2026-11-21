#pragma once

#include <hog/formation/sphere.hpp>

#include <array>
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

    // The weights at `direction`, `s` and `t` of the way across a square of four places whose
    // weights are known, corners in the order (0, 0), (1, 0), (0, 1), (1, 1): each point's weight
    // a cubic Hermite of its weights and slopes there, a point missing from a corner counting
    // none, so a field read with them is near the same Hermite of its reads at the corners. A
    // weight the Hermite takes under nothing counts none, the rest shared out to sum to 1, so a
    // read lies between the points' values as a weighed one does. The nearest is the point
    // counting most.
    struct Corner {
        DVec3 direction{0.0};
        const Weights* weights = nullptr;
    };
    static void between(const DVec3& direction, const std::array<Corner, 4>& corners, double s,
                        double t, Weights& into);

    // The least and the most `field` reads anywhere within `within` radians of `direction`: each
    // read is shared out among the points within reach of it, so it lies between theirs.
    struct Range {
        double least = 0.0;
        double most = 0.0;
    };
    [[nodiscard]] Range range(std::span<const double> field, const DVec3& direction, double within,
                              std::uint32_t start = 0) const;

    [[nodiscard]] const Sphere& sphere() const noexcept { return *sphere_; }
    [[nodiscard]] double reach() const noexcept { return reach_; } // radians

private:
    const Sphere* sphere_;
    double reach_;
};

} // namespace hog::formation
