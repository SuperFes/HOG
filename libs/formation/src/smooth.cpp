#include <hog/core/portable_maths.hpp>
#include <hog/formation/smooth.hpp>

#include <algorithm>
#include <limits>

namespace hog::formation {

namespace {

double angle(const DVec3& a, const DVec3& b) noexcept {
    return maths::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

} // namespace

Smooth::Smooth(const Sphere& sphere, double reach)
    : sphere_{&sphere}, reach_{reach * sphere.spacing()} {}

void Smooth::weigh(const DVec3& direction, Weights& into, std::uint32_t start) const {
    const Sphere& sphere = *sphere_;
    into.points.clear();
    into.weights.clear();
    into.gradients.clear();
    into.nearest = sphere.nearest(direction, start);
    // Out from the nearest point through every point a bump could reach from, keeping those it
    // does.
    std::vector<std::uint32_t> seen{into.nearest};
    const double onward = reach_ + (0.5 * sphere.spacing());
    for (std::size_t next = 0; next < seen.size(); ++next) {
        const std::uint32_t point = seen[next];
        const double away = angle(direction, sphere.point(point));
        if (away < reach_) {
            into.points.push_back(point);
        }
        if (away >= onward) {
            continue;
        }
        for (const std::uint32_t beside : sphere.neighbors(point)) {
            if (std::ranges::find(seen, beside) == seen.end()) {
                seen.push_back(beside);
            }
        }
    }

    double total = 0.0;
    DVec3 total_gradient{0.0};
    for (const std::uint32_t point : into.points) {
        const DVec3& at = sphere.point(point);
        const double r = angle(direction, at) / reach_;
        const double fall = 1.0 - r;
        const double weight = fall * fall * fall * fall * ((4.0 * r) + 1.0);
        // The bump falls as the direction moves away from the point.
        const DVec3 toward = at - (glm::dot(at, direction) * direction);
        const double across = glm::length(toward);
        const DVec3 slope = across > 0.0
                                ? (-20.0 * r * fall * fall * fall / reach_) * (-toward / across)
                                : DVec3{0.0};
        into.weights.push_back(weight);
        into.gradients.push_back(slope);
        total += weight;
        total_gradient += slope;
    }
    // Shared out to sum to 1, the slopes of the shares following.
    for (std::size_t n = 0; n < into.weights.size(); ++n) {
        const double weight = into.weights[n];
        into.gradients[n] =
            ((into.gradients[n] * total) - (weight * total_gradient)) / (total * total);
        into.weights[n] = weight / total;
    }
}

Smooth::Range Smooth::range(std::span<const double> field, const DVec3& direction, double within,
                            std::uint32_t start) const {
    const Sphere& sphere = *sphere_;
    const double near = within + reach_;
    const double onward = near + (0.5 * sphere.spacing());
    std::vector<std::uint32_t> seen{sphere.nearest(direction, start)};
    Range range{.least = std::numeric_limits<double>::infinity(),
                .most = -std::numeric_limits<double>::infinity()};
    for (std::size_t next = 0; next < seen.size(); ++next) {
        const std::uint32_t point = seen[next];
        const double away = angle(direction, sphere.point(point));
        if (away < near) {
            range.least = std::min(range.least, field[point]);
            range.most = std::max(range.most, field[point]);
        }
        if (away >= onward) {
            continue;
        }
        for (const std::uint32_t beside : sphere.neighbors(point)) {
            if (std::ranges::find(seen, beside) == seen.end()) {
                seen.push_back(beside);
            }
        }
    }
    return range;
}

Smooth::Read Smooth::read(std::span<const double> field, const Weights& weights) {
    Read read;
    for (std::size_t n = 0; n < weights.points.size(); ++n) {
        const double value = field[weights.points[n]];
        read.value += weights.weights[n] * value;
        read.gradient += weights.gradients[n] * value;
    }
    return read;
}

} // namespace hog::formation
