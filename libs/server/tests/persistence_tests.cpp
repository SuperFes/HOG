#include <hog/core/bytes.hpp>
#include <hog/planet/projection.hpp>
#include <hog/save/snapshot.hpp>
#include <hog/server/persistence.hpp>
#include <hog/server/spatial_index.hpp>
#include <hog/server/spatial_tracking.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <flecs.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using hog::ErrorCode;
using hog::WorldPosition;
using hog::save::Section;
using hog::save::section_id;
using hog::save::Snapshot;
using hog::server::Persistence;
using hog::server::PersistentId;
using hog::server::Position;

namespace {

// A component that only some tests register, standing in for one added in a later build.
struct Health {
    std::uint32_t points = 0;
};

Position at(double x) {
    return Position{.at = WorldPosition{x, 115'000.0, -0.5}};
}

Snapshot stored(const Persistence& persistence) {
    Snapshot snapshot;
    persistence.store(snapshot);
    return snapshot;
}

Section& section(Snapshot& snapshot, hog::save::SectionId id) {
    const auto found = std::ranges::find(snapshot.sections, id, &Section::id);
    REQUIRE(found != snapshot.sections.end());
    return *found;
}

// The live entity with this id, which the test requires.
flecs::entity found(const Persistence& persistence, std::uint64_t id) {
    const auto entity = persistence.find(id);
    REQUIRE(entity);
    return entity.value_or(flecs::entity{});
}

std::uint64_t persistent_count(const flecs::world& world) {
    return static_cast<std::uint64_t>(world.count<PersistentId>());
}

// Three saved entities, two with positions, and an unsaved one that also has a position.
Snapshot sample() {
    flecs::world world;
    Persistence persistence{world};
    persistence.persist<Position>();
    persistence.create().set<Position>(at(1.0));
    const flecs::entity doomed = persistence.create();
    persistence.create();
    persistence.create().set<Position>(at(-3.25));
    doomed.destruct();
    world.entity().set<Position>(at(99.0));
    return stored(persistence);
}

} // namespace

template <>
struct hog::server::Persist<Health> {
    static constexpr save::SectionId section = save::section_id("HLTH");
    static constexpr std::uint16_t version = 2;
    static void encode(const Health& health, ByteWriter& writer) { writer.write(health.points); }
    static Result<Health> decode(std::uint16_t stored, ByteReader& reader) {
        // Version 1 kept points in 16 bits.
        const auto points = stored == 1
                                ? reader.read<std::uint16_t>().transform(
                                      [](std::uint16_t value) -> std::uint32_t { return value; })
                                : reader.read<std::uint32_t>();
        return points.transform([](std::uint32_t value) { return Health{value}; });
    }
};

TEST_CASE("persistent entities get their own ids", "[server][persistence]") {
    flecs::world world;
    Persistence persistence{world};
    const flecs::entity first = persistence.create();
    const flecs::entity second = persistence.create();
    CHECK(first.get<PersistentId>().value == 1);
    CHECK(second.get<PersistentId>().value == 2);
    CHECK(found(persistence, 2).id() == second.id());
    CHECK(persistence.size() == 2);

    second.destruct();
    CHECK_FALSE(persistence.find(2));
    CHECK(persistence.size() == 1);
    // Ids aren't reused.
    CHECK(persistence.create().get<PersistentId>().value == 3);
    CHECK_FALSE(persistence.find(0));
}

TEST_CASE("entities and their components survive a restore", "[server][persistence]") {
    const Snapshot snapshot = sample();
    REQUIRE(snapshot.sections.size() == 2);

    hog::server::SpatialIndex index{11};
    flecs::world world;
    hog::server::track_positions(world, index);
    Persistence persistence{world};
    persistence.persist<Position>();
    CHECK(persistence.restore(snapshot) == 3U);
    CHECK(persistent_count(world) == 3);
    CHECK(index.size() == 2);

    CHECK_FALSE(persistence.find(2));
    CHECK(found(persistence, 1).get<Position>().at == at(1.0).at);
    CHECK_FALSE(found(persistence, 3).has<Position>());
    CHECK(found(persistence, 4).get<Position>().at == at(-3.25).at);
    CHECK(persistence.create().get<PersistentId>().value == 5);
}

TEST_CASE("storing is deterministic", "[server][persistence]") {
    const Snapshot snapshot = sample();
    CHECK(sample() == snapshot);

    flecs::world world;
    Persistence persistence{world};
    persistence.persist<Position>();
    REQUIRE(persistence.restore(snapshot));
    CHECK(stored(persistence) == snapshot);
    CHECK(hog::save::encode(stored(persistence)) == hog::save::encode(snapshot));
}

TEST_CASE("components come and go between builds", "[server][persistence]") {
    const Snapshot old_build = sample();

    // A component added since the snapshot is simply missing.
    flecs::world newer;
    Persistence added{newer};
    added.persist<Position>();
    added.persist<Health>();
    REQUIRE(added.restore(old_build) == 3U);
    CHECK_FALSE(found(added, 1).has<Health>());
    found(added, 1).set<Health>({500});
    const Snapshot with_health = stored(added);

    // A component no longer registered is ignored.
    flecs::world older;
    Persistence retired{older};
    REQUIRE(retired.restore(with_health) == 3U);
    CHECK_FALSE(found(retired, 1).has<Position>());

    // Older versions of a component still decode.
    Snapshot version_one = with_health;
    Section& health = section(version_one, section_id("HLTH"));
    health.version = 1;
    hog::ByteWriter writer;
    writer.write(std::uint64_t{1});
    writer.write(std::uint64_t{4});
    writer.write(std::uint16_t{77});
    health.bytes = std::move(writer).take();
    flecs::world migrated;
    Persistence upgraded{migrated};
    upgraded.persist<Health>();
    REQUIRE(upgraded.restore(version_one) == 3U);
    CHECK(found(upgraded, 4).get<Health>().points == 77);
}

TEST_CASE("an empty snapshot restores nothing", "[server][persistence]") {
    flecs::world world;
    Persistence persistence{world};
    persistence.persist<Position>();
    CHECK(persistence.restore(Snapshot{}) == 0U);
    CHECK(persistence.create().get<PersistentId>().value == 1);
}

TEST_CASE("a failed restore changes nothing", "[server][persistence]") {
    Snapshot snapshot = sample();
    auto expected_code = ErrorCode::Parse;

    SECTION("a truncated component") {
        section(snapshot, section_id("POSN")).bytes.pop_back();
    }
    SECTION("a component of an unsaved entity") {
        // The second record's id follows the count, the first id and its 24 position bytes.
        section(snapshot, section_id("POSN")).bytes[8 + 8 + 24] = std::byte{2};
    }
    SECTION("ids out of order") {
        section(snapshot, Persistence::entities_section).bytes[16] = std::byte{9};
    }
    SECTION("a zero id") {
        section(snapshot, Persistence::entities_section).bytes[16] = std::byte{0};
    }
    SECTION("too many ids") {
        section(snapshot, Persistence::entities_section).bytes[8] = std::byte{200};
    }
    SECTION("trailing bytes") {
        section(snapshot, Persistence::entities_section).bytes.push_back(std::byte{0});
    }
    SECTION("components without entities") {
        std::erase_if(snapshot.sections, [](const Section& entities) {
            return entities.id == Persistence::entities_section;
        });
    }
    SECTION("a newer entities section") {
        section(snapshot, Persistence::entities_section).version = 2;
        expected_code = ErrorCode::Unsupported;
    }
    SECTION("a newer component") {
        section(snapshot, section_id("POSN")).version = 2;
        expected_code = ErrorCode::Unsupported;
    }

    flecs::world world;
    Persistence persistence{world};
    persistence.persist<Position>();
    const auto restored = persistence.restore(snapshot);
    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == expected_code);
    CHECK(persistent_count(world) == 0);
    CHECK(persistence.size() == 0);
    CHECK(persistence.create().get<PersistentId>().value == 1);
}

TEST_CASE("persistence misuse is caught", "[server][persistence]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    flecs::world world;
    Persistence persistence{world};
    persistence.persist<Position>();
    CHECK_THROWS_AS(persistence.persist<Position>(), AssertionFailed);

    persistence.create();
    CHECK_THROWS_AS(static_cast<void>(persistence.restore(sample())), AssertionFailed);
}

TEST_CASE("persistence can end before its world", "[server][persistence]") {
    flecs::world world;
    flecs::entity survivor;
    {
        Persistence persistence{world};
        survivor = persistence.create();
    }
    survivor.destruct();
    CHECK_FALSE(survivor.is_alive());
}
