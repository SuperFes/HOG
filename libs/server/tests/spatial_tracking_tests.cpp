#include <hog/planet/projection.hpp>
#include <hog/server/spatial_tracking.hpp>

#include <catch2/catch_test_macros.hpp>
#include <flecs.h>

using hog::DVec3;
using hog::WorldPosition;
using hog::server::Position;
using hog::server::SpatialIndex;

namespace {

constexpr int level = 11;

WorldPosition above(DVec3 direction) {
    return WorldPosition{glm::normalize(direction) * 115'000.0};
}

} // namespace

TEST_CASE("the index follows positions as they are set", "[server][spatial]") {
    SpatialIndex index{level};
    flecs::world world;
    hog::server::track_positions(world, index);

    const flecs::entity entity = world.entity().set<Position>({above({1.0, 0.0, 0.0})});
    CHECK(index.cell_of(entity.id()) == hog::planet::cell_at(above({1.0, 0.0, 0.0}), level));

    entity.set<Position>({above({0.0, 0.0, -1.0})});
    CHECK(index.cell_of(entity.id()) == hog::planet::cell_at(above({0.0, 0.0, -1.0}), level));

    entity.remove<Position>();
    CHECK_FALSE(index.cell_of(entity.id()));

    const flecs::entity doomed = world.entity().set<Position>({above({0.0, 1.0, 0.0})});
    CHECK(index.size() == 1);
    doomed.destruct();
    CHECK(index.size() == 0);
}

TEST_CASE("the index catches positions moved in place each frame", "[server][spatial]") {
    SpatialIndex index{level};
    flecs::world world;
    hog::server::track_positions(world, index);

    const flecs::entity walker = world.entity().set<Position>({above({1.0, 0.3, 0.2})});
    const flecs::entity statue = world.entity().set<Position>({above({-1.0, 0.3, 0.2})});
    world.prefab().set<Position>({above({1.0, 0.3, 0.2})});

    // 50 m east per frame: a few frames carry the walker over a cell boundary.
    const DVec3 east =
        glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, above({1.0, 0.3, 0.2}).metres()));
    world.system<Position>().kind(flecs::OnUpdate).each([&](flecs::entity e, Position& p) {
        if (e == walker) {
            p.at += east * 50.0;
        }
    });

    const auto start = index.cell_of(walker.id());
    REQUIRE(start);
    for (int frame = 0; frame < 6; ++frame) {
        world.progress();
        CHECK(index.cell_of(walker.id()) == hog::planet::cell_at(walker.get<Position>().at, level));
    }
    CHECK(index.cell_of(walker.id()) != start);
    CHECK(index.cell_of(statue.id()) == hog::planet::cell_at(above({-1.0, 0.3, 0.2}), level));
    CHECK(index.size() == 2);
}
