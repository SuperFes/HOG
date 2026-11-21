#include <hog/core/portable_maths.hpp>
#include <hog/formation/smooth.hpp>

#include <algorithm>
#include <limits>
#include <vector>

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

void Smooth::between(const DVec3& direction, const std::array<Corner, 4>& corners, double s,
                     double t, Weights& into) {
    into.points.clear();
    into.weights.clear();
    into.gradients.clear();
    // Each corner's weight and slope for every point in any of them.
    struct Known {
        std::array<double, 4> weight{};
        std::array<DVec3, 4> gradient{};
    };
    thread_local std::vector<Known> known;
    known.clear();
    for (std::size_t c = 0; c < 4; ++c) {
        const Weights& at = *corners.at(c).weights;
        for (std::size_t n = 0; n < at.points.size(); ++n) {
            const auto found = std::ranges::find(into.points, at.points[n]);
            const auto k = static_cast<std::size_t>(found - into.points.begin());
            if (found == into.points.end()) {
                into.points.push_back(at.points[n]);
                known.emplace_back();
            }
            known[k].weight.at(c) = at.weights[n];
            known[k].gradient.at(c) = at.gradients[n];
        }
    }
    // The square's sides, along s at t = 0 and 1, and along t at s = 0 and 1.
    const std::array<DVec3, 2> along_s{corners[1].direction - corners[0].direction,
                                       corners[3].direction - corners[2].direction};
    const std::array<DVec3, 2> along_t{corners[2].direction - corners[0].direction,
                                       corners[3].direction - corners[1].direction};
    // Hermite's: each end's value and slope, and how fast each changes.
    const auto basis = [](double x) {
        const double x2 = x * x;
        const double x3 = x2 * x;
        return std::array{std::array{1.0 - (3.0 * x2) + (2.0 * x3), (3.0 * x2) - (2.0 * x3),
                                     x - (2.0 * x2) + x3, x3 - x2},
                          std::array{(6.0 * x2) - (6.0 * x), (6.0 * x) - (6.0 * x2),
                                     1.0 - (4.0 * x) + (3.0 * x2), (3.0 * x2) - (2.0 * x)}};
    };
    const auto [hs, dhs] = basis(s);
    const auto [ht, dht] = basis(t);
    // Turning slopes across the square back into one along the sphere.
    const DVec3 es = (along_s[0] * (1.0 - t)) + (along_s[1] * t);
    const DVec3 et = (along_t[0] * (1.0 - s)) + (along_t[1] * s);
    const double ss = glm::dot(es, es);
    const double st = glm::dot(es, et);
    const double tt = glm::dot(et, et);
    const double det = (ss * tt) - (st * st);
    double most = -1.0;
    for (std::size_t k = 0; k < into.points.size(); ++k) {
        // Its slope's turn across the square, from how each side's slope changes along the other.
        const auto slope_s = [&](std::size_t c) {
            return glm::dot(known[k].gradient.at(c), along_s.at(c >> 1U));
        };
        const auto slope_t = [&](std::size_t c) {
            return glm::dot(known[k].gradient.at(c), along_t.at(c & 1U));
        };
        const double twist = 0.25 * ((slope_s(2) - slope_s(0)) + (slope_s(3) - slope_s(1)) +
                                     (slope_t(1) - slope_t(0)) + (slope_t(3) - slope_t(2)));
        double value = 0.0;
        double by_s = 0.0;
        double by_t = 0.0;
        for (std::size_t c = 0; c < 4; ++c) {
            const std::size_t a = c & 1U;
            const std::size_t b = c >> 1U;
            const double f = known[k].weight.at(c);
            const double fs = glm::dot(known[k].gradient.at(c), along_s.at(b));
            const double ft = glm::dot(known[k].gradient.at(c), along_t.at(a));
            value += (hs.at(a) * ht.at(b) * f) + (hs.at(2 + a) * ht.at(b) * fs) +
                     (hs.at(a) * ht.at(2 + b) * ft) + (hs.at(2 + a) * ht.at(2 + b) * twist);
            by_s += (dhs.at(a) * ht.at(b) * f) + (dhs.at(2 + a) * ht.at(b) * fs) +
                    (dhs.at(a) * ht.at(2 + b) * ft) + (dhs.at(2 + a) * ht.at(2 + b) * twist);
            by_t += (hs.at(a) * dht.at(b) * f) + (hs.at(2 + a) * dht.at(b) * fs) +
                    (hs.at(a) * dht.at(2 + b) * ft) + (hs.at(2 + a) * dht.at(2 + b) * twist);
        }
        DVec3 gradient{0.0};
        if (det > 0.0) {
            const double along_es = ((tt * by_s) - (st * by_t)) / det;
            const double along_et = ((ss * by_t) - (st * by_s)) / det;
            gradient = (es * along_es) + (et * along_et);
            gradient -= glm::dot(gradient, direction) * direction;
        }
        if (value <= 0.0) {
            value = 0.0;
            gradient = DVec3{0.0};
        }
        into.weights.push_back(value);
        into.gradients.push_back(gradient);
        if (value > most) {
            most = value;
            into.nearest = into.points[k];
        }
    }
    double total = 0.0;
    DVec3 total_gradient{0.0};
    for (std::size_t n = 0; n < into.weights.size(); ++n) {
        total += into.weights[n];
        total_gradient += into.gradients[n];
    }
    for (std::size_t n = 0; n < into.weights.size(); ++n) {
        into.gradients[n] =
            ((into.gradients[n] * total) - (into.weights[n] * total_gradient)) / (total * total);
        into.weights[n] /= total;
    }
}

} // namespace hog::formation
