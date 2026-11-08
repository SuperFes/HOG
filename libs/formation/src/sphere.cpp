#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>
#include <hog/formation/sphere.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <utility>

namespace hog::formation {

namespace {

using Triangle = std::array<std::uint32_t, 3>;

double angle_between(const DVec3& a, const DVec3& b) noexcept {
    return maths::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

glm::length_t smallest_of(const DVec3& values) noexcept {
    glm::length_t smallest = 0;
    for (glm::length_t at = 1; at < 3; ++at) {
        if (values[at] < values[smallest]) {
            smallest = at;
        }
    }
    return smallest;
}

// Uniform in [0, 1) from the top 53 bits.
double unit(std::uint64_t bits) noexcept {
    return static_cast<double>(bits >> 11U) * 0x1.0p-53;
}

void icosahedron(std::vector<DVec3>& points, std::vector<Triangle>& triangles) {
    const double phi = std::numbers::phi;
    points = {{-1, phi, 0}, {1, phi, 0}, {-1, -phi, 0}, {1, -phi, 0},
              {0, -1, phi}, {0, 1, phi}, {0, -1, -phi}, {0, 1, -phi},
              {phi, 0, -1}, {phi, 0, 1}, {-phi, 0, -1}, {-phi, 0, 1}};
    for (DVec3& point : points) {
        point = glm::normalize(point);
    }
    triangles = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                 {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                 {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                 {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
}

void subdivide(std::vector<DVec3>& points, std::vector<Triangle>& triangles) {
    std::unordered_map<std::uint64_t, std::uint32_t> middles;
    middles.reserve(triangles.size() * 3 / 2);
    const auto middle = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint64_t key = (std::uint64_t{std::min(a, b)} << 32U) | std::max(a, b);
        const auto [at, added] =
            middles.try_emplace(key, static_cast<std::uint32_t>(points.size()));
        if (added) {
            points.push_back(glm::normalize(points[a] + points[b]));
        }
        return at->second;
    };
    std::vector<Triangle> split;
    split.reserve(triangles.size() * 4);
    for (const auto& [a, b, c] : triangles) {
        const std::uint32_t ab = middle(a, b);
        const std::uint32_t bc = middle(b, c);
        const std::uint32_t ca = middle(c, a);
        split.push_back({a, ab, ca});
        split.push_back({b, bc, ab});
        split.push_back({c, ca, bc});
        split.push_back({ab, bc, ca});
    }
    triangles = std::move(split);
}

} // namespace

Sphere::Sphere(int subdivisions, std::uint64_t seed, double jitter) : subdivisions_{subdivisions} {
    HOG_ASSERT(subdivisions >= 0 && subdivisions <= max_subdivisions, "subdivisions out of range");
    HOG_ASSERT(jitter >= 0.0 && jitter <= 0.3, "jitter out of range");
    icosahedron(points_, triangles_);
    for (int step = 0; step < subdivisions; ++step) {
        subdivide(points_, triangles_);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    edges.reserve(triangles_.size() * 6);
    for (const auto& [a, b, c] : triangles_) {
        for (const auto& [from, to] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
            edges.emplace_back(from, to);
            edges.emplace_back(to, from);
        }
    }
    std::ranges::sort(edges);
    const auto [last, end] = std::ranges::unique(edges);
    edges.erase(last, end);
    offsets_.assign(points_.size() + 1, 0);
    neighbors_.reserve(edges.size());
    for (const auto& [from, to] : edges) {
        ++offsets_[from + 1];
        neighbors_.push_back(to);
    }
    for (std::size_t n = 1; n < offsets_.size(); ++n) {
        offsets_[n] += offsets_[n - 1];
    }

    const auto mean_spacing = [&] {
        double sum = 0.0;
        for (const auto& [from, to] : edges) {
            sum += angle_between(points_[from], points_[to]);
        }
        return sum / static_cast<double>(edges.size());
    };
    const double reach = jitter * mean_spacing();
    for (std::uint32_t index = 0; index < size(); ++index) {
        DVec3& point = points_[index];
        const DVec3 across = std::abs(point.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{1, 0, 0};
        const DVec3 east = glm::normalize(glm::cross(across, point));
        const DVec3 north = glm::cross(point, east);
        const double turn = 2.0 * std::numbers::pi * unit(hash_values(seed, index, 0));
        const double distance = reach * unit(hash_values(seed, index, 1));
        point = glm::normalize(
            point + (distance * ((maths::cos(turn) * east) + (maths::sin(turn) * north))));
    }
    spacing_ = mean_spacing();

    touching_offsets_.assign(points_.size() + 1, 0);
    for (const Triangle& triangle : triangles_) {
        for (const std::uint32_t corner : triangle) {
            ++touching_offsets_[corner + 1];
        }
    }
    for (std::size_t n = 1; n < touching_offsets_.size(); ++n) {
        touching_offsets_[n] += touching_offsets_[n - 1];
    }
    touching_.resize(touching_offsets_.back());
    std::vector<std::uint32_t> filled(touching_offsets_.begin(), touching_offsets_.end() - 1);
    for (std::uint32_t index = 0; index < triangles_.size(); ++index) {
        for (const std::uint32_t corner : triangles_[index]) {
            touching_[filled[corner]++] = index;
        }
    }

    areas_.assign(points_.size(), 0.0);
    for (const auto& [a, b, c] : triangles_) {
        const DVec3& pa = points_[a];
        const DVec3& pb = points_[b];
        const DVec3& pc = points_[c];
        const double excess =
            2.0 * maths::atan2(std::abs(glm::dot(pa, glm::cross(pb, pc))),
                               1.0 + glm::dot(pa, pb) + glm::dot(pb, pc) + glm::dot(pc, pa));
        for (const std::uint32_t corner : {a, b, c}) {
            areas_[corner] += excess / 3.0;
        }
    }
}

std::uint32_t Sphere::nearest(const DVec3& direction, std::uint32_t start) const {
    std::uint32_t at = start < size() ? start : 0;
    double best = glm::dot(points_[at], direction);
    for (;;) {
        const std::uint32_t was = at;
        for (const std::uint32_t next : neighbors(was)) {
            const double closeness = glm::dot(points_[next], direction);
            if (closeness > best) {
                best = closeness;
                at = next;
            }
        }
        if (at == was) {
            return at;
        }
    }
}

Sphere::Spot Sphere::locate(const DVec3& direction, std::uint32_t start) const {
    // Signed volumes against each edge, each opposite its corner; the triangle holding the
    // direction has none negative.
    const auto volumes_in = [&](std::uint32_t triangle) {
        const DVec3& a = points_[triangles_[triangle][0]];
        const DVec3& b = points_[triangles_[triangle][1]];
        const DVec3& c = points_[triangles_[triangle][2]];
        return DVec3{glm::dot(direction, glm::cross(b, c)), glm::dot(direction, glm::cross(c, a)),
                     glm::dot(direction, glm::cross(a, b))};
    };
    const auto worst_of = [](const DVec3& volumes) {
        return std::min({volumes.x, volumes.y, volumes.z});
    };

    // The nearest point is usually a corner of the triangle holding the direction, not always,
    // so from the best triangle round it, walk over whichever edge the direction lies beyond.
    const std::uint32_t near = nearest(direction, start);
    std::uint32_t current = touching_[touching_offsets_[near]];
    for (std::uint32_t at = touching_offsets_[near] + 1; at < touching_offsets_[near + 1]; ++at) {
        if (worst_of(volumes_in(touching_[at])) > worst_of(volumes_in(current))) {
            current = touching_[at];
        }
    }
    DVec3 volumes = volumes_in(current);
    for (std::size_t step = 0; step < points_.size() && worst_of(volumes) < 0.0; ++step) {
        const glm::length_t beyond = smallest_of(volumes);
        const Triangle& triangle = triangles_[current];
        const std::uint32_t from = triangle[static_cast<std::size_t>((beyond + 1) % 3)];
        const std::uint32_t to = triangle[static_cast<std::size_t>((beyond + 2) % 3)];
        std::uint32_t across = current;
        for (std::uint32_t at = touching_offsets_[from]; at < touching_offsets_[from + 1]; ++at) {
            const Triangle& other = triangles_[touching_[at]];
            if (touching_[at] != current && std::ranges::find(other, to) != other.end()) {
                across = touching_[at];
            }
        }
        if (across == current) {
            break;
        }
        current = across;
        volumes = volumes_in(current);
    }
    // Rounding can leave a hair outside every triangle; such weights are clamped.
    const DVec3 kept = glm::max(volumes, DVec3{0.0});
    return {.corners = triangles_[current],
            .weights = kept / std::max(kept.x + kept.y + kept.z, 1e-300)};
}

} // namespace hog::formation
