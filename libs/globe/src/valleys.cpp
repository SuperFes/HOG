#include "valleys.hpp"

#include <hog/core/portable_maths.hpp>
#include <hog/globe/noise.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace hog::globe {

namespace {

double smoothstep(double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// How far from its line its channel may reach: the noise winding it swings up to twice the
// meander either way.
double channel_reach(const Valley& valley, double toe) noexcept {
    return std::max(valley.belt + toe, (2.0 * valley.meander) + (valley.channel_width / 2.0));
}

double smoothstep_slope(double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return 6.0 * t * (1.0 - t);
}

// The channel's cut `lateral` metres to one side of the line's great circle, `run` metres down
// the whole valley where it's `depth` deep: how deep, and how that changes per metre across and
// per metre down the valley, as it winds and deepens.
struct ChannelCut {
    double cut = 0.0;
    double across = 0.0;
    double along = 0.0;
};

ChannelCut channel_cut(const Valley& valley, double lateral, double run, double depth) noexcept {
    const double head_end = run / valley.meander_length;
    const double foot_end = (valley.length - run) / valley.meander_length;
    const double ends = smoothstep(head_end) * smoothstep(foot_end);
    const double ends_slope = ((smoothstep_slope(head_end) * smoothstep(foot_end)) -
                               (smoothstep(head_end) * smoothstep_slope(foot_end))) /
                              valley.meander_length;
    const Octaves bends{
        .wavelength = valley.meander_length, .gain = 0.5, .finest = valley.meander_length / 4.0};
    const Noise bending = fbm(DVec3{run, 0.0, 0.0}, bends, 0.0, valley.seed);
    const double wound = valley.meander * ends * bending.value;
    const double winding =
        valley.meander * ((ends_slope * bending.value) + (ends * bending.gradient.x));
    const double across = 2.0 * (lateral - wound) / valley.channel_width;
    if (std::abs(across) >= 1.0) {
        return {};
    }
    const double deepening =
        (valley.channel_depth_to - valley.channel_depth) / std::max(valley.length, 1e-300);
    return {.cut = depth * (1.0 - (across * across)),
            .across = -depth * 4.0 * across / valley.channel_width,
            .along = (deepening * (1.0 - (across * across))) +
                     (4.0 * depth * across / valley.channel_width * winding)};
}

} // namespace

double rise_past(double away, double reach) noexcept {
    const double half = reach / 2.0;
    return out_of_the_way * smoothstep((away - half) / half);
}

Wall wall_at(const Valley& valley, DVec3 direction, double radius) noexcept {
    // Where along the line it's nearest, across its chord: lines are short against the planet.
    const DVec3 along = valley.to - valley.from;
    const double length = glm::dot(along, along);
    const double t = length > 0.0
                         ? std::clamp(glm::dot(direction - valley.from, along) / length, 0.0, 1.0)
                         : 0.0;
    const DVec3 nearest = glm::normalize(valley.from + (along * t));
    const double cosine = std::clamp(glm::dot(direction, nearest), -1.0, 1.0);
    const double away = maths::atan2(glm::length(glm::cross(direction, nearest)), cosine) * radius;
    const double floor = valley.floor_from + ((valley.floor_to - valley.floor_from) * t);

    const double half = valley.reach / 2.0;
    const double past = std::clamp((away - half) / half, 0.0, 1.0);
    const double rise = rise_past(away, valley.reach);
    const double steepening = out_of_the_way * 6.0 * past * (1.0 - past) / half;

    // Up the walls from the belt's edge, their foot rounded over half the belt where it's worn
    // down to the floor (or the channel's width, where the belt is narrower), never past the
    // middle of the floor, which lies level across.
    const double out = away - valley.belt;
    const double toe = std::min(std::max(0.5 * valley.belt, valley.channel_width), valley.belt);
    double climbed = 0.0;
    double climbing = 0.0;
    if (toe <= 0.0 || out >= toe) {
        climbed = valley.wall * std::max(out, 0.0);
        climbing = out > 0.0 ? valley.wall : 0.0;
    } else if (out > -toe) {
        climbed = valley.wall * (out + toe) * (out + toe) / (4.0 * toe);
        climbing = valley.wall * (out + toe) / (2.0 * toe);
    }

    // The river winds along the floor, but runs into the next valley's where they meet.
    // Its channel winds about the line's great circle, carried on past the ends, so it runs on
    // smoothly beside them.
    const DVec3 square = glm::cross(valley.from, valley.to);
    const double square_size = glm::length(square);
    const DVec3 normal = square_size > 0.0 ? square / square_size : DVec3{0.0};
    const double sine = std::clamp(glm::dot(direction, normal), -1.0, 1.0);
    const double lateral = maths::asin(sine) * radius;
    const DVec3 sideways =
        (normal - (direction * sine)) / std::max(std::sqrt(1.0 - (sine * sine)), 1e-12);
    const double whole = valley.head + ((valley.foot - valley.head) * t);
    const double run = whole * valley.length;
    const double depth =
        valley.channel_depth + ((valley.channel_depth_to - valley.channel_depth) * whole);
    const double water_depth =
        valley.water_depth + ((valley.water_depth_to - valley.water_depth) * whole);
    const ChannelCut channel = valley.channel_width > 0.0 && away < channel_reach(valley, toe)
                                   ? channel_cut(valley, lateral, run, depth)
                                   : ChannelCut{};
    const double cut = channel.cut;
    const double cutting = channel.across;
    // How the cut changes along the line between its ends, where it runs on, per metre.
    DVec3 cut_along{0.0};
    if (channel.along != 0.0 && t > 0.0 && t < 1.0) {
        const DVec3 chord = along - (direction * glm::dot(along, direction));
        cut_along = chord * ((valley.foot - valley.head) * valley.length / (length * radius) *
                             channel.along);
    }

    const double standing = floor + climbed - cut;
    // Steeper or gentler as the walls vary along it.
    const DVec3 varying =
        valley.wall > 0.0 ? valley.wall_gradient * (climbed / valley.wall) : DVec3{0.0};
    Wall wall{.height = standing + rise,
              .gradient = DVec3{0.0},
              .floor = floor - cut,
              .water = std::max(floor - depth + water_depth, valley.least_water),
              .bank = floor,
              .season = {.rise = valley.rise + ((valley.rise_to - valley.rise) * whole),
                         .peak = valley.peak},
              .fall = DVec3{0.0},
              .reaching = 1.0 - (past * past * (3.0 - (2.0 * past))),
              .steep = valley.wall > 0.0 ? climbing / valley.wall : 0.0,
              .standing = standing,
              .standing_gradient = DVec3{0.0},
              .reaching_gradient = DVec3{0.0},
              .cut = cut,
              .cut_gradient = DVec3{0.0}};
    // Up the walls away from the line, and along its floor between its ends.
    const DVec3 outward = direction - (nearest * cosine);
    if (const double size = glm::length(outward); size > 0.0) {
        const DVec3 unit = outward / size;
        wall.fall += unit * climbing;
        wall.standing_gradient += unit * climbing;
        wall.gradient += unit * (climbing + steepening);
        wall.reaching_gradient = unit * (-steepening / out_of_the_way);
    }
    const DVec3 cut_gradient = (sideways * cutting) + cut_along;
    wall.standing_gradient += varying - cut_gradient;
    wall.gradient += varying - cut_gradient;
    wall.cut_gradient = cut_gradient;
    // The way the floor runs, turning smoothly from one bend to the next.
    DVec3 runs = along;
    if (glm::dot(valley.runs_from, valley.runs_from) > 0.0) {
        runs = (valley.runs_from * (1.0 - t)) + (valley.runs_to * t);
        runs *= std::sqrt(length) / std::max(glm::length(runs), 1e-300);
    }
    const double span = std::sqrt(length) * radius;
    // The ground follows the floor along the line between its ends, level past them.
    const DVec3 chord = along - (direction * glm::dot(along, direction));
    if (const double size = glm::length(chord); size > 0.0 && t > 0.0 && t < 1.0) {
        const DVec3 falling = chord / size * ((valley.floor_to - valley.floor_from) / span);
        wall.gradient += falling;
        wall.standing_gradient += falling;
    }
    const DVec3 ahead = runs - (direction * glm::dot(runs, direction));
    const bool inside = (t > 0.0 || valley.head > 0.0) && (t < 1.0 || valley.foot < 1.0);
    if (const double size = glm::length(ahead); size > 0.0 && inside) {
        const DVec3 falling = ahead / size * ((valley.floor_to - valley.floor_from) / span);
        // The floor stops falling past the valley's ends; water running off turns there
        // smoothly.
        constexpr double turning = 0.1;
        const double end = std::min(whole, 1.0 - whole) / turning;
        const double kept = end >= 1.0 ? 1.0 : end * end * (3.0 - (2.0 * end));
        wall.fall += falling * kept;
    }
    return wall;
}

WallBounds wall_bounds(const Valley& valley, DVec3 direction, double within, double radius,
                       Interval steepness) noexcept {
    // Along the line no further than its chord moves over the cap, and away from it no further
    // than the cap reaches and the nearest point on the line moves with it.
    const DVec3 along = valley.to - valley.from;
    const double length = glm::dot(along, along);
    const double middle = length > 0.0 ? glm::dot(direction - valley.from, along) / length : 0.0;
    const double moves = length > 0.0 ? within / std::sqrt(length) : 0.0;
    const Interval t{.low = std::clamp(middle - moves, 0.0, 1.0),
                     .high = std::clamp(middle + moves, 0.0, 1.0)};
    const DVec3 nearest = glm::normalize(valley.from + (along * std::clamp(middle, 0.0, 1.0)));
    const double here = maths::atan2(glm::length(glm::cross(direction, nearest)),
                                     std::clamp(glm::dot(direction, nearest), -1.0, 1.0)) *
                        radius;
    // Rounding aside.
    constexpr double slack = 1e-6;
    const Interval away{.low = std::max(0.0, here - (2.0 * within * radius) - slack),
                        .high = here + (2.0 * within * radius) + slack};
    const auto floor_at = [&](double share) {
        return valley.floor_from + ((valley.floor_to - valley.floor_from) * share);
    };
    const Interval floor{.low = std::min(floor_at(t.low), floor_at(t.high)),
                         .high = std::max(floor_at(t.low), floor_at(t.high))};
    // Up the walls, rising with the distance and the steepness both.
    const double toe = std::min(std::max(0.5 * valley.belt, valley.channel_width), valley.belt);
    const auto climbed = [&](double wall, double distance) {
        const double out = distance - valley.belt;
        if (toe <= 0.0 || out >= toe) {
            return wall * std::max(out, 0.0);
        }
        return out > -toe ? wall * (out + toe) * (out + toe) / (4.0 * toe) : 0.0;
    };
    const Interval climbing{.low = climbed(std::max(steepness.low, 0.0), away.low),
                            .high = climbed(std::max(steepness.high, 0.0), away.high)};
    // The channel, wherever it may wind across the belt.
    double deepest = 0.0;
    if (valley.channel_width > 0.0 && away.low < channel_reach(valley, toe)) {
        const auto depth_at = [&](double share) {
            const double whole = valley.head + ((valley.foot - valley.head) * share);
            return valley.channel_depth +
                   ((valley.channel_depth_to - valley.channel_depth) * whole);
        };
        deepest = std::max({0.0, depth_at(t.low), depth_at(t.high)});
    }
    const Interval standing{.low = floor.low + climbing.low - deepest,
                            .high = floor.high + climbing.high};
    return {.standing = standing,
            .height = standing + Interval{.low = rise_past(away.low, valley.reach),
                                          .high = rise_past(away.high, valley.reach)}};
}

void Lowered::add(const formation::Smooth::Read& ground, double wall, double floor) noexcept {
    if (count_ >= grounds_.size()) {
        return;
    }
    grounds_[count_] = {.read = ground, .wall = wall, .floor = floor};
    if (count_ == 0 || ground.value < grounds_[lowest_].read.value) {
        lowest_ = count_;
    }
    ++count_;
}

formation::Smooth::Read Lowered::rounded(const Ground& low) const noexcept {
    const double sinking = std::max(0.0, low.read.value - floor_under(low)) / 2.0;
    thread_local std::vector<std::pair<double, DVec3>> drops;
    drops.clear();
    double deepest = 0.0;
    for (const Ground& one : std::span<const Ground>{grounds_.data(), count_}) {
        // Of two standing level, only one is rounded by the other.
        const double apart = one.read.value - low.read.value;
        if (apart < 0.0 || (apart == 0.0 && &one <= &low)) {
            continue;
        }
        const double width =
            std::min(blend_, rounding_ * glm::length(one.read.gradient - low.read.gradient)) *
            one.wall * low.wall;
        if (width <= 0.0 || apart >= width) {
            continue;
        }
        const double share = (width - apart) / width;
        const double rounded = share * share * width / 4.0;
        // How the drop changes: held halfway down to the floor, it's half the ground's.
        drops.emplace_back(std::min(rounded, sinking),
                           rounded < sinking
                               ? (low.read.gradient - one.read.gradient) * (share / 2.0)
                               : low.read.gradient / 2.0);
        deepest = std::max(deepest, drops.back().first);
    }
    if (deepest <= 0.0) {
        return low.read;
    }
    // A soft maximum of the drops, k ln(1 + sum of (e^(drop/k) - 1)): the one drop alone, nothing
    // added as one begins, turning smoothly from one to the next. Taken over the deepest, so none
    // overflows.
    const double none = maths::exp(-deepest / lowest_softness);
    double sum = 0.0;
    DVec3 sum_gradient{0.0};
    for (const auto& [drop, dropping] : drops) {
        const double weight = maths::exp((drop - deepest) / lowest_softness);
        sum += weight - none;
        sum_gradient += dropping * weight;
    }
    sum += none;
    return {.value = low.read.value - (deepest + (lowest_softness * maths::log(sum))),
            .gradient = low.read.gradient - (sum_gradient / sum)};
}

formation::Smooth::Read Lowered::lowest() const noexcept {
    // Each ground rounded down by the crests it meets; those that can't come within the hand-over
    // of `bound` skipped.
    const auto ways = [&](const double& bound, const auto& each) {
        for (const Ground& low : std::span<const Ground>{grounds_.data(), count_}) {
            if (low.read.value - (blend_ / 4.0) < bound + handing_over) {
                const formation::Smooth::Read way = rounded(low);
                each(way.value, way.gradient);
            }
        }
    };
    double lowest = std::numeric_limits<double>::infinity();
    ways(lowest, [&](double value, DVec3) { lowest = std::min(lowest, value); });
    // Turning as each way comes lowest over the hand-over, so what's rounded from it never steps:
    // a soft minimum, whose slope is the ways' as they share it. Those past the hand-over count
    // for next to nothing.
    DVec3 gradient{0.0};
    double weights = 0.0;
    ways(lowest, [&](double value, DVec3 way) {
        const double weight = maths::exp(-(value - lowest) / lowest_softness);
        gradient += way * weight;
        weights += weight;
    });
    return {.value = lowest - (lowest_softness * maths::log(weights)),
            .gradient = gradient / weights};
}

double Lowered::floor_under(const Ground& low) const noexcept {
    const double over = std::max(blend_, handing_over);
    double floors = 0.0;
    double weights = 0.0;
    for (const Ground& one : std::span<const Ground>{grounds_.data(), count_}) {
        const double apart = one.read.value - low.read.value;
        if (apart < 0.0) {
            continue;
        }
        const double near = std::clamp(1.0 - (apart / over), 0.0, 1.0);
        const double weight = near * near * (3.0 - (2.0 * near));
        floors += weight * one.floor;
        weights += weight;
    }
    return floors / weights;
}

void Lowest::add(const Wall& one) noexcept {
    if (count_ == 0) {
        cut_ = one.cut;
        cut_gradient_ = one.cut_gradient;
        fall_ = one.fall;
        floor_ = one.floor;
        lowest_ = one.standing;
    } else {
        // What runs with the ground hands over to the stretch coming lower over the blend, however
        // sharply the ground itself turns there, so none of it breaks where a floor meets a wall.
        const double over = std::max(blend_, handing_over);
        const double lower = std::clamp(0.5 + ((lowest_ - one.standing) / (2.0 * over)), 0.0, 1.0);
        const double taken = lower * lower * (3.0 - (2.0 * lower));
        cut_ += (one.cut - cut_) * taken;
        cut_gradient_ += (one.cut_gradient - cut_gradient_) * taken;
        fall_ += (one.fall - fall_) * taken;
        floor_ += (one.floor - floor_) * taken;
        lowest_ = std::min(lowest_, one.standing);
    }
    lowered_.add({.value = one.standing, .gradient = one.standing_gradient}, one.steep, one.floor);
    if (count_ < stretches_.size()) {
        stretches_[count_] = {
            .standing = one.standing, .water = one.water, .bank = one.bank, .season = one.season};
    }
    ++count_;
}

void Lowest::lower(Wall& wall) const noexcept {
    if (count_ == 0) {
        return;
    }
    const formation::Smooth::Read ground = lowered_.lowest();
    // The water's level is the deepest-standing stretch's, as far as each is the ground here.
    double deepest = 0.0;
    double flooded = 0.0;
    double water = -std::numeric_limits<double>::infinity();
    double bank = -std::numeric_limits<double>::infinity();
    Season season;
    for (const Stretch& one :
         std::span<const Stretch>{stretches_.data(), std::min(count_, stretches_.size())}) {
        const double over = std::max(0.0, one.standing - lowest_) / handing_over;
        if (over >= 1.0) {
            continue;
        }
        const double in = 1.0 - (over * over * (3.0 - (2.0 * over)));
        if (const double deep = in * (one.water - one.standing); deep > deepest) {
            deepest = deep;
            water = one.water;
            season = one.season;
        }
        if (const double deep = in * (one.bank - one.standing); deep > flooded) {
            flooded = deep;
            bank = one.bank;
        }
    }
    wall.standing = ground.value;
    wall.standing_gradient = ground.gradient;
    // What it stands over is the floor of whichever stretch it stands as, handed over as the
    // ground is.
    wall.floor = floor_;
    // Faded, it lies over the ground no deeper than it's faded to, so it never comes in at once.
    wall.water = std::max(std::min(water, wall.standing + deepest), wall.standing);
    wall.bank = std::max({std::min(bank, wall.standing + flooded), wall.water, wall.standing});
    wall.season = season;
    wall.cut = cut_;
    wall.cut_gradient = cut_gradient_;
    // Water runs off the stretch the ground stands as, turning only where the ground does.
    wall.fall = fall_;
}

} // namespace hog::globe
