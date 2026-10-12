#include <hog/core/hash.hpp>
#include <hog/planet/geology.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/tile.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

using hog::planet::CellId;
using hog::planet::fixed_one;
using hog::planet::Geology;
using hog::planet::Resource;
using hog::planet::Rock;
using hog::planet::Strata;

namespace {

constexpr double radius = 115'000.0;

hog::planet::PlanetConfig planet(std::uint64_t seed) {
    return {.seed = seed, .ocean_fraction = 0.7, .radius = radius};
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

TEST_CASE("continents stand on granite and oceans on basalt", "[planet][geology]") {
    const std::uint64_t seed = GENERATE(1U, 2U);
    const Geology geology{planet(seed)};
    std::set<Rock> sediments;
    for (const CellId cell : cells_at(6)) {
        const Strata strata = geology.strata(cell);
        REQUIRE(strata.cover_thickness >= 0);
        REQUIRE(strata.sediment_thickness >= 0);
        if (!geology.terrain().is_land(cell)) {
            REQUIRE(strata.bedrock == Rock::Basalt);
            REQUIRE(strata.cover == Rock::Sand);
        } else if (geology.terrain().continents().is_land(cell)) {
            REQUIRE(strata.bedrock == Rock::Granite);
            sediments.insert(strata.sediment);
        }
    }
    CHECK(sediments == std::set{Rock::Sandstone, Rock::Shale, Rock::Limestone});
}

TEST_CASE("mountains are stripped down to granite", "[planet][geology]") {
    const std::uint64_t seed = GENERATE(1U, 2U);
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
    CHECK(mountain / mountain_count < 0.2 * lowland / lowland_count);
    CHECK(exposed > 0);
}

TEST_CASE("the starting islands are bare basalt under their soil", "[planet][geology]") {
    const Geology geology{planet(1)};
    for (const auto& island : geology.terrain().archipelago().islands()) {
        const Strata strata = geology.strata(hog::planet::cell_at(island.center));
        CHECK(strata.bedrock == Rock::Basalt);
        CHECK(strata.sediment_thickness == 0);
    }
}

TEST_CASE("deposits are sparse seams in their host rock", "[planet][geology]") {
    const Geology geology{planet(1)};
    std::array<int, hog::planet::resource_count> found{};
    int land = 0;
    for (const CellId cell : scattered(40'000)) {
        const auto deposits = geology.deposits(cell);
        if (!geology.terrain().is_land(cell)) {
            for (const auto& deposit : deposits) {
                REQUIRE(deposit.empty());
            }
            continue;
        }
        ++land;
        const Strata strata = geology.strata(cell);
        for (int resource = 0; resource < hog::planet::resource_count; ++resource) {
            const auto& deposit = deposits.at(static_cast<std::size_t>(resource));
            if (deposit.empty()) {
                continue;
            }
            INFO(to_string(static_cast<Resource>(resource)));
            ++found.at(static_cast<std::size_t>(resource));
            REQUIRE(deposit.thickness > 0);
            REQUIRE(deposit.thickness <= 15 * fixed_one);
            REQUIRE(deposit.top >= strata.cover_thickness);
            const double volume = geology.volume(cell, deposit);
            REQUIRE(volume > 0.0);
            REQUIRE(std::isfinite(volume));
            switch (static_cast<Resource>(resource)) {
            case Resource::Clay:
                REQUIRE(deposit.top == strata.cover_thickness);
                break;
            case Resource::Coal:
            case Resource::Salt:
                REQUIRE(deposit.top + deposit.thickness <= strata.bedrock_depth());
                REQUIRE(strata.rock_at(deposit.top) == strata.sediment);
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
    }
    for (int resource = 0; resource < hog::planet::resource_count; ++resource) {
        const int count = found.at(static_cast<std::size_t>(resource));
        INFO(to_string(static_cast<Resource>(resource)) << " in " << count << " of " << land);
        CHECK(count > 0);
        CHECK(count < land / 3);
    }
    // Gold is the rarest.
    for (const int count : found) {
        CHECK(found.at(index(Resource::Gold)) <= count);
    }
}

TEST_CASE("the starting islands hold clay, iron and copper", "[planet][geology]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const Geology geology{planet(seed)};
    std::array<int, hog::planet::resource_count> found{};
    // A grid of cells about 150 m apart over each island.
    for (const auto& island : geology.terrain().archipelago().islands()) {
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(island.center);
        const auto steps = static_cast<int>(island.radius / 150.0);
        for (int east = -steps; east <= steps; ++east) {
            for (int north = -steps; north <= steps; ++north) {
                const hog::DVec3 offset = ((frame.east * static_cast<double>(east)) +
                                           (frame.north * static_cast<double>(north))) *
                                          (150.0 / radius);
                const CellId cell =
                    hog::planet::cell_at(glm::normalize(island.center + offset), 12);
                const auto deposits = geology.deposits(cell);
                for (std::size_t resource = 0; resource < deposits.size(); ++resource) {
                    found.at(resource) += deposits.at(resource).empty() ? 0 : 1;
                }
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
                                    static_cast<int>(strata.sediment));
            for (const auto& deposit : geology.deposits(cell)) {
                hash = hog::hash_values(hash, deposit.top, deposit.thickness);
            }
        }
        return hash;
    };
    const std::uint64_t first = summary(Geology{planet(1)});
    CHECK(summary(Geology{planet(1)}) == first);
    CHECK(summary(Geology{planet(2)}) != first);
}
