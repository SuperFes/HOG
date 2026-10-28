#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/rivers.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

// How far from a river's edge its banks may climb, at most: some widths across, and never
// beyond a third of its reach's length from its middle, which the cells it's found from always
// cover.
constexpr double bank_fade = 20.0;
constexpr double bank_reach = 1.0 / 3.0;
// A metre of rain a year runs off the rain-weighted area.
constexpr double seconds_per_year = 31'557'600.0;

double flow(double area) {
    return area / seconds_per_year;
}

std::int64_t fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

// The sea over landform cells, and the cells below the sea's level but cut off from it.
struct Standing {
    std::vector<bool> sea;
    std::vector<bool> sunken;
};

Standing find_standing(const Landform& landform) {
    const std::uint32_t count = landform.grid().size();
    Standing found{.sea = std::vector<bool>(count, false),
                   .sunken = std::vector<bool>(count, false)};
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (landform.heights()[cell] > 0) {
            continue;
        }
        // Only the sea's own cells drain nowhere.
        if (landform.drainage().receiver[cell] == cell) {
            found.sea[cell] = true;
        } else {
            found.sunken[cell] = true;
        }
    }
    return found;
}

constexpr std::uint32_t none = ~std::uint32_t{0};

// Per landform cell, the largest river flowing into it, or `none`.
template <typename Flows>
std::vector<std::uint32_t> main_donors(const LevelGrid& grid, const Drainage& drainage,
                                       std::span<const std::int64_t> drained, Flows flows) {
    const std::uint32_t count = grid.size();
    std::vector<std::uint32_t> upstream(count, none);
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        const std::uint32_t receiver = drainage.receiver[cell];
        const std::uint32_t largest = upstream[receiver];
        if (flows(cell) && (largest == none || drained[cell] > drained[largest] ||
                            (drained[cell] == drained[largest] && cell < largest))) {
            upstream[receiver] = cell;
        }
    }
    return upstream;
}

// Per cell a river flows from, the Hermite tangent every reach through it shares: from the
// largest river flowing in, or from the cell itself at a source, on to the cell it drains into.
template <typename Flows>
std::vector<DVec3> course_tangents(const LevelGrid& grid, const Drainage& drainage,
                                   std::span<const std::uint32_t> upstream, Flows flows) {
    const std::uint32_t count = grid.size();
    std::vector<DVec3> tangents(count, DVec3{0.0});
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (!flows(cell)) {
            continue;
        }
        const DVec3 after = cell_center(grid.cell(drainage.receiver[cell]));
        tangents[cell] = upstream[cell] == none
                             ? after - cell_center(grid.cell(cell))
                             : (after - cell_center(grid.cell(upstream[cell]))) * 0.5;
    }
    return tangents;
}

// A reach's course a share `t` of the way along, off the sphere by a hair, and how fast and which
// way it heads and turns there by the share.
struct Bend {
    DVec3 point;
    DVec3 heading;
    DVec3 turning;
};

Bend hermite(const Reach& reach, double t) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return {.point = (reach.from * ((2.0 * t3) - (3.0 * t2) + 1.0)) +
                     (reach.leaving * (t3 - (2.0 * t2) + t)) +
                     (reach.to * ((3.0 * t2) - (2.0 * t3))) + (reach.arriving * (t3 - t2)),
            .heading = ((reach.from - reach.to) * ((6.0 * t2) - (6.0 * t))) +
                       (reach.leaving * ((3.0 * t2) - (4.0 * t) + 1.0)) +
                       (reach.arriving * ((3.0 * t2) - (2.0 * t))),
            .turning = ((reach.from - reach.to) * ((12.0 * t) - 6.0)) +
                       (reach.leaving * ((6.0 * t) - 4.0)) + (reach.arriving * ((6.0 * t) - 2.0))};
}

// A smoothstep from 0 to 1, how fast it rises and how that changes, so what it blends starts and
// ends level.
struct Ease {
    double value;
    double rate;
    double bend;
};

Ease ease(double t) {
    return {
        .value = t * t * (3.0 - (2.0 * t)), .rate = 6.0 * t * (1.0 - t), .bend = 6.0 - (12.0 * t)};
}

constexpr std::uint64_t swing_salt = 0x7377'696E'6773'0000ULL; // "swings"
// Rivers meander between these shares of meander_widths and of swing_widths apart.
constexpr double wave_least = 0.8;
constexpr double wave_spread = 0.4;
constexpr double swing_least = 0.15;
// No further than this share of the way to the next cell, so the cells a reach is found from
// still take in its banks.
constexpr double swing_reach = 0.2;

// Pieces of a reach's river a meander either side of a point is found along.
constexpr int meander_pieces = 16;

// A share in [0, 1) drawn for a cell, a different one for each `which`.
double chance(std::uint64_t seed, std::uint32_t cell, std::int64_t which) {
    return fixed_to_double(lattice_value(seed, cell, which, 0));
}

// How far a reach's meanders swing it to the left of its course a share `t` along, how fast that
// changes by the share, and how that changes.
struct Meandering {
    double offset;
    double change;
    double bend;
};

Meandering meandering(const Reach& reach, double t) {
    const double phase =
        reach.phase + (reach.turn_from * t) + ((reach.turn_to - reach.turn_from) * t * t / 2.0);
    const double turning = reach.turn_from + ((reach.turn_to - reach.turn_from) * t);
    const double sine = std::sin(phase);
    const double cosine = std::cos(phase);
    const double widening = reach.swing_to - reach.swing_from;
    const Ease along = ease(t);
    const double swing = reach.swing_from + (widening * along.value);
    Meandering meanders{.offset = swing * sine,
                        .change = (widening * along.rate * sine) + (swing * turning * cosine),
                        .bend = (widening * along.bend * sine) +
                                (2.0 * widening * along.rate * turning * cosine) +
                                (swing * (reach.turn_to - reach.turn_from) * cosine) -
                                (swing * turning * turning * sine)};
    if (t > 1.0 - reach.settling) {
        const double settling = reach.settling;
        const Ease settled = ease((t - (1.0 - settling)) / settling);
        const double gap = reach.joining - meanders.offset;
        meanders.bend = (meanders.bend * (1.0 - settled.value)) -
                        (2.0 * meanders.change * settled.rate / settling) +
                        (gap * settled.bend / (settling * settling));
        meanders.change =
            (meanders.change * (1.0 - settled.value)) + (gap * settled.rate / settling);
        meanders.offset += gap * settled.value;
    }
    return meanders;
}

// A reach's river a share `t` along, swung about its course by its meanders, and about how fast
// and which way it runs and turns there by the share.
struct Swung {
    DVec3 point;
    DVec3 heading;
    DVec3 turning;
};

Swung swung(const Reach& reach, double t) {
    const Bend bend = hermite(reach, t);
    const double out = glm::length(bend.point);
    const DVec3 course = bend.point / out;
    const DVec3 side = glm::normalize(glm::cross(course, bend.heading));
    const Meandering meanders = meandering(reach, t);
    return {.point = glm::normalize(course + (side * meanders.offset)),
            .heading = (bend.heading / out) + (side * meanders.change),
            .turning = (bend.turning / out) + (side * meanders.bend)};
}

// How far a point is from a reach's river, swinging about its course, given how far along its
// course the point is nearest: from the nearest of the pieces of it a meander either side of
// there, which take in all its swings, nearer still.
double river_away(const Reach& reach, DVec3 point, double t) {
    const double turning = reach.turn_from + ((reach.turn_to - reach.turn_from) * t);
    const double span = 2.0 * std::numbers::pi / std::max(turning, 4.0 * std::numbers::pi);
    const double first = std::max(t - span, 0.0);
    const double last = std::min(t + span, 1.0);
    double nearest = std::numeric_limits<double>::infinity();
    double nearer = t;
    double before = first;
    DVec3 start = swung(reach, first).point;
    for (int piece = 1; piece <= meander_pieces; ++piece) {
        const double after = first + ((last - first) * piece / meander_pieces);
        const DVec3 end = swung(reach, after).point;
        const DVec3 along = end - start;
        const double share =
            std::clamp(glm::dot(point - start, along) / glm::dot(along, along), 0.0, 1.0);
        const double away = glm::length(point - (start + (along * share)));
        if (away < nearest) {
            nearest = away;
            nearer = before + ((after - before) * share);
        }
        before = after;
        start = end;
    }
    // Only ever nearer, so the distance is always to a point of the river.
    Swung river = swung(reach, nearer);
    double away = glm::length(river.point - point);
    for (int step = 0; step < 3; ++step) {
        const DVec3 off = river.point - point;
        const double rate = glm::dot(river.heading, river.heading) + glm::dot(off, river.turning);
        if (rate <= 0.0) {
            break;
        }
        const double next = std::clamp(nearer - (glm::dot(off, river.heading) / rate), first, last);
        const Swung there = swung(reach, next);
        const double further = glm::length(there.point - point);
        if (further >= away) {
            break;
        }
        nearer = next;
        river = there;
        away = further;
    }
    return away;
}

// Each cell's meanders: how fast their phase turns over the unit sphere, and how far they swing.
struct Waves {
    std::vector<double> wavenumber;
    std::vector<double> swing;
};

template <typename Flows>
Waves draw_waves(const Landform& landform, std::span<const std::int64_t> levels, Flows flows,
                 std::uint64_t seed, double radius, double scale) {
    const LevelGrid& grid = landform.grid();
    const Drainage& drainage = landform.drainage();
    const std::uint32_t count = grid.size();
    Waves waves{.wavenumber = std::vector<double>(count, 0.0),
                .swing = std::vector<double>(count, 0.0)};
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (!flows(cell)) {
            continue;
        }
        const DVec3 here = cell_center(grid.cell(cell));
        const std::uint32_t receiver = drainage.receiver[cell];
        const double wide =
            scale * Rivers::width(static_cast<double>(landform.drained()[cell])) / radius;
        waves.wavenumber[cell] =
            2.0 * std::numbers::pi /
            (Rivers::meander_widths * wide * (wave_least + (wave_spread * chance(seed, cell, 1))));
        const double run = glm::length(cell_center(grid.cell(receiver)) - here);
        const double steep = std::clamp(static_cast<double>(levels[cell] - levels[receiver]) /
                                            (Rivers::falls_gradient * run * radius * fixed_one),
                                        0.0, 1.0);
        double spacing = std::numeric_limits<double>::infinity();
        for (const std::uint32_t next : grid.neighbors(cell)) {
            spacing = std::min(spacing, glm::length(cell_center(grid.cell(next)) - here));
        }
        waves.swing[cell] = std::min(
            Rivers::swing_widths * wide *
                (swing_least + ((1.0 - swing_least) * chance(seed, cell, 2))) * (1.0 - steep),
            swing_reach * spacing);
    }
    return waves;
}

// The course of each cell's river: the Hermite tangents it shares with the reaches meeting it at
// either end, and its meanders, which carry on into the river downstream.
struct Course {
    DVec3 leaving{};
    DVec3 arriving{};
    double phase = 0.0;
    double turn_from = 0.0;
    double turn_to = 0.0;
    double swing_from = 0.0;
    double swing_to = 0.0;
    double joining = 0.0;
    double settling = 0.0;
};

template <typename Flows>
std::vector<Course> lay_courses(const Landform& landform, std::span<const std::int64_t> levels,
                                Flows flows, std::uint64_t seed, double radius, double scale) {
    const LevelGrid& grid = landform.grid();
    const Drainage& drainage = landform.drainage();
    const std::vector<std::uint32_t> upstream =
        main_donors(grid, drainage, landform.drained(), flows);
    const std::vector<DVec3> tangents = course_tangents(grid, drainage, upstream, flows);
    const Waves waves = draw_waves(landform, levels, flows, seed, radius, scale);
    std::vector<Course> courses(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (!flows(cell)) {
            continue;
        }
        const std::uint32_t receiver = drainage.receiver[cell];
        const bool carries_on = flows(receiver) && upstream[receiver] == cell;
        Course& course = courses[cell];
        course.leaving = tangents[cell];
        // Each river runs straight on into the water it ends in.
        course.arriving = flows(receiver)
                              ? tangents[receiver]
                              : cell_center(grid.cell(receiver)) - cell_center(grid.cell(cell));
        course.turn_from = waves.wavenumber[cell] * glm::length(course.leaving);
        course.turn_to =
            waves.wavenumber[carries_on ? receiver : cell] * glm::length(course.arriving);
        course.swing_from = waves.swing[cell];
        course.swing_to = carries_on ? waves.swing[receiver] : waves.swing[cell];
    }
    // Phases run on downstream from each source.
    for (auto cell = drainage.order.rbegin(); cell != drainage.order.rend(); ++cell) {
        const std::uint32_t receiver = drainage.receiver[*cell];
        if (!flows(*cell)) {
            continue;
        }
        Course& course = courses[*cell];
        if (upstream[*cell] == none) {
            course.phase = 2.0 * std::numbers::pi * chance(seed, *cell, 0);
        }
        if (flows(receiver) && upstream[receiver] == *cell) {
            courses[receiver].phase = course.phase + ((course.turn_from + course.turn_to) / 2.0);
        }
    }
    // A tributary's meanders settle onto the river it joins over half a meander, and longer the
    // further they have to go.
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const std::uint32_t receiver = drainage.receiver[cell];
        if (!flows(cell) || !flows(receiver) || upstream[receiver] == cell) {
            continue;
        }
        Course& course = courses[cell];
        const double run =
            glm::length(cell_center(grid.cell(receiver)) - cell_center(grid.cell(cell)));
        course.joining = waves.swing[receiver] * std::sin(courses[receiver].phase);
        course.settling = std::min(
            ((std::numbers::pi / waves.wavenumber[cell]) + (2.0 * std::abs(course.joining))) / run,
            1.0);
    }
    return courses;
}

} // namespace

DVec3 course_at(const Reach& reach, double t) {
    return glm::normalize(hermite(reach, t).point);
}

Swing swing_at(const Reach& reach, double t) {
    const Meandering meanders = meandering(reach, t);
    return {.offset = meanders.offset,
            .slope = meanders.change / glm::length(hermite(reach, t).heading)};
}

double Rivers::width(double area) {
    return 2.5 * std::sqrt(flow(area));
}

double Rivers::depth(double area) {
    return 0.5 * std::sqrt(std::sqrt(flow(area)));
}

Rivers::Rivers(const Landform& landform, std::span<const std::int64_t> ground, Meander meander,
               double radius, double scale)
    : landform_{&landform}, grid_{&landform.grid()}, meander_{meander}, radius_{radius} {
    const LevelGrid& grid = landform.grid();
    const Drainage& drainage = landform.drainage();
    const auto drained = landform.drained();
    const std::uint32_t count = grid.size();
    HOG_ASSERT(ground.size() == count, "one ground height per landform cell");

    const auto lakes = landform.lakes();
    const auto outlet = [&](std::uint32_t cell) { return drainage.receiver[cell] == cell; };
    const auto lake = [&](std::uint32_t cell) { return lakes[cell] != Landform::dry; };
    Standing standing = find_standing(landform);
    sea_ = std::move(standing.sea);
    sunken_ = std::move(standing.sunken);
    // Rivers stop at a lake's shore, and leave from its outlet; dry basins hold none.
    const auto flows = [&](std::uint32_t cell) {
        return !outlet(cell) && !landform.basin(cell) &&
               static_cast<double>(drained[cell]) >= source_area;
    };
    // A river's water stands at the ground of its cell's centre, but never above the water
    // upstream, so it only falls towards the sea and cuts through rises on the way.
    levels_.assign(ground.begin(), ground.end());
    for (auto cell = drainage.order.rbegin(); cell != drainage.order.rend(); ++cell) {
        const std::uint32_t receiver = drainage.receiver[*cell];
        if (flows(*cell) && flows(receiver)) {
            levels_[receiver] = std::min(levels_[receiver], levels_[*cell]);
        }
    }
    // Lakes are level. Rivers stand no lower than the lake they run into, and ground away from
    // rivers drains into the water downstream: a river, a lake or the sea, or the lowest ground
    // of a world without one.
    for (const std::uint32_t cell : drainage.order) {
        const std::uint32_t receiver = drainage.receiver[cell];
        if (outlet(cell)) {
            levels_[cell] = std::max<std::int64_t>(ground[cell], 0);
        } else if (lake(cell)) {
            levels_[cell] = lakes[cell];
        } else if (flows(cell)) {
            levels_[cell] = std::max(levels_[cell], levels_[receiver]);
        } else {
            levels_[cell] = levels_[receiver];
        }
    }

    const std::vector<Course> courses = lay_courses(
        landform, levels_, flows, hash_combine(meander.seed(), swing_salt), radius, scale);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> near;
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (!flows(cell)) {
            continue;
        }
        const std::uint32_t receiver = drainage.receiver[cell];
        const auto area = static_cast<double>(drained[cell]);
        const auto reach = static_cast<std::uint32_t>(reaches_.size());
        const DVec3 from = cell_center(grid.cell(cell));
        const DVec3 to = cell_center(grid.cell(receiver));
        const std::int64_t drop = levels_[cell] - levels_[receiver];
        const bool falls = static_cast<double>(drop) >=
                           falls_gradient * glm::length(to - from) * radius * fixed_one;
        const Course& course = courses[cell];
        reaches_.push_back({.from = from,
                            .to = to,
                            .from_surface = levels_[cell],
                            .to_surface = levels_[receiver],
                            .width = scale * width(area),
                            .depth = fixed(scale * depth(area)),
                            .fall = falls ? drop : 0,
                            .leaving = course.leaving,
                            .arriving = course.arriving,
                            .flow = flow(area),
                            .phase = course.phase,
                            .turn_from = course.turn_from,
                            .turn_to = course.turn_to,
                            .swing_from = course.swing_from,
                            .swing_to = course.swing_to,
                            .joining = course.joining,
                            .settling = course.settling});
        for (const std::uint32_t end : {cell, receiver}) {
            near.emplace_back(end, reach);
            for (const std::uint32_t next : grid.neighbors(end)) {
                near.emplace_back(next, reach);
            }
        }
    }
    std::ranges::sort(near);
    const auto [last, end] = std::ranges::unique(near);
    near.erase(last, end);
    first_.assign(count + 1, 0);
    nearby_.reserve(near.size());
    for (const auto& [cell, reach] : near) {
        ++first_[cell + 1];
        nearby_.push_back(reach);
    }
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        first_[cell + 1] += first_[cell];
    }
}

template <typename Reaching, typename Visit>
void Rivers::each_reach(CellId cell, Reaching reaching, Visit visit) const {
    // The reaches are found from the shifted point, which lies on the river's line.
    const DVec3 point = meander_(cell_center(cell));
    const std::uint32_t here = grid_->index(cell_at(point, grid_->level()));
    for (std::uint32_t k = first_[here]; k < first_[here + 1]; ++k) {
        const Reach& reach = reaches_[nearby_[k]];
        // A course keeps within the hull of its ends and the points a third of its tangents on.
        const DVec3 middle = (reach.from + reach.to) * 0.5;
        const double bound = std::max({glm::length(reach.from - middle),
                                       glm::length(reach.from + (reach.leaving / 3.0) - middle),
                                       glm::length(reach.to - (reach.arriving / 3.0) - middle)}) +
                             std::max({reach.swing_from, reach.swing_to, std::abs(reach.joining)});
        if (glm::length(point - middle) - bound > reaching(reach) / radius_) {
            continue;
        }
        // Nearest the pieces of the course, then the course itself nearer that.
        double nearest = std::numeric_limits<double>::infinity();
        double t = 0.0;
        DVec3 start = reach.from;
        for (int piece = 0; piece < course_pieces; ++piece) {
            const DVec3 end =
                piece + 1 == course_pieces
                    ? reach.to
                    : course_at(reach, static_cast<double>(piece + 1) / course_pieces);
            const DVec3 along = end - start;
            const double share =
                std::clamp(glm::dot(point - start, along) / glm::dot(along, along), 0.0, 1.0);
            const double away = glm::length(point - (start + (along * share)));
            if (away < nearest) {
                nearest = away;
                t = (piece + share) / course_pieces;
            }
            start = end;
        }
        for (int step = 0; step < 2; ++step) {
            const Bend bend = hermite(reach, t);
            const DVec3 off = bend.point - point;
            const double slope = glm::dot(bend.heading, bend.heading) + glm::dot(off, bend.turning);
            if (slope <= 0.0) {
                break;
            }
            t = std::clamp(t - (glm::dot(off, bend.heading) / slope), 0.0, 1.0);
        }
        // Its water stands as far down as the course runs, wherever its meanders take it.
        nearest = river_away(reach, point, t);
        const std::int64_t level =
            reach.from_surface +
            std::llround(static_cast<double>(reach.to_surface - reach.from_surface) * t);
        visit(reach, level, nearest * radius_);
    }
}

double Rivers::banks(const Reach& reach) const {
    // Reaches are only found from the cells about their ends, so the banks must climb back to
    // the ground well within them.
    return std::max(std::min(bank_fade * reach.width,
                             bank_reach * glm::length(reach.to - reach.from) * radius_) -
                        (reach.width / 2.0),
                    0.0);
}

std::int64_t Rivers::sunk(const Reach& reach, std::int64_t level, std::int64_t ground) const {
    // Where the ground stands higher than the banks can climb, so does the river in it.
    const std::int64_t deepest =
        std::llround(banks(reach) * bank_slope * static_cast<double>(fixed_one));
    // Where the ground dips below the river's line, the water follows it down.
    return std::max(std::min(level, ground), ground - deepest);
}

std::int64_t Rivers::cut(CellId cell, std::int64_t ground) const {
    std::int64_t lowest = ground;
    // Beyond its banks, a river's cut climbs above the ground.
    const auto reaching = [&](const Reach& reach) { return (reach.width / 2.0) + banks(reach); };
    each_reach(cell, reaching, [&](const Reach& reach, std::int64_t level, double away) {
        const std::int64_t surface = sunk(reach, level, ground);
        const double half = reach.width / 2.0;
        std::int64_t bed = 0;
        if (away < half) {
            const double share = 1.0 - ((away / half) * (away / half));
            bed = surface -
                  std::max<std::int64_t>(std::llround(static_cast<double>(reach.depth) * share), 1);
        } else {
            bed =
                surface + std::llround((away - half) * bank_slope * static_cast<double>(fixed_one));
        }
        lowest = std::min(lowest, bed);
    });
    return lowest;
}

std::optional<Channel> Rivers::channel(CellId cell, std::int64_t ground) const {
    std::optional<std::int64_t> surface;
    const auto reaching = [](const Reach& reach) { return reach.width / 2.0; };
    each_reach(cell, reaching, [&](const Reach& reach, std::int64_t level, double away) {
        if (away < reach.width / 2.0) {
            const std::int64_t water = sunk(reach, level, ground);
            surface = std::min(surface.value_or(water), water);
        }
    });
    if (!surface) {
        return std::nullopt;
    }
    return Channel{.bed = cut(cell, ground), .surface = *surface};
}

bool Rivers::in_channel(CellId cell) const {
    bool found = false;
    const auto reaching = [](const Reach& reach) { return reach.width / 2.0; };
    each_reach(cell, reaching, [&](const Reach& reach, std::int64_t /*level*/, double away) {
        found = found || away < reach.width / 2.0;
    });
    return found;
}

std::optional<StandingWater> Rivers::standing(CellId cell, std::int64_t ground) const {
    // Found about the shifted point, so shores follow the ground, not the grid.
    const CellId shifted = cell_at(meander_(cell_center(cell)));
    // A lake fills the ground under its level wherever its cells are blended into it; the cells
    // round it stand above its level, so the water meets the ground all along its shore.
    std::int64_t lake = Landform::dry;
    for (const std::uint32_t corner : grid_->corners(shifted)) {
        lake = std::max(lake, landform_->lakes()[corner]);
    }
    if (lake != Landform::dry) {
        if (ground < lake) {
            return StandingWater{.level = lake, .sea = false};
        }
        return std::nullopt;
    }
    // The sea stands over its own cells and those beside them, as low as the ground lies.
    if (ground >= 0) {
        return std::nullopt;
    }
    const std::uint32_t here = grid_->index(shifted.parent(grid_->level()));
    bool sea = sea_[here];
    bool sunken = sunken_[here];
    for (const std::uint32_t around : grid_->neighbors(here)) {
        sea = sea || sea_[around];
        sunken = sunken || sunken_[around];
    }
    // Low ground finer than the grid takes the sea, unless it lies by ground cut off from it.
    if (sea || !sunken) {
        return StandingWater{.level = 0, .sea = true};
    }
    return std::nullopt;
}

std::optional<std::int64_t> Rivers::lake(CellId cell, std::int64_t ground) const {
    const auto water = standing(cell, ground);
    if (!water || water->sea) {
        return std::nullopt;
    }
    return water->level;
}

const Reach* Rivers::near(CellId cell, double margin) const {
    const Reach* widest = nullptr;
    const auto reaching = [&](const Reach& reach) { return (reach.width / 2.0) + margin; };
    each_reach(cell, reaching, [&](const Reach& reach, std::int64_t /*level*/, double away) {
        if (away < (reach.width / 2.0) + margin &&
            (widest == nullptr || reach.width > widest->width)) {
            widest = &reach;
        }
    });
    return widest;
}

} // namespace hog::planet
