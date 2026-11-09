#include <hog/flow/region.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

using hog::DVec3;
using hog::globe::Material;
using hog::globe::Rock;

namespace {

constexpr double tick = 0.05;

// Some fallen rock on the planet, found by looking about cells spread over it.
struct Found {
    hog::globe::Generator generator{hog::globe::Planet{}};
    std::optional<Rock> rock;

    // On ground that stays within a few metres of level a few rocks' lengths either side, so a rock
    // dug under rolls only into its hollow, not off down a slope.
    [[nodiscard]] bool gentle(const Rock& candidate) const {
        const double lod = hog::globe::voxel_lod(generator);
        const auto ground_at = [&](DVec3 point) {
            return hog::globe::ground_below(generator, glm::normalize(point), lod).value_or(0.0);
        };
        const DVec3 side = glm::normalize(glm::cross(candidate.up, candidate.across));
        const double here = ground_at(candidate.middle);
        const double reach = 4.0 * candidate.axes.x;
        const std::array<DVec3, 4> aways{candidate.across, -candidate.across, side, -side};
        return std::ranges::all_of(aways, [&](DVec3 away) {
            return std::abs(ground_at(candidate.middle + (away * reach)) - here) <= 0.5 * reach;
        });
    }

    Found() {
        const int level = 13;
        const std::uint64_t cells = hog::globe::cells_at(level);
        for (std::uint64_t k = 0; k < 40'000 && !rock; ++k) {
            const auto cell = hog::globe::cell_at_index(level, (k * 2'654'435'761ULL) % cells);
            const std::vector<Rock> near =
                generator.rocks_near(hog::globe::center(cell) * generator.planet().radius,
                                     hog::globe::voxel_lod(generator));
            for (const Rock& candidate : near) {
                if (!rock && candidate.axes.z > 2.5 && gentle(candidate)) {
                    rock = candidate;
                }
            }
        }
    }
};

// A region holding every brick within `reach` of `point`.
void hold_about(hog::flow::Region& region, const hog::globe::Layers& layers, DVec3 point,
                double reach) {
    for (const auto& [brick, indices] : hog::globe::sites_near(layers, point, reach)) {
        region.load(brick);
    }
}

bool solid_at(hog::flow::Region& region, const hog::globe::Layers& layers, DVec3 point) {
    const double radius = glm::length(point);
    const auto cell = hog::globe::cell_under(point / radius, layers.level);
    return region
               .at({.face = cell.face,
                    .i = cell.i,
                    .j = cell.j,
                    .layer = static_cast<std::uint32_t>(layers.layer_at(radius))})
               .solid != Material::Air;
}

} // namespace

TEST_CASE("a rock the ground holds up stays where it lies", "[flow][rocks]") {
    const Found found;
    REQUIRE(found.rock.has_value());
    const Rock& rock = *found.rock;
    hog::globe::MadeWorld made{found.generator};
    hog::globe::World& world = made.world();
    made.cover(glm::normalize(rock.middle));
    hog::flow::Region region{world, false};
    hold_about(region, world.layers(), rock.middle, 14.0);

    for (int step = 0; step < 5; ++step) {
        region.step(tick);
    }
    CHECK(region.parcels().empty());
    CHECK(solid_at(region, world.layers(), rock.middle));
}

TEST_CASE("a rock with the ground dug from under it rolls into the hollow and rests there",
          "[flow][rocks]") {
    const Found found;
    REQUIRE(found.rock.has_value());
    const Rock& rock = *found.rock;
    hog::globe::MadeWorld made{found.generator};
    hog::globe::World& world = made.world();
    made.cover(glm::normalize(rock.middle));
    // A ball taking the ground under the whole footprint, its top a voxel short of the rock's
    // middle.
    const double hollow = (0.75 * std::max(rock.axes.x, rock.axes.y)) + 1.0;
    hog::globe::dig(world, rock.middle - (rock.up * (hollow + world.layers().thickness)), hollow);
    hog::flow::Region region{world, false};
    hold_about(region, world.layers(), rock.middle, 14.0);

    region.step(tick);
    REQUIRE(region.parcels().size() == 1);
    CHECK(region.parcels().front().radius > 0.5);
    // Taken out of the ground it lay in.
    CHECK_FALSE(solid_at(region, world.layers(), rock.middle));

    DVec3 last = region.parcels().front().at;
    double radius = region.parcels().front().radius;
    for (int step = 0; step < 700 && !region.parcels().empty(); ++step) {
        last = region.parcels().front().at;
        region.step(tick);
    }
    REQUIRE(region.parcels().empty());
    // It came down into the hollow, and was built into the ground where it stopped.
    CHECK(glm::length(last) < glm::length(rock.middle) - 0.5);
    CHECK(solid_at(region, world.layers(), last - (glm::normalize(last) * 0.4 * radius)));
}
