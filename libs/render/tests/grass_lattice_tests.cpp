#include "probe.hpp"
#include "test_device.hpp"

#include <hog/core/hash.hpp>
#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/render/grass_lattice.hpp>
#include <hog/shaders/grass_lattice_probe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

using hog::planet::FaceIJ;
using hog::render::grass_hash;
using hog::render::grass_lattice_level;
using hog::render::grass_spot;
using hog::render::standing_for;
using hog::render::stands_up;
using hog::render::testing::probed;
using hog::render::testing::Probed;
using hog::render::testing::test_device;

namespace {

constexpr double radius = 115'000.0;
constexpr std::uint32_t seed = 0x5eedU;
constexpr int lattice = 21;

// `count` cells at `level`, spread over the faces.
std::vector<FaceIJ> cells_at(int level, std::size_t count) {
    std::vector<FaceIJ> cells;
    const std::uint32_t mask = (std::uint32_t{1} << static_cast<unsigned>(level)) - 1U;
    for (std::size_t at = 0; at < count; ++at) {
        const std::uint64_t dice = hog::hash_values(7, at);
        cells.push_back({.face = static_cast<int>(dice % 6U),
                         .i = static_cast<std::uint32_t>(dice >> 8U) & mask,
                         .j = static_cast<std::uint32_t>(dice >> 36U) & mask,
                         .level = level});
    }
    return cells;
}

FaceIJ parent_of(FaceIJ cell) {
    return {.face = cell.face, .i = cell.i >> 1U, .j = cell.j >> 1U, .level = cell.level - 1};
}

} // namespace

TEST_CASE("the grass lattice is as fine as the thickest grass grows", "[grass]") {
    const int level = grass_lattice_level(radius);
    CHECK(level == lattice);
    const double across = (std::numbers::pi / 2.0) * radius / std::ldexp(1.0, level);
    CHECK(across <= hog::render::finest_grass_cell);
    CHECK(across > hog::render::finest_grass_cell / 2.0);
}

TEST_CASE("each world casts its grass lattice's dice its own way", "[grass]") {
    const hog::render::GrassLattice world = hog::render::grass_lattice(radius, 0x1234'5678'9abcULL);
    CHECK(world.level == lattice);
    CHECK(world.seed == hog::render::grass_lattice(radius, 0x1234'5678'9abcULL).seed);
    CHECK(world.seed != hog::render::grass_lattice(radius, 0x1234'5678'9abdULL).seed);
    // The high half counts too.
    CHECK(world.seed != hog::render::grass_lattice(radius, 0x1235'5678'9abcULL).seed);
}

TEST_CASE("a grass lattice cell's dice and spot are the same for ever", "[grass]") {
    const FaceIJ cell{.face = 3, .i = 123'456, .j = 1'654'321, .level = lattice};
    const std::uint32_t hash = grass_hash(cell, seed);
    CHECK(hash == 0x4e17'feeeU);
    CHECK(standing_for(parent_of(parent_of(cell)), lattice, seed) ==
          FaceIJ{.face = 3, .i = 123'458, .j = 1'654'323, .level = lattice});
    CHECK(stands_up(cell, seed, 8) == 0);
    const hog::Vec2 spot = grass_spot(hash);
    CHECK(spot.x == 33'396.0F / 65536.0F);
    CHECK(spot.y == 25'176.0F / 65536.0F);
    // Each part of where a cell is changes its dice.
    CHECK(grass_hash({.face = 2, .i = cell.i, .j = cell.j, .level = lattice}, seed) != hash);
    CHECK(grass_hash({.face = 3, .i = cell.i + 1, .j = cell.j, .level = lattice}, seed) != hash);
    CHECK(grass_hash({.face = 3, .i = cell.i, .j = cell.j + 1, .level = lattice}, seed) != hash);
    CHECK(grass_hash({.face = 3, .i = cell.i, .j = cell.j, .level = lattice - 1}, seed) != hash);
    CHECK(grass_hash(cell, seed + 1) != hash);
}

TEST_CASE("thinner grass keeps some of the blades thicker grass grows", "[grass]") {
    for (const FaceIJ& cell : cells_at(lattice - 4, 500)) {
        const FaceIJ standing = standing_for(cell, lattice, seed);
        CHECK(standing.level == lattice);
        CHECK(standing.face == cell.face);
        // Within the cell it stands for.
        CHECK(standing.i >> 4U == cell.i);
        CHECK(standing.j >> 4U == cell.j);
        CHECK(stands_up(standing, seed, 10) >= 4);
        // One of its children's, so a blade a thinner field keeps a thicker one grows too.
        bool among = false;
        for (std::uint32_t child = 0; child < 4; ++child) {
            const FaceIJ finer{.face = cell.face,
                               .i = (cell.i << 1U) | (child & 1U),
                               .j = (cell.j << 1U) | (child >> 1U),
                               .level = cell.level + 1};
            among = among || standing_for(finer, lattice, seed) == standing;
        }
        CHECK(among);
    }
    // And no further up than it stands for.
    CHECK(stands_up(standing_for(cells_at(lattice - 4, 1)[0], lattice, seed), seed, 4) == 4);
}

TEST_CASE("each level up, a blade stands for its cells about a quarter as often", "[grass]") {
    const std::vector<FaceIJ> cells = cells_at(lattice, 40'000);
    std::array<std::size_t, 5> at_least{};
    for (const FaceIJ& cell : cells) {
        const int up = stands_up(cell, seed, 4);
        for (int level = 0; level <= up; ++level) {
            ++at_least.at(static_cast<std::size_t>(level));
        }
    }
    CHECK(at_least[0] == cells.size());
    for (std::size_t level = 1; level < at_least.size(); ++level) {
        const double expected = static_cast<double>(cells.size()) / std::pow(4.0, level);
        CHECK(std::abs(static_cast<double>(at_least.at(level)) - expected) <
              (4.0 * std::sqrt(expected)) + 1.0);
    }
}

TEST_CASE("blades stand anywhere in their cells, evenly", "[grass]") {
    std::array<std::size_t, 16> squares{};
    const std::vector<FaceIJ> cells = cells_at(lattice, 16'000);
    for (const FaceIJ& cell : cells) {
        const hog::Vec2 spot = grass_spot(grass_hash(cell, seed));
        REQUIRE(spot.x >= 0.0F);
        REQUIRE(spot.x < 1.0F);
        REQUIRE(spot.y >= 0.0F);
        REQUIRE(spot.y < 1.0F);
        ++squares.at((static_cast<std::size_t>(spot.x * 4.0F) * 4U) +
                     static_cast<std::size_t>(spot.y * 4.0F));
    }
    for (const std::size_t in : squares) {
        CHECK(in > 850U);
        CHECK(in < 1150U);
    }
}

TEST_CASE("the GPU's grass lattice is the CPU's to the bit", "[grass][gpu]") {
    auto device = test_device();
    const std::vector<FaceIJ> cells = [] {
        std::vector<FaceIJ> all = cells_at(lattice, 300);
        for (const FaceIJ& coarse : cells_at(lattice - 5, 100)) {
            all.push_back(coarse);
        }
        return all;
    }();
    std::vector<Probed> asked;
    for (const FaceIJ& cell : cells) {
        asked.push_back({std::bit_cast<float>(static_cast<std::uint32_t>(cell.face)),
                         std::bit_cast<float>(cell.i), std::bit_cast<float>(cell.j),
                         std::bit_cast<float>(static_cast<std::uint32_t>(cell.level))});
        asked.push_back({std::bit_cast<float>(seed),
                         std::bit_cast<float>(static_cast<std::uint32_t>(lattice)),
                         std::bit_cast<float>(8U), 0.0F});
    }
    const std::vector<Probed> found =
        probed(*device, hog::shaders::grass_lattice_probe, asked, 2 * cells.size());
    for (std::size_t at = 0; at < cells.size(); ++at) {
        const FaceIJ& cell = cells[at];
        const std::uint32_t hash = grass_hash(cell, seed);
        const Probed& dice = found[2 * at];
        CHECK(std::bit_cast<std::uint32_t>(dice[0]) == hash);
        CHECK(dice[1] == static_cast<float>(stands_up(cell, seed, 8)));
        const hog::Vec2 spot = grass_spot(hash);
        CHECK(dice[2] == spot.x);
        CHECK(dice[3] == spot.y);
        const FaceIJ standing = standing_for(cell, lattice, seed);
        const Probed& stood = found[(2 * at) + 1];
        CHECK(std::bit_cast<std::uint32_t>(stood[0]) == static_cast<std::uint32_t>(standing.face));
        CHECK(std::bit_cast<std::uint32_t>(stood[1]) == standing.i);
        CHECK(std::bit_cast<std::uint32_t>(stood[2]) == standing.j);
        CHECK(std::bit_cast<std::uint32_t>(stood[3]) == static_cast<std::uint32_t>(standing.level));
    }
}
