#include <hog/planet/ground_column.hpp>
#include <hog/planet/tile.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

using hog::planet::CellId;
using hog::planet::Elevation;
using hog::planet::fixed_one;
using hog::planet::Material;
using hog::planet::Rock;
using hog::planet::Run;

namespace {

const Elevation& terrain() {
    static const Elevation evolved{{.seed = 7,
                                    .ocean_fraction = 0.7,
                                    .radius = 115'000.0,
                                    .erosion_cell = hog::testing::coarse_erosion_cell,
                                    .generator = hog::planet::Generator::Evolved}};
    return evolved;
}

std::vector<CellId> cells() {
    return hog::planet::sample_tile(CellId::from_face(4), 7, [](CellId cell) { return cell; });
}

// The material at `height` in runs starting at `low`.
Material material_in(const std::vector<Run>& runs, std::int64_t height) {
    const auto run = std::ranges::find_if(runs, [&](const Run& each) { return height < each.top; });
    return run == runs.end() ? Material::Air : run->material;
}

} // namespace

TEST_CASE("every rock is held as its own material", "[planet][ground_column]") {
    std::set<Material> held;
    for (int rock = 0; rock < hog::planet::rock_count; ++rock) {
        held.insert(hog::planet::material_of(static_cast<Rock>(rock)));
    }
    CHECK(held.size() == static_cast<std::size_t>(hog::planet::rock_count));
    CHECK(hog::planet::material_of(Rock::Shale) == Material::Mudstone);
    CHECK(hog::planet::material_of(Rock::Peridotite) == Material::Mantle);
}

TEST_CASE("a column runs upward from rock to air, each run unlike the one under it",
          "[planet][ground_column]") {
    for (const CellId cell : cells()) {
        const std::int64_t ground = terrain().elevation(cell);
        const std::int64_t low = ground - (3'000 * fixed_one);
        const std::int64_t high = std::max<std::int64_t>(ground, 0) + (500 * fixed_one);
        const std::vector<Run> runs = hog::planet::ground_column(terrain(), cell, low, high);
        REQUIRE(!runs.empty());
        CHECK(runs.back().top == high);
        CHECK(runs.back().material == Material::Air);
        std::int64_t under = low;
        for (std::size_t index = 0; index < runs.size(); ++index) {
            CHECK(runs[index].top > under);
            under = runs[index].top;
            if (index > 0) {
                CHECK(runs[index].material != runs[index - 1].material);
            }
        }
    }
}

TEST_CASE("a column holds the beds' rock, the soil, the water and the air where the terrain has "
          "them",
          "[planet][ground_column]") {
    std::set<Material> seen;
    for (const CellId cell : cells()) {
        const std::int64_t ground = terrain().elevation(cell);
        const std::int64_t bedrock = ground - terrain().soil(cell);
        std::int64_t water = ground;
        if (const auto standing = terrain().rivers().standing(cell, ground)) {
            water = std::max(water, standing->level);
        }
        if (const auto river = terrain().river(cell)) {
            water = std::max(water, river->surface);
        }
        const auto layering = terrain().layering(cell);
        const std::int64_t low = bedrock - (2'000 * fixed_one);
        const std::int64_t high = std::max(water, ground) + (200 * fixed_one);
        const std::vector<Run> runs = hog::planet::ground_column(terrain(), cell, low, high);
        for (std::int64_t height = low + (fixed_one / 2); height < high; height += 37 * fixed_one) {
            Material expected = Material::Air;
            if (height < bedrock) {
                expected = hog::planet::material_of(
                    terrain().beds().rock_at(layering.column, height - layering.risen));
            } else if (height < ground) {
                expected = Material::Soil;
            } else if (height < water) {
                expected = Material::Water;
            }
            INFO("height " << hog::planet::fixed_to_double(height) << " ground "
                           << hog::planet::fixed_to_double(ground));
            CHECK(material_in(runs, height) == expected);
            seen.insert(expected);
        }
    }
    // The sample reaches down through soil into layered rock, and over sea.
    CHECK(seen.contains(Material::Soil));
    CHECK(seen.contains(Material::Water));
    CHECK(seen.contains(Material::Basalt));
    CHECK(seen.size() >= 5);
}

TEST_CASE("a column wholly in the air is one run of it", "[planet][ground_column]") {
    const CellId cell = cells().front();
    const std::int64_t above =
        std::max<std::int64_t>(terrain().elevation(cell), 0) + (10'000 * fixed_one);
    CHECK(hog::planet::ground_column(terrain(), cell, above, above + (100 * fixed_one)) ==
          std::vector<Run>{{.material = Material::Air, .top = above + (100 * fixed_one)}});
}
