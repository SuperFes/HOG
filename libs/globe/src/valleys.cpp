#include "valleys.hpp"

#include <hog/core/portable_maths.hpp>
#include <hog/globe/noise.hpp>

#include <algorithm>
#include <cmath>

namespace hog::globe {

namespace {

double smoothstep(double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
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
    const DVec3 square = glm::cross(valley.from, valley.to);
    const double side = glm::dot(direction, square) < 0.0 ? -1.0 : 1.0;
    const double whole = valley.head + ((valley.foot - valley.head) * t);
    const double run = whole * valley.length;
    const double depth =
        valley.channel_depth + ((valley.channel_depth_to - valley.channel_depth) * whole);
    const double water_depth =
        valley.water_depth + ((valley.water_depth_to - valley.water_depth) * whole);
    double cut = 0.0;
    double cutting = 0.0;
    if (valley.channel_width > 0.0 && away < valley.belt + toe) {
        const double ends = smoothstep(run / valley.meander_length) *
                            smoothstep((valley.length - run) / valley.meander_length);
        const Octaves bends{.wavelength = valley.meander_length,
                            .gain = 0.5,
                            .finest = valley.meander_length / 4.0};
        const double wound =
            valley.meander * ends * fbm(DVec3{run, 0.0, 0.0}, bends, 0.0, valley.seed).value;
        const double off = (side * away) - wound;
        const double across = 2.0 * off / valley.channel_width;
        if (std::abs(across) < 1.0) {
            cut = depth * (1.0 - (across * across));
            cutting = -depth * 4.0 * across / valley.channel_width;
        }
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
        wall.standing_gradient += unit * (climbing - (side * cutting));
        wall.gradient += unit * (climbing + steepening - (side * cutting));
        wall.reaching_gradient = unit * (-steepening / out_of_the_way);
        wall.cut_gradient = unit * (side * cutting);
    }
    wall.standing_gradient += varying;
    wall.gradient += varying;
    // The way the floor runs, turning smoothly from one bend to the next.
    DVec3 runs = along;
    if (glm::dot(valley.runs_from, valley.runs_from) > 0.0) {
        runs = (valley.runs_from * (1.0 - t)) + (valley.runs_to * t);
        runs *= std::sqrt(length) / std::max(glm::length(runs), 1e-300);
    }
    const DVec3 ahead = runs - (direction * glm::dot(runs, direction));
    const bool inside = (t > 0.0 || valley.head > 0.0) && (t < 1.0 || valley.foot < 1.0);
    if (const double size = glm::length(ahead); size > 0.0 && inside) {
        const double span = std::sqrt(length) * radius;
        const DVec3 falling = ahead / size * ((valley.floor_to - valley.floor_from) / span);
        wall.gradient += falling;
        wall.standing_gradient += falling;
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
    if (valley.channel_width > 0.0 && away.low < valley.belt + toe) {
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

void merge_lower(formation::Smooth::Read& top, double& top_wall, double height, DVec3 gradient,
                 double wall, double rounding, double blend) noexcept {
    const double width =
        std::min(blend, rounding * glm::length(gradient - top.gradient)) * wall * top_wall;
    const double apart = std::abs(height - top.value);
    const bool lower = height < top.value;
    if (apart >= width) {
        if (lower) {
            top = {.value = height, .gradient = gradient};
            top_wall = wall;
        }
        return;
    }
    const double share = (width - apart) / width;
    const double low = lower ? height : top.value;
    const DVec3 low_gradient = lower ? gradient : top.gradient;
    const DVec3 high_gradient = lower ? top.gradient : gradient;
    const double low_wall = lower ? wall : top_wall;
    const double high_wall = lower ? top_wall : wall;
    top = {.value = low - (share * share * width / 4.0),
           .gradient = (low_gradient * (1.0 - (share / 2.0))) + (high_gradient * (share / 2.0))};
    top_wall = (low_wall * (1.0 - (share / 2.0))) + (high_wall * (share / 2.0));
}

void Lowest::add(const Wall& one) noexcept {
    if (count_ == 0) {
        top_ = {.value = one.standing, .gradient = one.standing_gradient};
        top_wall_ = one.steep;
        cut_ = one.cut;
        cut_gradient_ = one.cut_gradient;
        fall_ = one.fall;
        floor_ = one.floor;
    } else {
        // What runs with the ground hands over to the stretch coming lower over the blend, however
        // sharply the ground itself turns there, so none of it breaks where a floor meets a wall.
        const double over = std::max(blend_, handing_over);
        const double lower =
            std::clamp(0.5 + ((top_.value - one.standing) / (2.0 * over)), 0.0, 1.0);
        const double taken = lower * lower * (3.0 - (2.0 * lower));
        cut_ += (one.cut - cut_) * taken;
        cut_gradient_ += (one.cut_gradient - cut_gradient_) * taken;
        fall_ += (one.fall - fall_) * taken;
        floor_ += (one.floor - floor_) * taken;
        merge_lower(top_, top_wall_, one.standing, one.standing_gradient, one.steep, rounding_,
                    blend_);
    }
    if (count_ < waters_.size()) {
        waters_[count_] = {.ground = one.standing,
                           .water = std::max(0.0, one.water - one.standing),
                           .flood = std::max(0.0, one.bank - one.standing)};
    }
    ++count_;
}

void Lowest::lower(Wall& wall) const noexcept {
    if (count_ == 0) {
        return;
    }
    double water = 0.0;
    double flood = 0.0;
    for (std::size_t k = 0; k < std::min(count_, waters_.size()); ++k) {
        const Water& one = waters_[k];
        const double over = std::max(0.0, one.ground - top_.value) / handing_over;
        if (over < 1.0) {
            const double in = 1.0 - (over * over * (3.0 - (2.0 * over)));
            water = std::max(water, in * one.water);
            flood = std::max(flood, in * one.flood);
        }
    }
    wall.standing = top_.value;
    wall.standing_gradient = top_.gradient;
    // What it stands over is the floor of whichever stretch it stands as, handed over as the
    // ground is.
    wall.floor = floor_;
    wall.water = wall.standing + water;
    wall.bank = wall.standing + flood;
    wall.cut = cut_;
    wall.cut_gradient = cut_gradient_;
    // Water runs off the stretch the ground stands as, turning only where the ground does.
    wall.fall = fall_;
}

} // namespace hog::globe
