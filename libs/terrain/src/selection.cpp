#include <hog/core/assert.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/selection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>
#include <set>
#include <vector>

namespace hog::terrain {

namespace {

[[nodiscard]] double angle_between(DVec3 a, DVec3 b) {
    return std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
}

// A sphere around everything a cell's patch can hold, with the cone it lies in.
struct Bounds {
    DVec3 centre;    // metres from the planet's centre
    double radius;   // metres
    DVec3 direction; // unit, the cell's centre
    double angle;    // radians from direction to the cell's farthest corner
    double spacing;  // metres between the patch's vertices, at most
};

class Selector {
public:
    Selector(const Viewpoint& view, double planet_radius, const SelectionSettings& settings)
        : settings_(settings), eye_(view.eye.metres()), eye_height_(glm::length(eye_)),
          lowest_(planet_radius + settings.lowest), highest_(planet_radius + settings.highest),
          pixels_per_radian_(view.viewport_height / (2.0 * std::tan(view.vertical_fov / 2.0))) {
        HOG_ASSERT(lowest_ > 0.0 && highest_ > lowest_, "ground range out of order");
        HOG_ASSERT(eye_height_ > 0.0, "eye at the planet's centre");
        eye_direction_ = eye_ / eye_height_;
        const DVec3 right = glm::normalize(glm::cross(view.forward, view.up));
        const DVec3 up = glm::cross(right, view.forward);
        const double half_height = view.vertical_fov / 2.0;
        const double half_width = std::atan(view.aspect * std::tan(half_height));
        // Inward normals, all through the eye; nothing is far enough to need a far plane.
        planes_ = {view.forward,
                   (view.forward * std::sin(half_height)) - (up * std::cos(half_height)),
                   (view.forward * std::sin(half_height)) + (up * std::cos(half_height)),
                   (view.forward * std::sin(half_width)) - (right * std::cos(half_width)),
                   (view.forward * std::sin(half_width)) + (right * std::cos(half_width))};
        // Ground beyond the horizon of the lowest ground can't be seen, from above it.
        if (eye_height_ > lowest_) {
            horizon_ = std::acos(lowest_ / eye_height_) + std::acos(lowest_ / highest_);
        }
    }

    // Radians from the eye's direction to the nearest of the cell's cone.
    [[nodiscard]] double gap(const Bounds& bounds) const {
        return std::max(0.0, angle_between(eye_direction_, bounds.direction) - bounds.angle);
    }

    [[nodiscard]] Bounds bounds(planet::CellId cell) const {
        const DVec3 middle = planet::cell_center(cell);
        const std::array<DVec3, 4> corners = planet::cell_vertices(cell);
        double angle = 0.0;
        double edge = 0.0;
        for (std::size_t corner = 0; corner < corners.size(); ++corner) {
            angle = std::max(angle, angle_between(middle, corners.at(corner)));
            edge = std::max(
                edge, glm::distance(corners.at(corner), corners.at((corner + 1) % corners.size())));
        }
        // Every point lies within `angle` of the middle, between the lowest and highest ground,
        // so its distance from a point on the middle's ray peaks at one of those two radii.
        const double cos_angle = std::cos(angle);
        const double along = ((lowest_ * cos_angle) + highest_) / 2.0;
        const auto reach = [&](double radius) {
            return std::sqrt(std::max(0.0, (radius * radius) + (along * along) -
                                               (2.0 * radius * along * cos_angle)));
        };
        return {.centre = middle * along,
                .radius = std::max(reach(lowest_), reach(highest_)),
                .direction = middle,
                .angle = angle,
                .spacing = edge * highest_ / patch_cells};
    }

    [[nodiscard]] bool visible(const Bounds& bounds) const {
        const DVec3 offset = bounds.centre - eye_;
        for (const DVec3& plane : planes_) {
            if (glm::dot(offset, plane) < -bounds.radius) {
                return false;
            }
        }
        return !horizon_ || gap(bounds) <= *horizon_;
    }

    // The nearest the cell's ground can come to the eye: no closer in angle than its cone allows,
    // at whichever height between the lowest and highest ground is nearest.
    [[nodiscard]] double distance(const Bounds& bounds) const {
        const double cos_gap = std::cos(gap(bounds));
        const double nearest = std::clamp(eye_height_ * cos_gap, lowest_, highest_);
        return std::sqrt(std::max(0.0, (nearest * nearest) + (eye_height_ * eye_height_) -
                                           (2.0 * nearest * eye_height_ * cos_gap)));
    }

    [[nodiscard]] bool too_coarse(planet::CellId cell, const Bounds& bounds) const {
        if (cell.level() >= max_patch_level || bounds.spacing <= settings_.finest_spacing) {
            return false;
        }
        const double away = distance(bounds);
        return away <= 0.0 || bounds.spacing * pixels_per_radian_ / away > settings_.max_error;
    }

    // Splits from the six faces down, keeping patches culled or fine enough as leaves.
    [[nodiscard]] std::set<planet::CellId> refine() const {
        std::set<planet::CellId> leaves;
        std::vector<planet::CellId> pending;
        pending.reserve(planet::face_count);
        for (int face = 0; face < planet::face_count; ++face) {
            pending.push_back(planet::CellId::from_face(face));
        }
        while (!pending.empty()) {
            const planet::CellId cell = pending.back();
            pending.pop_back();
            const Bounds around = bounds(cell);
            if (!visible(around) || !too_coarse(cell, around)) {
                leaves.insert(cell);
                continue;
            }
            for (int child = 0; child < 4; ++child) {
                pending.push_back(cell.child(child));
            }
        }
        return leaves;
    }

private:
    SelectionSettings settings_;
    DVec3 eye_;
    double eye_height_;
    DVec3 eye_direction_{};
    double lowest_;
    double highest_;
    double pixels_per_radian_;
    std::array<DVec3, 5> planes_{};
    std::optional<double> horizon_;
};

// The leaf covering all of `cell`, if one does rather than several finer ones.
[[nodiscard]] std::optional<planet::CellId> leaf_over(const std::set<planet::CellId>& leaves,
                                                      planet::CellId cell) {
    // Leaves don't overlap, so the one covering `cell` sorts next to it.
    const auto after = leaves.lower_bound(cell);
    if (after != leaves.end() && after->contains(cell)) {
        return *after;
    }
    if (after != leaves.begin() && std::prev(after)->contains(cell)) {
        return *std::prev(after);
    }
    return std::nullopt;
}

// Splits leaves until every pair sharing an edge differs by at most a level.
void balance(std::set<planet::CellId>& leaves) {
    std::vector<planet::CellId> pending(leaves.begin(), leaves.end());
    while (!pending.empty()) {
        const planet::CellId cell = pending.back();
        pending.pop_back();
        if (!leaves.contains(cell)) {
            continue;
        }
        for (const planet::CellId next : planet::edge_neighbors(cell)) {
            const auto over = leaf_over(leaves, next);
            if (!over || over->level() >= cell.level() - 1) {
                continue;
            }
            leaves.erase(*over);
            for (int child = 0; child < 4; ++child) {
                leaves.insert(over->child(child));
                pending.push_back(over->child(child));
            }
            // The split child beside it may still be too coarse.
            pending.push_back(cell);
            break;
        }
    }
}

} // namespace

std::vector<SelectedPatch> select_patches(const Viewpoint& view, double planet_radius,
                                          const SelectionSettings& settings) {
    const Selector selector{view, planet_radius, settings};
    std::set<planet::CellId> leaves = selector.refine();
    balance(leaves);

    std::vector<SelectedPatch> drawn;
    for (const planet::CellId cell : leaves) {
        const Bounds around = selector.bounds(cell);
        if (!selector.visible(around)) {
            continue;
        }
        std::uint8_t coarser = 0;
        const auto neighbours = planet::edge_neighbors(cell);
        for (std::size_t edge = 0; edge < neighbours.size(); ++edge) {
            const auto over = leaf_over(leaves, neighbours.at(edge));
            if (over && over->level() < cell.level()) {
                coarser |= static_cast<std::uint8_t>(1U << edge);
            }
        }
        drawn.push_back(
            {.cell = cell, .coarser_edges = coarser, .distance = selector.distance(around)});
    }
    std::ranges::stable_sort(drawn, {}, &SelectedPatch::distance);
    return drawn;
}

} // namespace hog::terrain
