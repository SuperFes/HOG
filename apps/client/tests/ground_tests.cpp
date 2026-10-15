#include <hog/client/ground.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

using hog::client::GroundSlots;
using hog::planet::CellId;
using hog::terrain::Patch;
using hog::terrain::StreamFrame;

namespace {

// Hands out slots as render::TerrainPool does, noting what's staged.
struct FakePool {
    std::vector<std::uint32_t> free{2, 1, 0};
    std::set<std::uint32_t> held;
    std::vector<std::uint32_t> staged;

    std::optional<std::uint32_t> acquire() {
        if (free.empty()) {
            return std::nullopt;
        }
        const std::uint32_t slot = free.back();
        free.pop_back();
        held.insert(slot);
        return slot;
    }
    void release(std::uint32_t slot) {
        REQUIRE(held.erase(slot) == 1);
        free.push_back(slot);
    }
    hog::Result<> stage(std::uint32_t frame, std::uint32_t slot,
                        std::span<const hog::terrain::PatchVertex> /*vertices*/) {
        CHECK(frame == 1);
        CHECK(held.contains(slot));
        staged.push_back(slot);
        return {};
    }
};

static_assert(hog::client::SlotPool<FakePool>);
static_assert(hog::client::SlotPool<hog::render::TerrainPool>);

constexpr CellId face = CellId::from_face(0);

Patch patch(CellId cell) {
    return {.cell = cell, .origin = {}, .heights = {}, .vertices = {}};
}

StreamFrame arriving(const std::vector<CellId>& cells, std::vector<CellId> evicted = {}) {
    StreamFrame frame;
    frame.evicted = std::move(evicted);
    for (const CellId cell : cells) {
        frame.arrived.push_back(patch(cell));
    }
    return frame;
}

} // namespace

TEST_CASE("patches arriving get slots of their own, and give them back once let go",
          "[client][ground]") {
    FakePool pool;
    GroundSlots slots;
    REQUIRE(slots.take(pool, 1, arriving({face.child(0), face.child(1)})));
    CHECK(pool.staged == std::vector<std::uint32_t>{0, 1});
    CHECK(slots.slot(face.child(0)) == 0U);
    CHECK(slots.slot(face.child(1)) == 1U);
    CHECK_FALSE(slots.slot(face));

    // Letting one go frees its slot for the next to arrive in the same frame.
    pool.staged.clear();
    REQUIRE(slots.take(pool, 1, arriving({face.child(2)}, {face.child(0)})));
    CHECK(pool.staged == std::vector<std::uint32_t>{0});
    CHECK(slots.slot(face.child(2)) == 0U);
    CHECK_FALSE(slots.slot(face.child(0)));
    CHECK(slots.size() == 2);
}

TEST_CASE("a patch baked again goes into the slot it's in", "[client][ground]") {
    FakePool pool;
    GroundSlots slots;
    REQUIRE(slots.take(pool, 1, arriving({face})));
    REQUIRE(slots.take(pool, 1, arriving({face})));
    CHECK(pool.staged == std::vector<std::uint32_t>{0, 0});
    CHECK(pool.held.size() == 1);
}

TEST_CASE("patches arriving with no slot left fail", "[client][ground]") {
    FakePool pool;
    GroundSlots slots;
    CHECK_FALSE(slots.take(pool, 1,
                           arriving({face.child(0), face.child(1), face.child(2), face.child(3)})));
}

TEST_CASE("patches are drawn from their slots", "[client][ground]") {
    FakePool pool;
    GroundSlots slots;
    REQUIRE(slots.take(pool, 1, arriving({face.child(0), face.child(3)})));
    const hog::WorldPosition origin{1.0, 2.0, 3.0};
    const std::vector<hog::terrain::StreamedPatch> drawn{
        {.cell = face.child(3), .origin = origin, .coarser_edges = 5},
        {.cell = face.child(0), .origin = {}, .coarser_edges = 0}};
    const std::vector<hog::render::TerrainDraw> draws = slots.draws(drawn);
    REQUIRE(draws.size() == 2);
    CHECK(draws[0].slot == 1);
    CHECK(draws[0].origin == origin);
    CHECK(draws[0].coarser_edges == 5);
    CHECK(draws[1].slot == 0);
}

TEST_CASE("each biome is coloured as it looks from above", "[client][ground]") {
    const hog::render::BiomePalette palette = hog::client::biome_palette();
    for (const hog::Vec3 colour : palette) {
        CHECK(glm::all(glm::greaterThanEqual(colour, hog::Vec3{0.0F})));
        CHECK(glm::all(glm::lessThanEqual(colour, hog::Vec3{1.0F})));
    }
    const auto of = [&](hog::planet::Biome biome) {
        return palette.at(static_cast<std::size_t>(biome));
    };
    CHECK(of(hog::planet::Biome::Ocean).b > of(hog::planet::Biome::Ocean).r);
    CHECK(of(hog::planet::Biome::Grassland).g > of(hog::planet::Biome::Grassland).b);
    CHECK(of(hog::planet::Biome::Ice) != of(hog::planet::Biome::Desert));
}
