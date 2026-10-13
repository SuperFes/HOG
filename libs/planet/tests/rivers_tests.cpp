#include <hog/planet/landform.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/rivers.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
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
    static const Rivers rivers{landform(), ground(landform()), {1, landform().grid()}, radius};
    return rivers;
}

// The ground before rivers, a gentle slope up to the north, high above any river's bed.
std::int64_t plain(hog::DVec3 at) {
    return std::llround((2000.0 + (100.0 * at.z)) * static_cast<double>(fixed_one));
}

// Cells on a line across a reach's middle from bank to bank, wide enough to take in the meanders,
// close enough together not to step over the narrowest channel.
std::vector<CellId> across(const Reach& reach) {
    const hog::DVec3 middle = glm::normalize((reach.from + reach.to) * 0.5);
    const hog::DVec3 side = glm::normalize(glm::cross(middle, reach.to - reach.from));
    const double span = glm::length(reach.to - reach.from) * 0.5;
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
    for (const Reach& reach : rivers().reaches()) {
        std::optional<hog::planet::Channel> found;
        for (const CellId cell : across(reach)) {
            if (const auto channel =
                    rivers().channel(cell, plain(hog::planet::cell_center(cell)))) {
                found = channel;
                // The ground is far above the river's planned water, so the channel holds it.
                REQUIRE(channel->bed < channel->surface);
                REQUIRE(channel->surface <= plain(hog::planet::cell_center(cell)));
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("a river follows dips in the ground rather than standing above it", "[planet][rivers]") {
    for (const Reach& reach : rivers().reaches()) {
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
    for (const Reach& reach : rivers().reaches()) {
        // Shifting points to make meanders can bunch them up to about twice as close.
        const double step = reach.width / 4.0;
        const double half = reach.width / 2.0;
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
    const Rivers scaled{landform(), ground(landform()), {1, landform().grid()}, radius, 3.0};
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

Landform bowl(std::int64_t runoff) {
    LevelGrid grid{level, radius};
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const hog::DVec3 at = hog::planet::cell_center(grid.cell(cell));
        const double dip = 600.0 * std::max(0.0, 1.0 - (glm::length(at - bowl_center) / 0.25));
        const double metres = at.z > 0.0 ? 20.0 + (900.0 * at.z) - dip : -1000.0;
        heights.push_back(std::llround(metres * static_cast<double>(fixed_one)));
    }
    const std::vector<std::int64_t> none(grid.size(), 0);
    const std::vector<std::int64_t> rain(grid.size(), runoff);
    return Landform{std::move(grid),
                    std::move(heights),
                    {.uplift = none, .erodibility = none, .runoff = rain},
                    0};
}

bool in_bowl(hog::DVec3 at, double within) {
    return glm::length(at - bowl_center) < within;
}

} // namespace

TEST_CASE("a basin with water enough holds a level lake, drained by a river from its outlet",
          "[planet][rivers]") {
    const Landform basin = bowl(fixed_one / 2);
    const Rivers wet{basin, ground(basin), {1, basin.grid()}, radius};
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

TEST_CASE("a basin too dry to fill holds no lake and no rivers", "[planet][rivers]") {
    const Landform basin = bowl(fixed_one / 1'000);
    const Rivers dry{basin, ground(basin), {1, basin.grid()}, radius};
    CHECK_FALSE(dry.lake(hog::planet::cell_at(bowl_center, 22),
                         std::numeric_limits<std::int64_t>::min() / 2));
    for (const Reach& reach : dry.reaches()) {
        REQUIRE_FALSE(in_bowl(reach.from, 0.2));
    }
}

TEST_CASE("without rain there are no rivers", "[planet][rivers]") {
    const Landform dry = worn(0);
    const Rivers none{dry, ground(dry), {1, dry.grid()}, radius};
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
