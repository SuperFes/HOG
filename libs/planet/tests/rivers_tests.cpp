#include <hog/planet/landform.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/rivers.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/testing/quick.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <tuple>
#include <vector>

using hog::planet::CellId;
using hog::planet::fixed_one;
using hog::planet::Landform;
using hog::planet::LevelGrid;
using hog::planet::Reach;
using hog::planet::Rivers;

namespace {

constexpr double radius = 115'000.0;
constexpr int level = 6;

// Land rising north of a sea, worn for a while so rivers have valleys to follow.
Landform worn(std::int64_t runoff) {
    LevelGrid grid{level, radius};
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const hog::DVec3 at = hog::planet::cell_center(grid.cell(cell));
        const double metres =
            at.z > 0.0
                ? 20.0 + (900.0 * at.z) + (60.0 * std::sin(23.0 * at.x) * std::cos(19.0 * at.y))
                : -1000.0;
        heights.push_back(std::llround(metres * static_cast<double>(fixed_one)));
    }
    const std::vector<std::int64_t> uplift(grid.size(), 5 * fixed_one);
    const std::vector<std::int64_t> erodibility(grid.size(), fixed_one / 20);
    const std::vector<std::int64_t> rain(grid.size(), runoff);
    return Landform{std::move(grid),
                    std::move(heights),
                    {.uplift = uplift, .erodibility = erodibility, .runoff = rain},
                    hog::planet::Hillslope{},
                    20};
}

const Landform& landform() {
    static const Landform landform = worn(fixed_one / 2);
    return landform;
}

std::vector<std::int64_t> ground(const Landform& landform) {
    return {landform.heights().begin(), landform.heights().end()};
}

const Rivers& rivers() {
    static const Rivers rivers{
        landform(), ground(landform()), {1, landform().grid(), radius}, radius};
    return rivers;
}

// The reaches looked across: all of them, or every fourth when the longest tests run small.
std::vector<Reach> looked_across() {
    std::vector<Reach> reaches;
    const auto all = rivers().reaches();
    for (std::size_t at = 0; at < all.size(); at += hog::testing::full_or<std::size_t>(1, 4)) {
        reaches.push_back(all[at]);
    }
    return reaches;
}

// The ground before rivers, a gentle slope up to the north, high above any river's bed.
std::int64_t plain(hog::DVec3 at) {
    return std::llround((2000.0 + (100.0 * at.z)) * static_cast<double>(fixed_one));
}

// Cells on a line across a reach's course a share `t` of the way along, from bank to bank, wide
// enough to take in the meanders, close enough together not to step over the narrowest channel.
// The line reaches `wide` of the reach's length either side.
std::vector<CellId> across(const Reach& reach, double t = 0.5, double wide = 0.5) {
    const hog::DVec3 middle = hog::planet::course_at(reach, t);
    const hog::DVec3 heading = hog::planet::course_at(reach, std::min(t + 0.01, 1.0)) -
                               hog::planet::course_at(reach, std::max(t - 0.01, 0.0));
    const hog::DVec3 side = glm::normalize(glm::cross(middle, heading));
    const double span = glm::length(reach.to - reach.from) * wide;
    const double step = reach.width / 4.0 / radius;
    const auto steps = static_cast<int>(2.0 * span / step);
    std::vector<CellId> cells;
    for (int at = 0; at <= steps; ++at) {
        const double offset = -span + (step * at);
        cells.push_back(hog::planet::cell_at(glm::normalize(middle + (side * offset)), 22));
    }
    return cells;
}

} // namespace

TEST_CASE("rivers widen and deepen with the water they carry", "[planet][rivers]") {
    CHECK(Rivers::width(1e9) > Rivers::width(1e8));
    CHECK(Rivers::depth(1e9) > Rivers::depth(1e8));
    CHECK(Rivers::width(Rivers::source_area) > 1.0);
    CHECK(Rivers::width(Rivers::source_area) < 5.0);
}

TEST_CASE("rivers run downhill to the sea or a lake, growing on the way", "[planet][rivers]") {
    const auto reaches = rivers().reaches();
    REQUIRE(reaches.size() > 20);
    int at_sea = 0;
    for (const Reach& reach : reaches) {
        REQUIRE(reach.to_surface <= reach.from_surface);
        const auto next = std::ranges::find_if(
            reaches, [&](const Reach& other) { return other.from == reach.to; });
        if (next == reaches.end()) {
            const bool sea = reach.to.z <= 0.05;
            at_sea += sea ? 1 : 0;
            const auto lake =
                rivers().lake(hog::planet::cell_at(reach.to, 22), reach.to_surface - fixed_one);
            CHECK((sea || lake == reach.to_surface));
        } else {
            REQUIRE(next->from_surface == reach.to_surface);
            REQUIRE(next->width >= reach.width);
        }
    }
    CHECK(at_sea > 0);
}

TEST_CASE("a river cuts its channel below the ground, under water", "[planet][rivers]") {
    for (const Reach& reach : looked_across()) {
        std::optional<hog::planet::Channel> found;
        for (const CellId cell : across(reach)) {
            if (const auto channel =
                    rivers().channel(cell, plain(hog::planet::cell_center(cell)))) {
                found = channel;
                REQUIRE(rivers().in_channel(cell));
                // The ground is far above the river's planned water, so the channel holds it.
                REQUIRE(channel->bed < channel->surface);
                REQUIRE(channel->surface <= plain(hog::planet::cell_center(cell)));
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("a river's channel runs unbroken along its course", "[planet][rivers]") {
    const auto order = [](hog::DVec3 a, hog::DVec3 b) {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    };
    std::vector<hog::DVec3> starts;
    std::vector<hog::DVec3> ends;
    for (const Reach& reach : rivers().reaches()) {
        starts.push_back(reach.from);
        ends.push_back(reach.to);
    }
    std::ranges::sort(starts, order);
    std::ranges::sort(ends, order);
    for (const Reach& reach : looked_across()) {
        // Between rivers running in and on, where the meanders' shift can't take the line across
        // beyond either end.
        if (!std::ranges::binary_search(ends, reach.from, order) ||
            !std::ranges::binary_search(starts, reach.to, order)) {
            continue;
        }
        // Where it meets the reaches either side, which straight courses turned corners at.
        for (const double t : {0.02, 0.5, 0.98}) {
            // As far as the meanders shift it.
            const auto cells = across(reach, t, 0.875);
            REQUIRE(std::ranges::any_of(cells, [&](CellId cell) {
                return rivers().channel(cell, plain(hog::planet::cell_center(cell))).has_value();
            }));
        }
    }
}

TEST_CASE("a river bends through each cell rather than turning a corner there",
          "[planet][rivers]") {
    const auto reaches = rivers().reaches();
    // Of a reach's course, which way it heads a share `t` of the way along.
    const auto heading = [](const Reach& reach, double t) {
        constexpr double step = 1e-4;
        return glm::normalize(hog::planet::course_at(reach, std::min(t + step, 1.0)) -
                              hog::planet::course_at(reach, std::max(t - step, 0.0)));
    };
    int joined = 0;
    for (const Reach& reach : reaches) {
        CHECK(glm::length(hog::planet::course_at(reach, 0.0) - reach.from) < 1e-12);
        CHECK(glm::length(hog::planet::course_at(reach, 1.0) - reach.to) < 1e-12);
        const auto next = std::ranges::find_if(
            reaches, [&](const Reach& other) { return other.from == reach.to; });
        if (next == reaches.end()) {
            continue;
        }
        REQUIRE(reach.arriving == next->leaving);
        // The grid's own turns are an eighth of a circle or more.
        CHECK(glm::dot(heading(reach, 1.0), heading(*next, 0.0)) > std::cos(0.01));
        ++joined;
    }
    CHECK(joined > 10);
}

TEST_CASE("a river meanders about its course, by a width or two", "[planet][rivers]") {
    int swinging = 0;
    for (const Reach& reach : rivers().reaches()) {
        const double run = glm::length(reach.to - reach.from);
        CHECK(reach.swing_from * radius <= Rivers::swing_widths * reach.width * (1.0 + 1e-9));
        CHECK(reach.swing_from <= 0.2 * run * (1.0 + 1e-9));
        CHECK(reach.swing_to <= 0.2 * run * (1.0 + 1e-9));
        CHECK(reach.turn_from > 0.0);
        swinging += reach.swing_from * radius > reach.width ? 1 : 0;
        // Most of a meander or more, the shortest are a few widths across.
        const double meanders = (reach.turn_from + reach.turn_to) / 2.0 / (2.0 * std::numbers::pi);
        CHECK(meanders * Rivers::meander_widths * reach.width > 0.5 * run * radius);
    }
    CHECK(swinging > 10);
}

TEST_CASE("a river's meanders carry on through each cell, and tributaries settle onto the river",
          "[planet][rivers]") {
    const auto reaches = rivers().reaches();
    int carried = 0;
    int settled = 0;
    for (const Reach& reach : reaches) {
        const auto next = std::ranges::find_if(
            reaches, [&](const Reach& other) { return other.from == reach.to; });
        if (next == reaches.end()) {
            CHECK(reach.settling == 0.0);
            continue;
        }
        const hog::planet::Swing end = hog::planet::swing_at(reach, 1.0);
        const hog::planet::Swing start = hog::planet::swing_at(*next, 0.0);
        CHECK(std::abs(end.offset - start.offset) * radius < 1e-6);
        if (reach.settling == 0.0) {
            CHECK(std::abs(end.slope - start.slope) < 1e-6);
            ++carried;
        } else {
            CHECK(reach.settling <= 1.0);
            ++settled;
        }
    }
    CHECK(carried > 10);
    CHECK(settled > 0);
}

TEST_CASE("a river's width eases on into the river downstream, wider round its bends",
          "[planet][rivers]") {
    const auto reaches = rivers().reaches();
    int carried = 0;
    int varied = 0;
    for (const Reach& reach : reaches) {
        const double least = std::min(reach.width, reach.width_to);
        const double most = std::max(reach.width, reach.width_to);
        CHECK(std::abs(reach.wobble) <= Rivers::wobble_widening);
        double narrowest = std::numeric_limits<double>::infinity();
        double widest = 0.0;
        for (int step = 0; step <= 20; ++step) {
            const double width = hog::planet::width_at(reach, step / 20.0);
            CHECK(width >= least * (1.0 - Rivers::wobble_widening) * (1.0 - 1e-9));
            CHECK(width <= most * (1.0 + Rivers::bend_widening) * (1.0 + Rivers::wobble_widening) *
                               (1.0 + 1e-9));
            narrowest = std::min(narrowest, width);
            widest = std::max(widest, width);
        }
        varied += widest > 1.15 * narrowest ? 1 : 0;
        const auto next = std::ranges::find_if(
            reaches, [&](const Reach& other) { return other.from == reach.to; });
        if (next == reaches.end() || reach.settling != 0.0) {
            continue;
        }
        CHECK(reach.width_to == next->width);
        CHECK(std::abs(hog::planet::width_at(reach, 1.0) - hog::planet::width_at(*next, 0.0)) <
              1e-9 * next->width);
        ++carried;
    }
    CHECK(carried > 10);
    CHECK(varied > 10);
}

TEST_CASE("a river's channel swings with its meanders", "[planet][rivers]") {
    const hog::planet::Meander& meander = rivers().meander();
    int swung = 0;
    for (const Reach& reach : looked_across()) {
        for (const double t : {0.25, 0.4, 0.55, 0.7}) {
            const hog::DVec3 course = hog::planet::course_at(reach, t);
            const hog::DVec3 heading =
                hog::planet::course_at(reach, t + 0.01) - hog::planet::course_at(reach, t - 0.01);
            const hog::DVec3 side = glm::normalize(glm::cross(course, heading));
            const double offset = hog::planet::swing_at(reach, t).offset;
            const auto at = [&](double off) {
                return hog::planet::cell_at(
                    meander.unshifted(glm::normalize(course + (side * off))), 22);
            };
            REQUIRE(rivers().in_channel(at(offset)));
            // Across the swung river, which runs slanted over its course.
            const double slope = hog::planet::swing_at(reach, t).slope;
            const double half =
                hog::planet::width_at(reach, t) / 2.0 / radius * std::sqrt(1.0 + (slope * slope));
            CHECK(rivers().in_channel(at(offset + (0.8 * half))));
            CHECK(rivers().in_channel(at(offset - (0.8 * half))));
            if (std::abs(offset) * radius > hog::planet::width_at(reach, t)) {
                CHECK_FALSE(rivers().in_channel(at(0.0)));
                ++swung;
            }
        }
    }
    CHECK(swung > 10);
}

TEST_CASE("a river tells how much it carries", "[planet][rivers]") {
    const auto reaches = rivers().reaches();
    int joined = 0;
    for (const Reach& reach : reaches) {
        REQUIRE(reach.flow > 0.0);
        const auto next = std::ranges::find_if(
            reaches, [&](const Reach& other) { return other.from == reach.to; });
        if (next == reaches.end()) {
            continue;
        }
        REQUIRE(next->flow >= reach.flow);
        ++joined;
    }
    CHECK(joined > 10);
}

TEST_CASE("a river follows dips in the ground rather than standing above it", "[planet][rivers]") {
    for (const Reach& reach : looked_across()) {
        const std::int64_t low = std::min(reach.from_surface, reach.to_surface) - (50 * fixed_one);
        for (const CellId cell : across(reach)) {
            if (const auto channel = rivers().channel(cell, low)) {
                REQUIRE(channel->surface <= low);
                REQUIRE(channel->bed <= channel->surface);
            }
        }
    }
}

TEST_CASE("banks slope up from the water rather than standing as walls", "[planet][rivers]") {
    for (const Reach& reach : looked_across()) {
        // Shifting points to make meanders can bunch them up to about twice as close.
        const double step = reach.width / 4.0;
        // Its bed is steepest where it runs narrowest.
        const double half = reach.width / 2.0 * (1.0 - Rivers::wobble_widening);
        const double steepest =
            2.0 * step * std::max(0.5, 2.0 * hog::planet::fixed_to_double(reach.depth) / half);
        const auto cells = across(reach);
        for (std::size_t k = 1; k < cells.size(); ++k) {
            const auto height = [&](CellId cell) {
                return rivers().cut(cell, plain(hog::planet::cell_center(cell)));
            };
            REQUIRE(hog::planet::fixed_to_double(
                        std::abs(height(cells[k]) - height(cells[k - 1]))) < steepest);
        }
    }
}

TEST_CASE("scaled rivers run the same courses, wider and deeper", "[planet][rivers]") {
    const Rivers scaled{
        landform(), ground(landform()), {1, landform().grid(), radius}, radius, 3.0};
    const auto plain = rivers().reaches();
    const auto wide = scaled.reaches();
    REQUIRE(wide.size() == plain.size());
    for (std::size_t k = 0; k < plain.size(); ++k) {
        REQUIRE(wide[k].from == plain[k].from);
        REQUIRE(wide[k].to == plain[k].to);
        REQUIRE(wide[k].width == 3.0 * plain[k].width);
        REQUIRE(wide[k].depth > plain[k].depth);
    }
}

TEST_CASE("rivers can be found from a distance for drawing", "[planet][rivers]") {
    const Reach& reach = rivers().reaches().front();
    const hog::DVec3 middle = glm::normalize((reach.from + reach.to) * 0.5);
    const hog::DVec3 side = glm::normalize(glm::cross(middle, reach.to - reach.from));
    // Half a cell off the line, beyond any meander.
    const double off = glm::length(reach.to - reach.from) * 0.9;
    const CellId far = hog::planet::cell_at(glm::normalize(middle + (side * off)), 22);
    CHECK(rivers().near(far, 1.0) == nullptr);
    CHECK(rivers().near(far, off * radius * 2.0) != nullptr);
    for (const CellId cell : across(reach)) {
        if (rivers().channel(cell, plain(hog::planet::cell_center(cell)))) {
            CHECK(rivers().near(cell, 0.0) != nullptr);
        }
    }
}

namespace {

// Land rising north of a sea with a bowl sunk in it, left as it is.
constexpr hog::DVec3 bowl_center{0.6, 0.0, 0.8};

Landform bowl(std::int64_t runoff, double depth = 600.0) {
    LevelGrid grid{level, radius};
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const hog::DVec3 at = hog::planet::cell_center(grid.cell(cell));
        const double dip = depth * std::max(0.0, 1.0 - (glm::length(at - bowl_center) / 0.25));
        const double metres = at.z > 0.0 ? 20.0 + (900.0 * at.z) - dip : -1000.0;
        heights.push_back(std::llround(metres * static_cast<double>(fixed_one)));
    }
    const std::vector<std::int64_t> none(grid.size(), 0);
    const std::vector<std::int64_t> rain(grid.size(), runoff);
    return Landform{std::move(grid),
                    std::move(heights),
                    {.uplift = none, .erodibility = none, .runoff = rain},
                    hog::planet::Hillslope{},
                    0};
}

bool in_bowl(hog::DVec3 at, double within) {
    return glm::length(at - bowl_center) < within;
}

} // namespace

TEST_CASE("a basin with water enough holds a level lake, drained by a river from its outlet",
          "[planet][rivers]") {
    const Landform basin = bowl(fixed_one / 2);
    const Rivers wet{basin, ground(basin), {1, basin.grid(), radius}, radius};
    const CellId middle = hog::planet::cell_at(bowl_center, 22);
    const auto lake = wet.lake(middle, std::numeric_limits<std::int64_t>::min() / 2);
    REQUIRE(lake);
    const std::int64_t surface = lake.value_or(0);
    for (const double out : {0.05, 0.1}) {
        const CellId cell =
            hog::planet::cell_at(glm::normalize(bowl_center + hog::DVec3{0.0, out, 0.0}), 22);
        CHECK(wet.lake(cell, std::numeric_limits<std::int64_t>::min() / 2) == lake);
    }
    // Ground above the lake is dry.
    CHECK_FALSE(wet.lake(middle, surface + fixed_one));

    int inside = 0;
    int out_of = 0;
    for (const Reach& reach : wet.reaches()) {
        inside += in_bowl(reach.from, 0.1) && in_bowl(reach.to, 0.1) ? 1 : 0;
        out_of += in_bowl(reach.from, 0.3) && reach.from_surface <= surface ? 1 : 0;
    }
    CHECK(inside == 0);
    CHECK(out_of > 0);
}

TEST_CASE("a lake takes in all the ground under its level about it, its shores standing above",
          "[planet][rivers]") {
    const Landform basin = bowl(fixed_one / 2);
    const LevelGrid& grid = basin.grid();
    const auto lakes = basin.lakes();
    int wet = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (lakes[cell] == Landform::dry) {
            continue;
        }
        ++wet;
        REQUIRE(basin.basin(cell));
        REQUIRE(basin.heights()[cell] < lakes[cell]);
        REQUIRE(basin.lakeside()[cell] == fixed_one);
        REQUIRE(basin.submerged()[cell] == fixed_one);
        for (const std::uint32_t around : grid.neighbors(cell)) {
            REQUIRE(basin.lakeside()[around] == fixed_one);
            REQUIRE(basin.lakes_about()[around] >= lakes[cell]);
            if (lakes[around] == Landform::dry) {
                // Only ground filled less than a centimetre, a flat's slope, lies under the level.
                REQUIRE(basin.heights()[around] >= lakes[cell] - (fixed_one / 50));
            } else {
                REQUIRE(lakes[around] == lakes[cell]);
            }
        }
    }
    CHECK(wet > 10);
    std::int64_t lakeside = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        lakeside += basin.lakeside()[cell];
        // Off the lakes and the cells beside them, nothing.
        if (basin.lakeside()[cell] == 0) {
            REQUIRE(basin.lakes_about()[cell] == Landform::dry);
        }
        if (lakes[cell] == Landform::dry) {
            REQUIRE(basin.submerged()[cell] == 0);
        }
    }
    CHECK(lakeside > wet * fixed_one);
}

TEST_CASE("the water of a lake meets the ground at its shore", "[planet][rivers]") {
    const Landform basin = bowl(fixed_one / 2);
    const Rivers wet{basin, ground(basin), {1, basin.grid(), radius}, radius};
    const auto found = wet.lake(hog::planet::cell_at(bowl_center, 22), 0);
    REQUIRE(found);
    const std::int64_t lake = found.value_or(0);
    // Out from the middle, the water ends where the landform's ground rises through its level.
    int shores = 0;
    for (const double heading : {0.0, 1.0, 2.0, 3.0, 4.0, 5.0}) {
        const hog::DVec3 out{-0.8 * std::cos(heading), std::sin(heading), 0.6 * std::cos(heading)};
        for (int step = 1; step < 400; ++step) {
            const CellId cell =
                hog::planet::cell_at(glm::normalize(bowl_center + (out * (step * 0.001))), 22);
            const std::int64_t ground = basin.grid().sample(
                basin.heights(), hog::planet::cell_at(hog::planet::Meander{1, basin.grid(), radius}(
                                     hog::planet::cell_center(cell))));
            const bool water = wet.lake(cell, ground).has_value();
            REQUIRE(water == (ground < lake));
            if (!water) {
                ++shores;
                break;
            }
        }
    }
    CHECK(shores == 6);
}

TEST_CASE("a basin too dry to fill holds no lake and no rivers", "[planet][rivers]") {
    const Landform basin = bowl(fixed_one / 1'000);
    const Rivers dry{basin, ground(basin), {1, basin.grid(), radius}, radius};
    CHECK_FALSE(dry.lake(hog::planet::cell_at(bowl_center, 22),
                         std::numeric_limits<std::int64_t>::min() / 2));
    for (const Reach& reach : dry.reaches()) {
        REQUIRE_FALSE(in_bowl(reach.from, 0.2));
    }
}

TEST_CASE("without rain there are no rivers", "[planet][rivers]") {
    const Landform dry = worn(0);
    const Rivers none{dry, ground(dry), {1, dry.grid(), radius}, radius};
    CHECK(none.reaches().empty());
    for (std::uint32_t cell = 0; cell < dry.grid().size(); cell += 7) {
        REQUIRE_FALSE(none.channel(dry.grid().cell(cell).child(0), fixed_one));
        REQUIRE(none.cut(dry.grid().cell(cell).child(0), fixed_one) == fixed_one);
    }
}

TEST_CASE("each cell drains into water no higher than itself downstream", "[planet][rivers]") {
    const auto levels = rivers().levels();
    const auto& drainage = landform().drainage();
    REQUIRE(levels.size() == landform().grid().size());
    for (std::uint32_t cell = 0; cell < levels.size(); ++cell) {
        const std::uint32_t receiver = drainage.receiver[cell];
        if (receiver == cell) {
            REQUIRE(levels[cell] == 0);
        } else {
            REQUIRE(levels[receiver] <= levels[cell]);
        }
    }
}

TEST_CASE("low ground cut off from the sea holds a lake or lies dry, never the sea",
          "[planet][rivers]") {
    const CellId middle = hog::planet::cell_at(bowl_center, 22);
    const std::int64_t bottom = -400 * fixed_one;
    const Landform wet_basin = bowl(fixed_one / 2, 1200.0);
    const Rivers wet{wet_basin, ground(wet_basin), {1, wet_basin.grid(), radius}, radius};
    const auto lake = wet.standing(middle, bottom).value_or(hog::planet::StandingWater{});
    CHECK_FALSE(lake.sea);
    CHECK(lake.level > 0);

    const Landform dry_basin = bowl(fixed_one / 1'000, 1200.0);
    const Rivers dry{dry_basin, ground(dry_basin), {1, dry_basin.grid(), radius}, radius};
    CHECK_FALSE(dry.standing(middle, bottom));

    const CellId offshore = hog::planet::cell_at(glm::normalize(hog::DVec3{0.6, 0.0, -0.8}), 22);
    CHECK(dry.standing(offshore, bottom) == hog::planet::StandingWater{.level = 0, .sea = true});
    CHECK_FALSE(dry.lake(offshore, bottom));
}

TEST_CASE("rivers fall where their water drops steeply, and only there", "[planet][rivers]") {
    // A gentle slope up from the sea, broken by a cliff.
    constexpr double cliff = 0.5;
    LevelGrid grid{level, radius};
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const double z = hog::planet::cell_center(grid.cell(cell)).z;
        const double metres = z > 0.0 ? 20.0 + (300.0 * z) + (z > cliff ? 600.0 : 0.0) : -1000.0;
        heights.push_back(std::llround(metres * static_cast<double>(fixed_one)));
    }
    const std::vector<std::int64_t> uplift(grid.size(), 0);
    const std::vector<std::int64_t> erodibility(grid.size(), 0);
    const std::vector<std::int64_t> rain(grid.size(), fixed_one / 2);
    const Landform stepped{std::move(grid),
                           std::move(heights),
                           {.uplift = uplift, .erodibility = erodibility, .runoff = rain},
                           hog::planet::Hillslope{},
                           0};
    const Rivers flowing{stepped, ground(stepped), {1, stepped.grid(), radius}, radius};
    int over = 0;
    for (const Reach& reach : flowing.reaches()) {
        const bool crosses = reach.from.z > cliff && reach.to.z <= cliff;
        const double gradient =
            hog::planet::fixed_to_double(reach.from_surface - reach.to_surface) /
            (glm::length(reach.to - reach.from) * radius);
        if (crosses) {
            ++over;
            CHECK(reach.fall == reach.from_surface - reach.to_surface);
        } else if (gradient < Rivers::falls_gradient) {
            CHECK(reach.fall == 0);
        }
        CHECK(reach.fall >= 0);
    }
    CHECK(over > 0);
}
