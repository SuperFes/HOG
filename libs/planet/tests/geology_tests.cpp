#include <hog/core/hash.hpp>
#include <hog/planet/geology.hpp>
#include <hog/planet/hotspots.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/tile.hpp>
#include <hog/testing/quick.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <vector>

using hog::planet::Biome;
using hog::planet::CellId;
using hog::planet::fixed_one;
using hog::planet::Geology;
using hog::planet::Resource;
using hog::planet::Rock;
using hog::planet::Strata;

namespace {

constexpr double radius = 115'000.0;

hog::planet::PlanetConfig planet(std::uint64_t seed) {
    return {.seed = seed,
            .ocean_fraction = 0.7,
            .radius = radius,
            .erosion_cell = hog::testing::coarse_erosion_cell};
}

std::vector<CellId> cells_at(int level) {
    std::vector<CellId> cells;
    for (int face = 0; face < hog::planet::face_count; ++face) {
        const auto tile = hog::planet::sample_tile(CellId::from_face(face), level,
                                                   [](CellId cell) { return cell; });
        cells.insert(cells.end(), tile.begin(), tile.end());
    }
    return cells;
}

// Cells scattered over the planet, about 90 m across: finer than any deposit.
std::vector<CellId> scattered(std::uint64_t count) {
    constexpr int level = 11;
    constexpr std::uint64_t side = std::uint64_t{1} << level;
    std::vector<CellId> cells;
    cells.reserve(count);
    for (std::uint64_t sample = 0; sample < count; ++sample) {
        cells.push_back(CellId::from_face_ij(
            static_cast<int>(hog::hash_values(sample, 0) % hog::planet::face_count),
            static_cast<std::uint32_t>(hog::hash_values(sample, 1) % side),
            static_cast<std::uint32_t>(hog::hash_values(sample, 2) % side), level));
    }
    return cells;
}

constexpr std::size_t index(Resource resource) {
    return static_cast<std::size_t>(resource);
}

[[nodiscard]] bool sedimentary(Rock rock) {
    return rock == Rock::Sandstone || rock == Rock::Shale || rock == Rock::Limestone;
}

// Each resource's seam lies in the rock it forms in.
void check_host(Resource resource, const hog::planet::Deposit& deposit, const Strata& strata) {
    switch (resource) {
    case Resource::Clay:
        REQUIRE(deposit.top == strata.cover_thickness);
        break;
    case Resource::Coal: {
        const auto bed = strata.bed_at(deposit.top);
        REQUIRE(bed.has_value());
        REQUIRE((bed->rock == Rock::Shale || bed->rock == Rock::Sandstone));
        REQUIRE(strata.bed_at(deposit.top + deposit.thickness - 1)->index == bed->index);
        break;
    }
    case Resource::Salt:
        REQUIRE(deposit.top + deposit.thickness <= strata.bedrock_depth());
        REQUIRE(sedimentary(strata.rock_at(deposit.top)));
        break;
    case Resource::Tin:
    case Resource::Gold:
        REQUIRE(strata.rock_at(deposit.top) == Rock::Granite);
        [[fallthrough]];
    case Resource::Copper:
        REQUIRE(deposit.top >= strata.bedrock_depth());
        break;
    case Resource::Iron:
        break;
    }
}

} // namespace

TEST_CASE("strata stack from the surface down", "[planet][geology]") {
    constexpr Strata strata{.cover = Rock::Soil,
                            .cover_thickness = 2 * fixed_one,
                            .sediment = Rock::Limestone,
                            .sediment_thickness = 100 * fixed_one,
                            .bedrock = Rock::Granite};
    static_assert(strata.rock_at(0) == Rock::Soil);
    static_assert(strata.rock_at((2 * fixed_one) - 1) == Rock::Soil);
    static_assert(strata.rock_at(2 * fixed_one) == Rock::Limestone);
    static_assert(strata.rock_at(102 * fixed_one) == Rock::Granite);
    static_assert(strata.rock_at(10'000 * fixed_one) == Rock::Granite);
    static_assert(strata.bedrock_depth() == 102 * fixed_one);

    constexpr Strata crust{.cover = Rock::Soil,
                           .cover_thickness = 2 * fixed_one,
                           .sediment = Rock::Limestone,
                           .sediment_thickness = 100 * fixed_one,
                           .bedrock = Rock::Granite,
                           .mantle_depth = 30'000 * fixed_one};
    static_assert(crust.rock_at((30'000 * fixed_one) - 1) == Rock::Granite);
    static_assert(crust.rock_at(30'000 * fixed_one) == Rock::Peridotite);
    static_assert(crust.rock_at(100'000 * fixed_one) == Rock::Peridotite);

    constexpr Strata molten{.bedrock = Rock::Basalt,
                            .mantle_depth = 12'000 * fixed_one,
                            .chamber_top = 4'000 * fixed_one,
                            .chamber_thickness = 1'000 * fixed_one};
    static_assert(molten.rock_at((4'000 * fixed_one) - 1) == Rock::Basalt);
    static_assert(molten.rock_at(4'000 * fixed_one) == Rock::Magma);
    static_assert(molten.rock_at((5'000 * fixed_one) - 1) == Rock::Magma);
    static_assert(molten.rock_at(5'000 * fixed_one) == Rock::Basalt);

    constexpr Strata bare{.cover = Rock::Soil,
                          .cover_thickness = 0,
                          .sediment = Rock::Shale,
                          .sediment_thickness = 0,
                          .bedrock = Rock::Basalt};
    static_assert(bare.rock_at(0) == Rock::Basalt);
}

TEST_CASE("the rarest seam at a depth is the one found there", "[planet][geology]") {
    std::array<hog::planet::Deposit, hog::planet::resource_count> deposits{};
    deposits.at(index(Resource::Clay)) = {.top = 0, .thickness = 10 * fixed_one};
    deposits.at(index(Resource::Gold)) = {.top = 5 * fixed_one, .thickness = fixed_one};
    CHECK(hog::planet::resource_at(deposits, 0) == Resource::Clay);
    CHECK(hog::planet::resource_at(deposits, 5 * fixed_one) == Resource::Gold);
    CHECK(hog::planet::resource_at(deposits, 6 * fixed_one) == Resource::Clay);
    CHECK_FALSE(hog::planet::resource_at(deposits, 10 * fixed_one));
    CHECK_FALSE(hog::planet::resource_at({}, 0));
    CHECK(std::ranges::is_permutation(hog::planet::rarest_first,
                                      std::array{Resource::Clay, Resource::Coal, Resource::Salt,
                                                 Resource::Iron, Resource::Copper, Resource::Tin,
                                                 Resource::Gold}));
}

TEST_CASE("every rock and resource has a distinct name", "[planet][geology]") {
    std::set<std::string_view> rocks;
    for (int rock = 0; rock < hog::planet::rock_count; ++rock) {
        rocks.insert(to_string(static_cast<Rock>(rock)));
    }
    CHECK(rocks.size() == hog::planet::rock_count);
    CHECK_FALSE(rocks.contains("unknown"));
    std::set<std::string_view> resources;
    for (int resource = 0; resource < hog::planet::resource_count; ++resource) {
        resources.insert(to_string(static_cast<Resource>(resource)));
    }
    CHECK(resources.size() == hog::planet::resource_count);
    CHECK_FALSE(resources.contains("unknown"));
}

TEST_CASE("continents stand on granite and the deep ocean on basalt", "[planet][geology]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(2), values({1U, 2U})));
    const Geology geology{planet(seed)};
    std::set<Rock> sediments;
    std::set<Rock> shelf_sediments;
    for (const CellId cell : cells_at(6)) {
        const Strata strata = geology.strata(cell);
        REQUIRE(strata.cover_thickness >= 0);
        REQUIRE(strata.sediment_thickness >= 0);
        const bool land = geology.terrain().is_land(cell);
        if (geology.terrain().continents().is_land(cell)) {
            REQUIRE(geology.continental(cell));
        }
        if (!geology.continental(cell)) {
            REQUIRE(strata.bedrock == Rock::Basalt);
            if (!land) {
                REQUIRE(strata.cover == Rock::Sand);
            }
        } else if (land && geology.terrain().continents().is_land(cell)) {
            REQUIRE(strata.bedrock == Rock::Granite);
            if (strata.sediment_thickness > 0) {
                sediments.insert(strata.sediment);
            }
        } else if (!land) {
            // The shelf: the continent's rock under sea sand.
            REQUIRE(strata.bedrock == Rock::Granite);
            REQUIRE(strata.cover == Rock::Sand);
            if (strata.sediment_thickness > 0) {
                shelf_sediments.insert(strata.sediment);
            }
        }
    }
    CHECK(sediments == std::set{Rock::Sandstone, Rock::Shale, Rock::Limestone});
    CHECK(shelf_sediments == sediments);
}

TEST_CASE("shelves end partway down to the ocean floor", "[planet][geology]") {
    const Geology geology{planet(1)};
    int shelf = 0;
    int deep = 0;
    for (const CellId cell : cells_at(7)) {
        const std::int64_t height = geology.terrain().continents().height(cell);
        if (geology.terrain().is_land(cell)) {
            continue;
        }
        // Clear of the wandering edge, on either side.
        if (height > -fixed_one / 40) {
            REQUIRE(geology.continental(cell));
            ++shelf;
        } else if (height < -fixed_one * 3 / 40) {
            REQUIRE_FALSE(geology.continental(cell));
            ++deep;
        }
    }
    CHECK(shelf > 0);
    CHECK(deep > shelf);
}

TEST_CASE("the crust is thick under continents and mountains and thin under the sea",
          "[planet][geology]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(2), values({1U, 2U})));
    const Geology geology{planet(seed)};
    const auto km = [](std::int64_t depth) { return static_cast<double>(depth) / fixed_one / 1e3; };
    double thinnest_land = 1e9;
    double thickest_abyss = 0.0;
    double mountain = 0.0;
    double mountain_count = 0.0;
    double lowland = 0.0;
    double lowland_count = 0.0;
    for (const CellId cell : cells_at(6)) {
        const Strata strata = geology.strata(cell);
        REQUIRE(strata.mantle_depth == geology.crust_thickness(cell));
        REQUIRE(strata.mantle_depth > strata.bedrock_depth());
        REQUIRE(strata.rock_at(strata.mantle_depth) == Rock::Peridotite);
        REQUIRE(strata.rock_at(strata.mantle_depth - 1) == strata.bedrock);
        const double crust = km(strata.mantle_depth);
        REQUIRE(crust >= 6.3);
        REQUIRE(crust <= 66.0);
        const std::int64_t continent = geology.terrain().continents().height(cell);
        if (continent > fixed_one / 20) {
            thinnest_land = std::min(thinnest_land, crust);
            const std::int64_t strength = geology.terrain().mountain_strength(cell);
            if (strength == 0) {
                lowland += crust;
                lowland_count += 1.0;
            } else if (strength > fixed_one / 2) {
                mountain += crust;
                mountain_count += 1.0;
            }
        } else if (continent < -fixed_one * 3 / 20 && geology.terrain().height(cell) == continent) {
            thickest_abyss = std::max(thickest_abyss, crust);
        }
    }
    CHECK(thinnest_land >= 31.5);
    CHECK(thickest_abyss <= 7.7);
    REQUIRE(mountain_count > 0.0);
    CHECK(mountain / mountain_count > (lowland / lowland_count) + 10.0);
}

TEST_CASE("the crust thickens smoothly across the shelves", "[planet][geology]") {
    const Geology geology{planet(1)};
    constexpr int level = 9;
    const std::vector<CellId> cells = cells_at(level);
    std::int64_t steepest = 0;
    for (std::size_t at = 0; at < cells.size(); at += hog::testing::full_or<std::size_t>(1, 5)) {
        const CellId cell = cells[at];
        if (geology.terrain().is_land(cell) || geology.terrain().mountain_strength(cell) > 0) {
            continue;
        }
        const std::int64_t here = geology.crust_thickness(cell);
        for (const CellId neighbor : hog::planet::edge_neighbors(cell)) {
            steepest = std::max(steepest, std::abs(geology.crust_thickness(neighbor) - here));
        }
    }
    // It thickens as steeply as the sea floor rises, with no jump at the shelf's edge, where
    // the crust under the ocean floor and the continents differ by 28 km.
    INFO("steepest step " << static_cast<double>(steepest) / fixed_one << " m");
    CHECK(steepest < 5'000 * fixed_one);
}

TEST_CASE("mountains are stripped down to granite", "[planet][geology]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(2), values({1U, 2U})));
    const Geology geology{planet(seed)};
    double lowland = 0.0;
    double lowland_count = 0.0;
    double mountain = 0.0;
    double mountain_count = 0.0;
    int exposed = 0;
    for (const CellId cell : cells_at(6)) {
        if (!geology.terrain().continents().is_land(cell)) {
            continue;
        }
        const Strata strata = geology.strata(cell);
        const std::int64_t strength = geology.terrain().mountain_strength(cell);
        const auto sediment = static_cast<double>(strata.sediment_thickness);
        if (strength == 0) {
            lowland += sediment;
            lowland_count += 1.0;
        } else if (strength > fixed_one / 2) {
            mountain += sediment;
            mountain_count += 1.0;
        }
        if (strata.rock_at(fixed_one) == Rock::Granite) {
            ++exposed;
        }
    }
    REQUIRE(mountain_count > 0.0);
    // Belts lift the granite, but fold the beds deep into it as well.
    CHECK(mountain / mountain_count < 0.5 * lowland / lowland_count);
    CHECK(exposed > 0);
}

TEST_CASE("the ground under the cover cuts down through the beds", "[planet][geology]") {
    const Geology geology{planet(1)};
    const auto& beds = geology.terrain().beds();
    int columns = 0;
    int mixed = 0;
    for (const CellId cell : cells_at(6)) {
        const Strata strata = geology.strata(cell);
        if (strata.beds == nullptr) {
            CHECK(strata.bedrock == Rock::Basalt);
            continue;
        }
        REQUIRE(strata.beds == &beds);
        if (strata.sediment_thickness == 0) {
            CHECK(strata.rock_at(strata.cover_thickness) == Rock::Granite);
            continue;
        }
        ++columns;
        CHECK(strata.rock_at(strata.cover_thickness) == strata.sediment);
        CHECK(strata.rock_at(strata.bedrock_depth()) == strata.bedrock);
        std::set<Rock> rocks;
        for (std::int64_t depth = strata.cover_thickness; depth < strata.bedrock_depth();
             depth += 20 * fixed_one) {
            const Rock rock = strata.rock_at(depth);
            REQUIRE(rock == beds.rock_at(strata.column, strata.surface - depth));
            REQUIRE(sedimentary(rock));
            const auto bed = strata.bed_at(depth);
            REQUIRE(bed.has_value());
            REQUIRE(bed->rock == rock);
            rocks.insert(rock);
        }
        mixed += rocks.size() > 1 ? 1 : 0;
    }
    REQUIRE(columns > 100);
    // Beds tens of metres thick, so most columns hold more than one rock.
    CHECK(mixed > columns / 2);
}

TEST_CASE("the cover is the soil the land evolved, thin on bare rock", "[planet][geology]") {
    const Geology geology{planet(1)};
    const auto& terrain = geology.terrain();
    int covered = 0;
    int bare = 0;
    int deep = 0;
    const auto& grid = terrain.landform().grid();
    for (std::uint32_t index = 0; index < grid.size(); index += 3) {
        const CellId cell = grid.cell(index);
        const Biome biome = geology.climate().biome(cell);
        if (!terrain.is_land(cell) || biome == Biome::Beach || biome == Biome::Desert ||
            biome == Biome::Ice) {
            continue;
        }
        const Strata strata = geology.strata(cell);
        REQUIRE(strata.cover == Rock::Soil);
        REQUIRE(strata.cover_thickness <= terrain.soil(cell));
        const std::int64_t shown = terrain.exposure(cell, terrain.elevation(cell)).bare;
        if (shown > fixed_one / 2) {
            REQUIRE(strata.cover_thickness <= (terrain.soil(cell) / 2) + 1);
            ++bare;
        } else if (shown == 0) {
            REQUIRE(strata.cover_thickness == terrain.soil(cell));
            ++covered;
            deep += strata.cover_thickness > 2 * fixed_one ? 1 : 0;
        }
    }
    INFO(covered << " covered, " << bare << " bare, " << deep << " deep");
    CHECK(covered > 100);
    CHECK(bare > 0);
    CHECK(deep > 0);
}

TEST_CASE("volcanic islands are bare basalt under their soil", "[planet][geology]") {
    const Geology geology{planet(1)};
    int found = 0;
    for (const CellId cell : cells_at(7)) {
        if (geology.terrain().continents().is_land(cell) || !geology.terrain().is_land(cell)) {
            continue;
        }
        ++found;
        const Strata strata = geology.strata(cell);
        CHECK(strata.bedrock == Rock::Basalt);
        CHECK(strata.sediment_thickness == 0);
    }
    CHECK(found > 0);
}

TEST_CASE("a magma chamber lies in the crust under each arc volcano and each young shield",
          "[planet][geology]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(4), values({1U, 2U, 3U, 4U})));
    const Geology geology{planet(seed)};
    const auto volcanoes = geology.terrain().volcanoes().volcanoes();
    const auto shields = geology.terrain().hotspots().shields();
    const auto chambers = geology.chambers();
    const std::size_t plumes = geology.terrain().hotspots().hotspots().size();
    REQUIRE(chambers.size() == volcanoes.size() + plumes);
    const auto in_crust = [&](const hog::planet::Chamber& chamber) {
        const CellId cell = hog::planet::cell_at(chamber.center, 12);
        const Strata strata = geology.strata(cell);
        CHECK(strata.chamber_thickness > 990 * fixed_one);
        CHECK(strata.chamber_top >= 2'000 * fixed_one);
        CHECK(strata.chamber_top + strata.chamber_thickness < strata.mantle_depth);
        CHECK(strata.rock_at(strata.chamber_top - 1) != Rock::Magma);
    };
    for (std::size_t index = 0; index < volcanoes.size(); ++index) {
        const auto& chamber = chambers[index];
        CHECK_FALSE(chamber.basaltic);
        CHECK(glm::length(chamber.center - volcanoes[index].center) < 1e-12);
        CHECK(chamber.radius < volcanoes[index].radius / 2.0);
        in_crust(chamber);
    }
    for (std::size_t plume = 0; plume < plumes; ++plume) {
        const auto& chamber = chambers[volcanoes.size() + plume];
        CHECK(chamber.basaltic);
        CHECK(glm::length(chamber.center -
                          shields[plume * hog::planet::Hotspots::chain_length].center) < 1e-12);
        in_crust(chamber);
    }
}

TEST_CASE("magma lies only under the middle of volcanoes", "[planet][geology]") {
    const Geology geology{planet(1)};
    const auto volcanoes = geology.terrain().volcanoes().volcanoes();
    const auto shields = geology.terrain().hotspots().shields();
    const auto under = [&](CellId cell) { return geology.strata(cell).chamber_thickness > 0; };
    const auto central = [&](hog::DVec3 at) {
        return std::ranges::any_of(volcanoes,
                                   [&](const auto& volcano) {
                                       return glm::length(at - volcano.center) * radius <
                                              volcano.radius / 2.0;
                                   }) ||
               std::ranges::any_of(shields, [&](const auto& shield) {
                   return glm::length(at - shield.center) * radius < 4'000.0;
               });
    };
    for (const CellId cell : scattered(40'000)) {
        if (under(cell)) {
            REQUIRE(central(hog::planet::cell_center(cell)));
        }
    }
    // A grid of cells about 150 m apart over the first plume's youngest shield.
    REQUIRE_FALSE(shields.empty());
    const hog::DVec3 young = shields.front().center;
    const hog::planet::TangentFrame frame = hog::planet::tangent_frame(young);
    constexpr int steps = 6'000 / 150;
    int molten = 0;
    for (int east = -steps; east <= steps; ++east) {
        for (int north = -steps; north <= steps; ++north) {
            const hog::DVec3 offset = ((frame.east * static_cast<double>(east)) +
                                       (frame.north * static_cast<double>(north))) *
                                      (150.0 / radius);
            const hog::DVec3 at = glm::normalize(young + offset);
            if (under(hog::planet::cell_at(at, 12))) {
                ++molten;
                REQUIRE(central(at));
            }
        }
    }
    CHECK(molten > 0);
}

TEST_CASE("rock warms with depth through the crust and the mantle", "[planet][geology]") {
    const Geology geology{planet(1)};
    const auto clear_of_chambers = [&](CellId cell) {
        return std::ranges::none_of(geology.chambers(), [&](const auto& chamber) {
            return glm::length(hog::planet::cell_center(cell) - chamber.center) * radius < 20'000.0;
        });
    };
    int checked = 0;
    for (const CellId cell : cells_at(5)) {
        if (!clear_of_chambers(cell)) {
            continue;
        }
        ++checked;
        const std::int64_t elevation = geology.terrain().elevation(cell);
        const std::int64_t surface = geology.rock_temperature(cell, 0);
        if (elevation >= 0) {
            REQUIRE(surface == geology.climate().temperature(cell));
        } else if (elevation < -1'000 * fixed_one) {
            REQUIRE(surface == 4 * fixed_one);
        }
        const std::int64_t mantle = geology.crust_thickness(cell);
        REQUIRE(geology.rock_temperature(cell, mantle) == 800 * fixed_one);
        REQUIRE(geology.rock_temperature(cell, mantle + (10'000 * fixed_one)) == 900 * fixed_one);
        std::int64_t last = surface;
        for (std::int64_t depth = 0; depth <= 110'000 * fixed_one;
             depth += hog::testing::full_or<std::int64_t>(500, 2'500) * fixed_one) {
            const std::int64_t heat = geology.rock_temperature(cell, depth);
            REQUIRE(heat >= last);
            last = heat;
        }
        // Nothing deeper than the planet's middle.
        REQUIRE(geology.rock_temperature(cell, std::numeric_limits<std::int64_t>::max()) ==
                geology.rock_temperature(cell, static_cast<std::int64_t>(radius) * fixed_one));
    }
    CHECK(checked > 4'500);
}

TEST_CASE("the rock around a magma chamber is hot", "[planet][geology]") {
    const Geology geology{planet(1)};
    for (const auto& chamber : geology.chambers()) {
        const CellId cell = hog::planet::cell_at(chamber.center, 12);
        const Strata strata = geology.strata(cell);
        const std::int64_t roof = strata.chamber_top;
        CHECK(geology.rock_temperature(cell, roof + (strata.chamber_thickness / 2)) ==
              1'100 * fixed_one);
        CHECK(geology.rock_temperature(cell, roof) >= 1'099 * fixed_one);
        CHECK(geology.rock_temperature(cell, roof - (500 * fixed_one)) > 400 * fixed_one);
        // Cooling all the way up to warm ground.
        std::int64_t last = geology.rock_temperature(cell, roof);
        for (std::int64_t depth = roof; depth >= 0; depth -= 100 * fixed_one) {
            const std::int64_t heat = geology.rock_temperature(cell, depth);
            REQUIRE(heat <= last);
            last = heat;
        }
        // Warmer than the air above, where the sea does not cool it first.
        if (geology.terrain().elevation(cell) >= 0) {
            CHECK(geology.rock_temperature(cell, 0) > geology.climate().temperature(cell));
        }
        // Rock beside it at the same depth, past the rim, is warmed too but less.
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(chamber.center);
        const CellId beside = hog::planet::cell_at(
            glm::normalize(chamber.center + (frame.east * ((chamber.radius + 500.0) / radius))),
            12);
        const std::int64_t level = roof + (strata.chamber_thickness / 2) -
                                   geology.terrain().elevation(cell) +
                                   geology.terrain().elevation(beside);
        CHECK(geology.strata(beside).chamber_thickness == 0);
        CHECK(geology.rock_temperature(beside, level) > 300 * fixed_one);
        CHECK(geology.rock_temperature(beside, level) < 1'000 * fixed_one);
    }
}

TEST_CASE("deposits are sparse seams in their host rock", "[planet][geology]") {
    const Geology geology{planet(1)};
    std::array<int, hog::planet::resource_count> found{};
    int land = 0;
    int shelf = 0;
    int shelf_deposits = 0;
    for (const CellId cell : scattered(40'000)) {
        const auto deposits = geology.deposits(cell);
        if (!geology.continental(cell) && !geology.terrain().is_land(cell)) {
            for (const auto& deposit : deposits) {
                REQUIRE(deposit.empty());
            }
            continue;
        }
        const bool offshore = !geology.terrain().is_land(cell);
        if (offshore) {
            ++shelf;
            shelf_deposits +=
                std::ranges::any_of(deposits, [](const auto& deposit) { return !deposit.empty(); })
                    ? 1
                    : 0;
        } else {
            ++land;
        }
        const Strata strata = geology.strata(cell);
        for (int resource = 0; resource < hog::planet::resource_count; ++resource) {
            const auto& deposit = deposits.at(static_cast<std::size_t>(resource));
            if (deposit.empty()) {
                continue;
            }
            INFO(to_string(static_cast<Resource>(resource)));
            if (!offshore) {
                ++found.at(static_cast<std::size_t>(resource));
            }
            REQUIRE(deposit.thickness > 0);
            REQUIRE(deposit.thickness <= 15 * fixed_one);
            REQUIRE(deposit.top >= strata.cover_thickness);
            const double volume = geology.volume(cell, deposit);
            REQUIRE(volume > 0.0);
            REQUIRE(std::isfinite(volume));
            check_host(static_cast<Resource>(resource), deposit, strata);
        }
    }
    for (int resource = 0; resource < hog::planet::resource_count; ++resource) {
        const int count = found.at(static_cast<std::size_t>(resource));
        INFO(to_string(static_cast<Resource>(resource)) << " in " << count << " of " << land);
        CHECK(count > 0);
        CHECK(count < land / 3);
    }
    // Seams run on under the shelf.
    CHECK(shelf > 0);
    CHECK(shelf_deposits > 0);
    // Gold is the rarest.
    for (const int count : found) {
        CHECK(found.at(index(Resource::Gold)) <= count);
    }
}

TEST_CASE("the land nearest the start holds clay, iron and copper", "[planet][geology]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(4), values({1U, 2U, 3U, 4U})));
    const Geology geology{planet(seed)};
    std::array<int, hog::planet::resource_count> found{};
    // A grid of cells about 300 m apart within 8 km of it.
    const hog::DVec3 land = geology.terrain().nearest_land(hog::planet::start_direction);
    const hog::planet::TangentFrame frame = hog::planet::tangent_frame(land);
    constexpr int steps = 8'000 / 300;
    for (int east = -steps; east <= steps; ++east) {
        for (int north = -steps; north <= steps; ++north) {
            const hog::DVec3 offset = ((frame.east * static_cast<double>(east)) +
                                       (frame.north * static_cast<double>(north))) *
                                      (300.0 / radius);
            const CellId cell = hog::planet::cell_at(glm::normalize(land + offset), 12);
            const auto deposits = geology.deposits(cell);
            for (std::size_t resource = 0; resource < deposits.size(); ++resource) {
                found.at(resource) += deposits.at(resource).empty() ? 0 : 1;
            }
        }
    }
    CHECK(found.at(index(Resource::Clay)) > 0);
    CHECK(found.at(index(Resource::Iron)) > 0);
    CHECK(found.at(index(Resource::Copper)) > 0);
}

TEST_CASE("geology depends on the seed", "[planet][geology]") {
    const auto summary = [](const Geology& geology) {
        std::uint64_t hash = 0;
        for (const CellId cell : scattered(2'000)) {
            const Strata strata = geology.strata(cell);
            hash = hog::hash_values(hash, strata.cover_thickness, strata.sediment_thickness,
                                    static_cast<int>(strata.sediment), strata.mantle_depth);
            for (const auto& deposit : geology.deposits(cell)) {
                hash = hog::hash_values(hash, deposit.top, deposit.thickness);
            }
        }
        for (const auto& chamber : geology.chambers()) {
            hash =
                hog::hash_values(hash, chamber.depth, std::bit_cast<std::uint64_t>(chamber.radius),
                                 std::bit_cast<std::uint64_t>(chamber.center.x));
        }
        return hash;
    };
    const std::uint64_t first = summary(Geology{planet(1)});
    CHECK(summary(Geology{planet(1)}) == first);
    CHECK(summary(Geology{planet(2)}) != first);
}
