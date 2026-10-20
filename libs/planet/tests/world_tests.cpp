#include <hog/core/hash.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

using hog::ErrorCode;
using hog::GameTime;
using hog::planet::AuthorId;
using hog::planet::Biome;
using hog::planet::CellId;
using hog::planet::CellState;
using hog::planet::Change;
using hog::planet::ChangeFilter;
using hog::planet::ChangePayload;
using hog::planet::Excavate;
using hog::planet::fixed_one;
using hog::planet::Hollow;
using hog::planet::Layer;
using hog::planet::SetBiome;
using hog::planet::SetElevation;
using hog::planet::World;

namespace {

constexpr hog::planet::PlanetConfig planet{.seed = 1, .ocean_fraction = 0.7, .radius = 115'000.0};

// One World for the whole file: the seed's generators are costly to set up.
World& shared_world() {
    static World world{planet};
    return world;
}

World& fresh_world() {
    shared_world().roll_back({});
    return shared_world();
}

GameTime at(std::int64_t seconds) {
    return GameTime{std::chrono::seconds{seconds}};
}

Change change(CellId cell, ChangePayload payload, std::int64_t seconds = 0, AuthorId author = {},
              Layer layer = Layer::Player) {
    return {
        .cell = cell, .time = at(seconds), .author = author, .layer = layer, .payload = payload};
}

constexpr CellId cell = CellId::from_face_ij(2, 300, 400, 10);

ChangeFilter by_author(AuthorId author) {
    ChangeFilter filter;
    filter.author = author;
    return filter;
}

ChangeFilter by_layer(Layer layer) {
    ChangeFilter filter;
    filter.layer = layer;
    return filter;
}

ChangeFilter during(std::int64_t from, std::int64_t until) {
    ChangeFilter filter;
    filter.from = at(from);
    filter.until = at(until);
    return filter;
}

CellState seed_state(CellId at_cell) {
    const auto& geology = shared_world().geology();
    const std::int64_t elevation = geology.terrain().elevation(at_cell);
    const hog::planet::Climate::Surface surface = geology.climate().surface(at_cell, elevation);
    return {.elevation = elevation,
            .biome = surface.biome,
            .temperature = surface.temperature,
            .moisture = surface.moisture,
            .water_table = surface.water_table,
            .excavated = 0,
            .hollows = {}};
}

} // namespace

TEST_CASE("an untouched world is its seed", "[planet][world]") {
    World& w = fresh_world();
    CHECK(w.size() == 0);
    CHECK(w.state(cell) == seed_state(cell));
    CHECK(w.changes().empty());
}

TEST_CASE("changes apply over the seed", "[planet][world]") {
    World& w = fresh_world();
    REQUIRE(w.apply(change(cell, SetElevation{.elevation = 50 * fixed_one})));
    REQUIRE(w.apply(change(cell, Excavate{.depth = 3 * fixed_one}, 1)));
    REQUIRE(w.apply(change(cell, Excavate{.depth = 2 * fixed_one}, 2)));
    REQUIRE(w.apply(change(cell, SetBiome{.biome = Biome::Grassland}, 3)));
    const CellState state = w.state(cell);
    CHECK(state.elevation == 45 * fixed_one);
    CHECK(state.excavated == 5 * fixed_one);
    CHECK(state.biome == Biome::Grassland);
    // The climate is the generated ground's, whatever is dug or raised on it.
    CHECK(state.temperature == seed_state(cell).temperature);
    CHECK(w.size() == 4);
}

TEST_CASE("a change covers everything inside its cell", "[planet][world]") {
    World& w = fresh_world();
    REQUIRE(w.apply(change(cell, SetBiome{.biome = Biome::Desert})));
    const CellId child = cell.child(2);
    const CellId grandchild = child.child(1);
    CHECK(w.state(child).biome == Biome::Desert);
    CHECK(w.state(grandchild).biome == Biome::Desert);

    REQUIRE(w.apply(change(child, Excavate{.depth = fixed_one})));
    CHECK(w.state(grandchild).excavated == fixed_one);
    // Neither the parent nor a sibling.
    CHECK(w.state(cell).excavated == 0);
    CHECK(w.state(cell.child(0)).excavated == 0);
    CHECK(w.state(cell.parent()) == seed_state(cell.parent()));
}

TEST_CASE("higher layers win, then later times, then later changes", "[planet][world]") {
    World& w = fresh_world();
    SECTION("layers") {
        REQUIRE(w.apply(change(cell, SetBiome{.biome = Biome::Savanna}, 5, {}, Layer::Player)));
        REQUIRE(w.apply(change(cell, SetBiome{.biome = Biome::Tundra}, 9, {}, Layer::Authored)));
        REQUIRE(w.apply(change(cell, SetBiome{.biome = Biome::Desert}, 7, {}, Layer::Simulation)));
        CHECK(w.state(cell).biome == Biome::Savanna);
    }
    SECTION("times") {
        REQUIRE(w.apply(change(cell, SetElevation{.elevation = 2 * fixed_one}, 20)));
        REQUIRE(w.apply(change(cell, SetElevation{.elevation = fixed_one}, 10)));
        CHECK(w.state(cell).elevation == 2 * fixed_one);
    }
    SECTION("order made") {
        REQUIRE(w.apply(change(cell, SetElevation{.elevation = fixed_one}, 10)));
        REQUIRE(w.apply(change(cell, SetElevation{.elevation = 2 * fixed_one}, 10)));
        CHECK(w.state(cell).elevation == 2 * fixed_one);
    }
    SECTION("a coarse change made later still applies after a finer one") {
        REQUIRE(w.apply(change(cell.child(3), SetElevation{.elevation = fixed_one}, 10)));
        REQUIRE(w.apply(change(cell, SetElevation{.elevation = 2 * fixed_one}, 20)));
        CHECK(w.state(cell.child(3)).elevation == 2 * fixed_one);
    }
}

TEST_CASE("invalid changes are refused and change nothing", "[planet][world]") {
    World& w = fresh_world();
    const auto refused = [&](const Change& bad) {
        const auto applied = w.apply(bad);
        return !applied && applied.error().code == ErrorCode::InvalidArgument;
    };
    CHECK(refused(change(CellId{}, SetElevation{})));
    CHECK(refused(change(cell, Excavate{.depth = 0})));
    CHECK(refused(change(cell, Excavate{.depth = -fixed_one})));
    CHECK(refused(change(cell, SetBiome{.biome = static_cast<Biome>(hog::planet::biome_count)})));
    CHECK(refused(change(cell, Hollow{.top = -fixed_one, .bottom = fixed_one})));
    CHECK(refused(change(cell, Hollow{.top = fixed_one, .bottom = fixed_one})));
    CHECK(
        refused(change(cell, SetElevation{}, 0, {}, static_cast<Layer>(hog::planet::layer_count))));
    CHECK(w.size() == 0);

    // A replay with one bad change applies none of them.
    const std::vector<Change> batch{change(cell, SetElevation{}),
                                    change(cell, Excavate{.depth = 0})};
    const auto replayed = w.replay(batch);
    REQUIRE_FALSE(replayed);
    CHECK(replayed.error().message.starts_with("change 1:"));
    CHECK(w.size() == 0);
}

TEST_CASE("hollows dug in a cell join where they meet", "[planet][world]") {
    World& w = fresh_world();
    const auto dig = [&](std::int64_t top, std::int64_t bottom, std::int64_t seconds) {
        REQUIRE(w.apply(
            change(cell, Hollow{.top = top * fixed_one, .bottom = bottom * fixed_one}, seconds)));
    };
    const auto metres = [](std::initializer_list<std::pair<std::int64_t, std::int64_t>> spans) {
        std::vector<Hollow> hollows;
        for (const auto& [top, bottom] : spans) {
            hollows.push_back({.top = top * fixed_one, .bottom = bottom * fixed_one});
        }
        return hollows;
    };
    dig(10, 12, 1);
    dig(2, 4, 2);
    CHECK(w.state(cell).hollows == metres({{2, 4}, {10, 12}}));
    dig(4, 6, 3); // touching
    CHECK(w.state(cell).hollows == metres({{2, 6}, {10, 12}}));
    dig(5, 11, 4); // bridging
    CHECK(w.state(cell).hollows == metres({{2, 12}}));
    dig(20, 21, 5);
    // Cells inside take their parent's hollows.
    CHECK(w.state(cell.child(3)).hollows == metres({{2, 12}, {20, 21}}));
    w.roll_back(during(4, 6));
    CHECK(w.state(cell).hollows == metres({{2, 6}, {10, 12}}));
}

TEST_CASE("openings are dug out or natural caves", "[planet][world]") {
    World& w = fresh_world();
    CHECK_FALSE(w.opening_at(cell, 5 * fixed_one));
    REQUIRE(w.apply(change(cell, Excavate{.depth = 2 * fixed_one})));
    REQUIRE(w.apply(change(cell, Hollow{.top = 4 * fixed_one, .bottom = 6 * fixed_one})));
    CHECK(w.opening_at(cell, fixed_one) == hog::planet::Opening{});
    CHECK_FALSE(w.opening_at(cell, 3 * fixed_one));
    CHECK(w.opening_at(cell, 5 * fixed_one) == hog::planet::Opening{});
    CHECK_FALSE(w.opening_at(cell, 6 * fixed_one));

    // A roomy stretch of lava tube, found through the world.
    bool found = false;
    for (const auto& tube : w.caves().lava_tubes()) {
        for (const auto& passage : tube.passages) {
            if (found || passage.from_radius < 2.0) {
                continue;
            }
            const CellId over = hog::planet::cell_at(glm::normalize(passage.from), 20);
            const std::int64_t depth =
                w.geology().terrain().elevation(over) -
                std::llround((glm::length(passage.from) - planet.radius) * fixed_one);
            if (w.geology().strata(over).rock_at(depth) != hog::planet::Rock::Basalt) {
                continue;
            }
            const auto opening = w.opening_at(over, depth);
            REQUIRE(opening);
            const auto cave = opening.value_or(hog::planet::Opening{}).cave;
            REQUIRE(cave);
            CHECK(cave.value_or(hog::planet::CaveHit{}).cave == hog::planet::Cave::LavaTube);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("replaying changes equals applying them one by one", "[planet][world]") {
    const std::vector<Change> batch{
        change(cell, SetElevation{.elevation = 10 * fixed_one}, 1),
        change(cell.child(1), Excavate{.depth = fixed_one}, 2),
        change(cell, SetBiome{.biome = Biome::Beach}, 3, {}, Layer::Simulation),
    };
    World& w = fresh_world();
    REQUIRE(w.replay(batch));
    const CellState replayed = w.state(cell.child(1));
    CHECK(w.changes() == batch);
    w.roll_back({});
    for (const Change& each : batch) {
        REQUIRE(w.apply(each));
    }
    CHECK(w.state(cell.child(1)) == replayed);
}

TEST_CASE("rolling back removes the matching changes", "[planet][world]") {
    constexpr AuthorId griefer{7};
    constexpr AuthorId builder{8};
    World& w = fresh_world();
    const CellId other = CellId::from_face_ij(4, 10, 20, 12);
    REQUIRE(w.apply(change(cell, SetElevation{.elevation = fixed_one}, 10, builder)));
    REQUIRE(w.apply(change(cell, Excavate{.depth = 5 * fixed_one}, 20, griefer)));
    REQUIRE(w.apply(change(other, Excavate{.depth = 5 * fixed_one}, 30, griefer)));
    REQUIRE(w.apply(change(other, Excavate{.depth = fixed_one}, 40, builder)));

    SECTION("by author") {
        CHECK(w.changes(by_author(griefer)).size() == 2);
        CHECK(w.roll_back(by_author(griefer)) == 2);
        CHECK(w.state(cell).elevation == fixed_one);
        CHECK(w.state(other).excavated == fixed_one);
        CHECK(w.size() == 2);
    }
    SECTION("by author and time range") {
        ChangeFilter filter = during(25, 35);
        filter.author = griefer;
        CHECK(w.roll_back(filter) == 1);
        CHECK(w.state(cell).excavated == 5 * fixed_one);
        CHECK(w.state(other).excavated == fixed_one);
    }
    SECTION("the range excludes its end") {
        CHECK(w.roll_back(during(10, 20)) == 1);
        CHECK(w.state(cell).elevation == seed_state(cell).elevation - (5 * fixed_one));
    }
    SECTION("by layer") {
        CHECK(w.roll_back(by_layer(Layer::Authored)) == 0);
        CHECK(w.roll_back(by_layer(Layer::Player)) == 4);
        CHECK(w.state(cell) == seed_state(cell));
        CHECK(w.state(other) == seed_state(other));
    }
}

TEST_CASE("rolling back the latest changes equals never making them", "[planet][world]") {
    static World rolled_back{planet};
    static World replayed{planet};
    // A few overlapping cells, so changes to parents and children interleave.
    const std::vector<CellId> cells{cell,          cell.child(0), cell.child(0).child(3),
                                    cell.child(2), cell.parent(), CellId::from_face_ij(5, 7, 9, 8)};
    for (std::uint64_t trial = 0; trial < 50; ++trial) {
        INFO("trial " << trial);
        const auto draw = [&](std::uint64_t index, std::uint64_t what, std::uint64_t range) {
            return hog::hash_values(trial, index, what) % range;
        };
        const auto count = static_cast<std::size_t>(draw(0, 0, 200));
        std::vector<Change> all;
        for (std::uint64_t index = 0; index < count; ++index) {
            const CellId target = cells.at(draw(index, 1, cells.size()));
            const std::int64_t amount =
                static_cast<std::int64_t>(draw(index, 2, 100) + 1) * fixed_one / 4;
            ChangePayload payload;
            switch (draw(index, 3, 3)) {
            case 0:
                payload = SetElevation{.elevation = amount - (12 * fixed_one)};
                break;
            case 1:
                payload = Excavate{.depth = amount};
                break;
            default:
                payload =
                    SetBiome{.biome = static_cast<Biome>(draw(index, 4, hog::planet::biome_count))};
            }
            all.push_back(change(target, payload, static_cast<std::int64_t>(draw(index, 5, 60)),
                                 AuthorId{draw(index, 6, 4)},
                                 static_cast<Layer>(draw(index, 7, hog::planet::layer_count))));
        }
        const auto cut = static_cast<std::int64_t>(draw(0, 8, 61));
        std::vector<Change> kept;
        for (const Change& each : all) {
            if (each.time < at(cut)) {
                kept.push_back(each);
            }
        }

        rolled_back.roll_back({});
        replayed.roll_back({});
        REQUIRE(rolled_back.replay(all));
        REQUIRE(replayed.replay(kept));
        ChangeFilter latest;
        latest.from = at(cut);
        CHECK(rolled_back.roll_back(latest) == all.size() - kept.size());
        CHECK(rolled_back.changes() == kept);
        for (const CellId target : cells) {
            for (const CellId probe : {target, target.child(1), target.parent()}) {
                CHECK(rolled_back.state(probe) == replayed.state(probe));
            }
        }
    }
}
