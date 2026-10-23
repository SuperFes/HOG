#include "leaves.hpp"

#include <hog/core/assert.hpp>
#include <hog/core/profile.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/sea.hpp>
#include <hog/terrain/selection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::terrain {

namespace {

[[nodiscard]] double angle_between(DVec3 a, DVec3 b) {
    return std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
}

// As angle_between, though precise for small angles too.
[[nodiscard]] double small_angle_between(DVec3 a, DVec3 b) {
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

// Metres between points `a` and `b` metres from the centre, `angle` radians apart; free of the
// cancellation the law of cosines suffers when they're close.
[[nodiscard]] double apart(double a, double b, double angle) {
    const double half = std::sin(angle / 2.0);
    return std::sqrt(((a - b) * (a - b)) + (4.0 * a * b * half * half));
}

// Cells a thread takes at once; fewer than two chunks' worth aren't worth sharing.
constexpr std::uint32_t cells_per_chunk = 32;

// Calls body(range, thread) over [0, count) on the pool in chunks, or all here without one.
template <typename F>
void for_chunks(jobs::JobSystem* jobs, std::size_t count, std::uint32_t chunk, F body) {
    if (jobs == nullptr || count < 2 * std::size_t{chunk}) {
        body(jobs::Range{.begin = 0, .end = static_cast<std::uint32_t>(count)}, 0U);
        return;
    }
    jobs->parallel_for(static_cast<std::uint32_t>(count), chunk, std::move(body));
}

// A cone from the planet's centre, as drawn.
struct Cone {
    DVec3 direction; // unit
    double angle;    // radians from direction to its rim
    double gap;      // radians from the eye's direction to the nearest of it
};

// A sphere around everything a cell's patch can hold, as drawn, with the planet's centre on the
// origin.
struct Bounds {
    DVec3 from_eye; // metres from the eye to the sphere's centre
    double radius;  // metres
    double gap;     // radians from the eye's direction to the nearest of the cell
    double reach;   // and to the farthest
    double spacing; // metres between the patch's vertices, at most
    double lowest;  // metres from the centre the patch keeps between
    double highest;
};

class Selector {
public:
    Selector(const Viewpoint& view, double planet_radius, const SelectionSettings& settings,
             const KnownHeights& known)
        : settings_(settings), known_(&known), planet_radius_(planet_radius),
          drawn_radius_(view.bend.faked == view.bend.radius ? planet_radius : view.bend.faked),
          squeeze_(planet_radius / drawn_radius_), lowest_(drawn_radius_ + settings.lowest),
          highest_(drawn_radius_ + settings.highest),
          pixels_per_radian_(view.viewport_height / (2.0 * std::tan(view.vertical_fov / 2.0))) {
        HOG_ASSERT(lowest_ > 0.0 && highest_ > lowest_, "ground range out of order");
        HOG_ASSERT(view.bend.faked == view.bend.radius || view.bend.radius == planet_radius,
                   "bent from another planet");
        const double from_centre = glm::length(view.eye.metres());
        HOG_ASSERT(from_centre > 0.0, "eye at the planet's centre");
        eye_direction_ = view.eye.metres() / from_centre;
        // Bending keeps the eye's height over sea level.
        eye_height_ = drawn_radius_ + (from_centre - planet_radius);
        eye_ = eye_direction_ * eye_height_;
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
            eye_horizon_ = std::acos(lowest_ / eye_height_);
        }
    }

    // The cone a cell's ground, within `angle` of `middle`, is drawn in. Bending keeps each
    // point's bearing from the eye and squeezes its angle from it, so the cone is drawn within
    // the sector of those bearings and squeezed angles, which is farthest from its middle at its
    // corners.
    [[nodiscard]] Cone drawn(DVec3 middle, double angle) const {
        const double away = angle_between(eye_direction_, middle);
        const double gap = std::max(0.0, away - angle) * squeeze_;
        if (squeeze_ == 1.0) {
            return {.direction = middle, .angle = angle, .gap = gap};
        }
        const double farthest = std::min(away + angle, std::numbers::pi) * squeeze_;
        const Cone around_eye{.direction = eye_direction_, .angle = farthest, .gap = gap};
        // Around the eye, or around the far side, every bearing.
        if (away <= angle || away + angle >= std::numbers::pi) {
            return around_eye;
        }
        const double bearing = std::asin(std::min(1.0, std::sin(angle) / std::sin(away)));
        const DVec3 aside = glm::normalize(middle - (eye_direction_ * std::cos(away)));
        const DVec3 across = glm::cross(eye_direction_, aside);
        const auto at = [&](double from, DVec3 towards) {
            return (eye_direction_ * std::cos(from)) + (towards * std::sin(from));
        };
        const DVec3 direction = at(away * squeeze_, aside);
        const DVec3 cornerwise = (aside * std::cos(bearing)) + (across * std::sin(bearing));
        const double spread = std::max(small_angle_between(direction, at(gap, cornerwise)),
                                       small_angle_between(direction, at(farthest, cornerwise)));
        if (spread >= farthest) {
            return around_eye;
        }
        return {.direction = direction, .angle = spread, .gap = gap};
    }

    [[nodiscard]] Bounds bounds(planet::CellId cell) const {
        const DVec3 middle = planet::cell_center(cell);
        const std::array<DVec3, 4> corners = planet::cell_vertices(cell);
        // The farthest corner is the one least along the middle.
        double along_middle = 1.0;
        double edge = 0.0;
        for (std::size_t corner = 0; corner < corners.size(); ++corner) {
            along_middle = std::min(along_middle, glm::dot(middle, corners.at(corner)));
            edge = std::max(
                edge, glm::distance(corners.at(corner), corners.at((corner + 1) % corners.size())));
        }
        const double angle = std::acos(std::max(along_middle, -1.0));
        double highest = settings_.highest;
        double lowest = settings_.lowest;
        if (const auto heights = *known_ ? (*known_)(cell) : std::nullopt) {
            highest = heights->highest;
            lowest = heights->lowest;
        }
        const double spacing = edge * (planet_radius_ + highest) / patch_cells;
        // Triangles sag below their vertices towards the centre, most midway along a diagonal;
        // less so drawn on the flatter faked sphere.
        lowest -= spacing * spacing / (4.0 * planet_radius_);

        // Every point lies within the cone, between the lowest and highest ground, so its
        // distance from a point on the cone's axis peaks at one of those two radii.
        const Cone cone = drawn(middle, angle);
        const double low = drawn_radius_ + lowest;
        const double high = drawn_radius_ + highest;
        const double along = ((low * std::cos(cone.angle)) + high) / 2.0;
        return {.from_eye = (cone.direction * along) - eye_,
                .radius = std::max(apart(low, along, cone.angle), apart(high, along, cone.angle)),
                .gap = cone.gap,
                .reach = std::min(small_angle_between(eye_direction_, cone.direction) + cone.angle,
                                  std::numbers::pi),
                .spacing = spacing,
                .lowest = low,
                .highest = high};
    }

    // Picked: above the horizon, and in view unless all round the eye is wanted.
    [[nodiscard]] bool visible(const Bounds& bounds) const {
        return above_horizon(bounds) && (!settings_.only_in_view || in_frustum(bounds));
    }

    [[nodiscard]] bool in_view(const Bounds& bounds) const {
        return above_horizon(bounds) && in_frustum(bounds);
    }

    [[nodiscard]] bool in_frustum(const Bounds& bounds) const {
        return std::ranges::none_of(planes_, [&](const DVec3& plane) {
            return glm::dot(bounds.from_eye, plane) < -bounds.radius;
        });
    }

    [[nodiscard]] bool above_horizon(const Bounds& bounds) const {
        if (!eye_horizon_) {
            return true;
        }
        // Seen past the horizon of the lowest ground, as far as the patch's highest stands up.
        const double beyond = std::acos(std::min(1.0, lowest_ / bounds.highest));
        return bounds.gap <= *eye_horizon_ + beyond;
    }

    // The nearest the cell's ground can come to the eye: no closer in angle than its gap allows,
    // at whichever height between the lowest and highest ground is nearest.
    [[nodiscard]] double distance(const Bounds& bounds) const {
        const double nearest =
            std::clamp(eye_height_ * std::cos(bounds.gap), bounds.lowest, bounds.highest);
        return apart(nearest, eye_height_, bounds.gap);
    }

    [[nodiscard]] bool under_sea(const Bounds& bounds) const {
        return settings_.seabed_unseen > 0.0 &&
               seabed_showing(eye_height_, drawn_radius_, bounds.lowest, bounds.highest, bounds.gap,
                              bounds.reach) <= settings_.seabed_unseen;
    }

    [[nodiscard]] bool too_coarse(planet::CellId cell, const Bounds& bounds) const {
        if (cell.level() >= max_patch_level || bounds.spacing <= settings_.finest_spacing) {
            return false;
        }
        const double away = distance(bounds);
        return away <= 0.0 || bounds.spacing * pixels_per_radian_ / away > settings_.max_error;
    }

    // Splits from the six faces down, a level at a time, keeping patches culled or fine enough
    // as leaves, sorted.
    [[nodiscard]] std::vector<std::pair<planet::CellId, Bounds>>
    refine(jobs::JobSystem* jobs) const {
        HOG_ZONE("refine patches");
        std::vector<std::pair<planet::CellId, Bounds>> leaves;
        std::vector<planet::CellId> level;
        level.reserve(planet::face_count);
        for (int face = 0; face < planet::face_count; ++face) {
            level.push_back(planet::CellId::from_face(face));
        }
        std::vector<Bounds> around;
        std::vector<std::uint8_t> split; // not vector<bool>, written from several threads
        std::vector<planet::CellId> finer;
        while (!level.empty()) {
            around.resize(level.size());
            split.resize(level.size());
            for_chunks(jobs, level.size(), cells_per_chunk,
                       [&](jobs::Range range, std::uint32_t /*thread*/) {
                           for (std::uint32_t at = range.begin; at < range.end; ++at) {
                               around[at] = bounds(level[at]);
                               split[at] =
                                   visible(around[at]) && too_coarse(level[at], around[at]) ? 1 : 0;
                           }
                       });
            finer.clear();
            for (std::size_t at = 0; at < level.size(); ++at) {
                if (split[at] == 0) {
                    leaves.emplace_back(level[at], around[at]);
                    continue;
                }
                for (int child = 0; child < 4; ++child) {
                    finer.push_back(level[at].child(child));
                }
            }
            level.swap(finer);
        }
        std::ranges::sort(leaves, {}, &std::pair<planet::CellId, Bounds>::first);
        return leaves;
    }

private:
    SelectionSettings settings_;
    const KnownHeights* known_;
    double planet_radius_;
    double drawn_radius_;
    double squeeze_; // the planet's radius over the drawn one
    DVec3 eye_direction_{};
    double eye_height_ = 0.0; // metres from the drawn centre
    DVec3 eye_{};             // as drawn
    double lowest_;
    double highest_;
    double pixels_per_radian_;
    std::array<DVec3, 5> planes_{};
    std::optional<double> eye_horizon_;
};

// What a round of balancing found, a list a thread: leaves too coarse beside those checked, and
// those checked they were too coarse for.
struct Unbalanced {
    std::vector<std::vector<planet::CellId>> split;
    std::vector<std::vector<planet::CellId>> asking;
};

void find_unbalanced(std::span<const planet::CellId> leaves,
                     std::span<const planet::CellId> pending, jobs::JobSystem* jobs,
                     Unbalanced& found) {
    for_chunks(jobs, pending.size(), cells_per_chunk, [&](jobs::Range range, std::uint32_t thread) {
        for (std::uint32_t at = range.begin; at < range.end; ++at) {
            const planet::CellId cell = pending[at];
            bool asks = false;
            for (const planet::CellId next : planet::edge_neighbors(cell)) {
                const auto over = detail::leaf_over(leaves, next);
                if (over && over->level() < cell.level() - 1) {
                    found.split[thread].push_back(*over);
                    asks = true;
                }
            }
            if (asks) {
                found.asking[thread].push_back(cell);
            }
        }
    });
}

// Replaces the leaves to split, sorted, with their children, adding those to `made`.
void split_leaves(std::vector<planet::CellId>& leaves, std::span<const planet::CellId> split,
                  std::vector<planet::CellId>& made) {
    std::vector<planet::CellId> finer;
    finer.reserve(leaves.size() + (3 * split.size()));
    auto next_split = split.begin();
    for (const planet::CellId cell : leaves) {
        if (next_split == split.end() || *next_split != cell) {
            finer.push_back(cell);
            continue;
        }
        ++next_split;
        for (int child = 0; child < 4; ++child) {
            finer.push_back(cell.child(child));
            made.push_back(cell.child(child));
        }
    }
    leaves.swap(finer);
}

// Splits leaves, sorted by cell, until every pair sharing an edge differs by at most a level.
// The coarsest such splitting is the only one, so they're split in rounds, each checking only the
// leaves the last could have unsettled: those it made, and those beside that asked for it.
void balance(std::vector<planet::CellId>& leaves, jobs::JobSystem* jobs) {
    HOG_ZONE("balance patches");
    const std::size_t threads = jobs == nullptr ? 1 : jobs->thread_count();
    Unbalanced found{.split = std::vector<std::vector<planet::CellId>>(threads),
                     .asking = std::vector<std::vector<planet::CellId>>(threads)};
    std::vector<planet::CellId> pending = leaves;
    std::vector<planet::CellId> split;
    while (!pending.empty()) {
        find_unbalanced(leaves, pending, jobs, found);
        split.clear();
        for (std::vector<planet::CellId>& some : found.split) {
            split.insert(split.end(), some.begin(), some.end());
            some.clear();
        }
        std::ranges::sort(split);
        const auto [last, end] = std::ranges::unique(split);
        split.erase(last, end);

        pending.clear();
        split_leaves(leaves, split, pending);
        // The split child beside one asking may still be too coarse for it.
        for (std::vector<planet::CellId>& asking : found.asking) {
            std::ranges::copy_if(asking, std::back_inserter(pending), [&](planet::CellId cell) {
                return std::ranges::binary_search(leaves, cell);
            });
            asking.clear();
        }
    }
}

} // namespace

std::vector<SelectedPatch> select_patches(const Viewpoint& view, double planet_radius,
                                          const SelectionSettings& settings,
                                          const KnownHeights& known, jobs::JobSystem* jobs) {
    HOG_ZONE("select patches");
    const Selector selector{view, planet_radius, settings, known};
    const std::vector<std::pair<planet::CellId, Bounds>> refined = selector.refine(jobs);
    std::vector<planet::CellId> leaves;
    leaves.reserve(refined.size());
    for (const auto& [cell, around] : refined) {
        leaves.push_back(cell);
    }
    balance(leaves, jobs);

    // The bounds each leaf was refined with, found walking alongside as both are sorted, unless
    // balancing split it.
    std::vector<const Bounds*> unsplit(leaves.size(), nullptr);
    auto next_refined = refined.begin();
    for (std::size_t at = 0; at < leaves.size(); ++at) {
        while (next_refined != refined.end() && next_refined->first < leaves[at]) {
            ++next_refined;
        }
        if (next_refined != refined.end() && next_refined->first == leaves[at]) {
            unsplit[at] = &next_refined->second;
        }
    }
    std::vector<SelectedPatch> picked(leaves.size());
    std::vector<std::uint8_t> shown(leaves.size());
    for_chunks(
        jobs, leaves.size(), cells_per_chunk, [&](jobs::Range range, std::uint32_t /*thread*/) {
            for (std::uint32_t at = range.begin; at < range.end; ++at) {
                const planet::CellId cell = leaves[at];
                const Bounds around = unsplit[at] != nullptr ? *unsplit[at] : selector.bounds(cell);
                if (!selector.visible(around)) {
                    continue;
                }
                shown[at] = 1;
                picked[at] = {.cell = cell,
                              .coarser_edges = detail::coarser_edges(leaves, cell),
                              .distance = selector.distance(around),
                              .in_view = selector.in_view(around),
                              .under_sea = selector.under_sea(around)};
            }
        });
    std::vector<SelectedPatch> drawn;
    drawn.reserve(leaves.size());
    for (std::size_t at = 0; at < leaves.size(); ++at) {
        if (shown[at] != 0) {
            drawn.push_back(picked[at]);
        }
    }
    std::ranges::stable_sort(drawn, {}, &SelectedPatch::distance);
    return drawn;
}

} // namespace hog::terrain
