#include <hog/client/ground.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
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
    return {.cell = cell, .origin = {}, .heights = {}, .coarser_error = 0.0, .vertices = {}};
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

TEST_CASE("grass grows on fine ground near the eye, thinning with distance", "[client][ground]") {
    constexpr double radius = 115'000.0;
    // Vertex spacing ~0.69 m at level 13 on this planet; a level up is too coarse for grass.
    const CellId fine = CellId::from_face_ij(0, 0, 0, 13);
    const CellId fine_far = CellId::from_face_ij(0, 1, 0, 13);
    const CellId coarse = CellId::from_face_ij(0, 0, 0, 12);
    FakePool pool;
    GroundSlots slots;
    REQUIRE(slots.take(pool, 1, arriving({fine, fine_far, coarse})));
    const hog::WorldPosition eye{radius, 0.0, 0.0};
    const auto patch = [&](CellId cell, double away) {
        return hog::terrain::StreamedPatch{
            .cell = cell, .origin = hog::WorldPosition{radius, away, 0.0}, .coarser_edges = 0};
    };
    const std::vector<hog::terrain::StreamedPatch> drawn{patch(fine, 0.0), patch(fine_far, 60.0),
                                                         patch(coarse, 0.0)};

    const std::vector<hog::render::GrassDraw> grown =
        hog::client::grass_draws(slots, drawn, eye, radius);
    std::vector<hog::render::GrassDraw> lawn;
    std::vector<hog::render::GrassDraw> swaths;
    for (const hog::render::GrassDraw& draw : grown) {
        (draw.tall_only ? swaths : lawn).push_back(draw);
    }
    // Only the fine patches grow the lawn, and the far one fewer blades than the near.
    REQUIRE(lawn.size() == 2);
    CHECK(lawn[0].slot == 0);
    CHECK(lawn[1].slot == 1);
    CHECK(lawn[0].blades_per_cell >= 1);
    CHECK(lawn[1].blades_per_cell >= 1);
    CHECK(lawn[1].blades_per_cell < lawn[0].blades_per_cell);
    CHECK(lawn[0].spacing < 0.75);
    CHECK(lawn[0].fade_far == hog::render::grass_far);
    // Every patch this fine also grows a far swath, seeded apart from its lawn.
    REQUIRE(swaths.size() == 3);
    CHECK(swaths[2].slot == 2);
    CHECK(swaths[2].fade_far > hog::render::grass_far);
    CHECK(swaths[0].seed != lawn[0].seed);
    // The same cell seeds the same grass on every machine.
    CHECK(lawn[0].seed == hog::client::grass_draws(slots, drawn, eye, radius)[0].seed);
    CHECK(lawn[0].seed != lawn[1].seed);

    // Out of reach altogether, nothing grows.
    const std::vector<hog::terrain::StreamedPatch> far_off{patch(fine, 300.0)};
    CHECK(hog::client::grass_draws(slots, far_off, eye, radius).empty());
}

TEST_CASE("the lawn reaches as far as the budget has it", "[client][ground]") {
    constexpr double radius = 115'000.0;
    const CellId fine = CellId::from_face_ij(0, 1, 0, 13);
    FakePool pool;
    GroundSlots slots;
    REQUIRE(slots.take(pool, 1, arriving({fine})));
    const hog::WorldPosition eye{radius, 0.0, 0.0};
    const std::vector<hog::terrain::StreamedPatch> drawn{
        {.cell = fine, .origin = hog::WorldPosition{radius, 90.0, 0.0}, .coarser_edges = 0}};
    const auto lawn = [&](const hog::client::GrassBudget& budget) {
        std::vector<hog::render::GrassDraw> found;
        for (const hog::render::GrassDraw& draw :
             hog::client::grass_draws(slots, drawn, eye, radius, budget)) {
            if (!draw.tall_only) {
                found.push_back(draw);
            }
        }
        return found;
    };
    // Out of the default lawn's reach, but in a longer one's, thinning towards its end.
    CHECK(lawn({}).empty());
    const std::vector<hog::render::GrassDraw> longer = lawn({.lawn_near = 40.0, .lawn_far = 120.0});
    REQUIRE(longer.size() == 1);
    CHECK(longer[0].fade_far == 120.0);
    const std::vector<hog::render::GrassDraw> held = lawn({.lawn_near = 110.0, .lawn_far = 120.0});
    REQUIRE(held.size() == 1);
    CHECK(longer[0].blades_per_cell < held[0].blades_per_cell);
}

TEST_CASE("tall swaths reach far over coarser ground, thinning as they go", "[client][ground]") {
    constexpr double radius = 115'000.0;
    // Vertex spacing ~5.5 m at level 10 on this planet; a level up is too coarse even for swaths.
    const CellId coarse = CellId::from_face_ij(0, 0, 0, 10);
    const CellId coarse_far = CellId::from_face_ij(0, 1, 0, 10);
    const CellId coarsest = CellId::from_face_ij(0, 0, 0, 9);
    FakePool pool;
    GroundSlots slots;
    REQUIRE(slots.take(pool, 1, arriving({coarse, coarse_far, coarsest})));
    const hog::WorldPosition eye{radius, 0.0, 0.0};
    const auto patch = [&](CellId cell, double away) {
        return hog::terrain::StreamedPatch{
            .cell = cell, .origin = hog::WorldPosition{radius, away, 0.0}, .coarser_edges = 0};
    };
    const std::vector<hog::terrain::StreamedPatch> drawn{
        patch(coarse, 0.0), patch(coarse_far, 400.0), patch(coarsest, 0.0)};
    const std::vector<hog::render::GrassDraw> swaths =
        hog::client::grass_draws(slots, drawn, eye, radius);
    // Only tall grass grows this coarse, and the far patch thinner than the near.
    REQUIRE(swaths.size() == 2);
    CHECK(swaths[0].tall_only);
    CHECK(swaths[1].tall_only);
    CHECK(swaths[0].slot == 0);
    CHECK(swaths[1].slot == 1);
    CHECK(swaths[1].blades_per_cell >= 1);
    CHECK(swaths[1].blades_per_cell < swaths[0].blades_per_cell);
}

TEST_CASE("each biome is coloured as it looks from above", "[client][ground]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    for (const hog::Vec3 colour : palette) {
        CHECK(glm::all(glm::greaterThanEqual(colour, hog::Vec3{0.0F})));
        CHECK(glm::all(glm::lessThanEqual(colour, hog::Vec3{1.0F})));
    }
    const auto of = [&](hog::planet::Biome biome) {
        return palette.at(static_cast<std::size_t>(biome));
    };
    // Sand under the sea, darker for being wet.
    CHECK(of(hog::planet::Biome::Ocean).r > of(hog::planet::Biome::Ocean).b);
    CHECK(glm::all(glm::lessThan(of(hog::planet::Biome::Ocean), of(hog::planet::Biome::Beach))));
    CHECK(of(hog::planet::Biome::Grassland).g > of(hog::planet::Biome::Grassland).b);
    CHECK(of(hog::planet::Biome::Ice) != of(hog::planet::Biome::Desert));
}

TEST_CASE("each biome's ground varies up close and averages to its colour", "[client][ground]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const auto looks = hog::client::biome_looks();
    for (std::size_t biome = 0; biome < looks.size(); ++biome) {
        INFO(hog::planet::to_string(static_cast<hog::planet::Biome>(biome)));
        const hog::terrain::Look& look = looks.at(biome);
        CHECK(hog::Vec3{look.colour} == palette.at(biome));
        // Any patches stand out.
        CHECK((look.accent_share == 0.0 || look.accent != look.colour));
        CHECK(look.grain > 0.0);
        const hog::terrain::MaterialImage material = hog::terrain::material_image(look, 64);
        const std::vector<std::uint8_t>& largest = material.levels.front();
        CHECK(std::ranges::any_of(std::views::iota(std::size_t{0}, largest.size() / 4),
                                  [&](std::size_t texel) {
                                      return largest.at(4 * texel) != largest.at(0) ||
                                             largest.at((4 * texel) + 1) != largest.at(1);
                                  }));
    }
    // Every look is its own.
    for (std::size_t biome = 1; biome < looks.size(); ++biome) {
        CHECK(looks.at(biome).seed != looks.at(biome - 1).seed);
    }
}
