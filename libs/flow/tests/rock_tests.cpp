#include <hog/flow/region.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

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

    Found() {
        const int level = 13;
        const std::uint64_t cells = hog::globe::cells_at(level);
        for (std::uint64_t k = 0; k < 4'000 && !rock; ++k) {
            const auto cell = hog::globe::cell_at_index(level, (k * 2'654'435'761ULL) % cells);
            const std::vector<Rock> near = generator.rocks_near(
                hog::globe::center(cell) * generator.planet().radius,
                hog::globe::voxel_lod(generator));
            if (!near.empty()) {
                rock = near.front();
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
    hog::globe::VoxelStore store{found.generator.layers()};
    hog::globe::World world{found.generator, store};
    hog::flow::Region region{world, false};
    hold_about(region, store.layers(), rock.middle, 14.0);

    for (int step = 0; step < 5; ++step) {
        region.step(tick);
    }
    CHECK(region.parcels().empty());
    CHECK(solid_at(region, store.layers(), rock.middle));
}

TEST_CASE("a rock with the ground dug from under it rolls into the hollow and rests there",
          "[flow][rocks]") {
    const Found found;
    REQUIRE(found.rock.has_value());
    const Rock& rock = *found.rock;
    hog::globe::VoxelStore store{found.generator.layers()};
    hog::globe::World world{found.generator, store};
    // A ball taking the ground under the footprint, stopping short of the rock's middle.
    const double hollow = (0.5 * rock.axes.x) + 1.0;
    hog::globe::dig(world, rock.middle - (rock.up * (rock.axes.z + 0.9)), hollow);
    hog::flow::Region region{world, false};
    hold_about(region, store.layers(), rock.middle, 14.0);

    region.step(tick);
    REQUIRE(region.parcels().size() == 1);
    CHECK(region.parcels().front().radius > 0.5);
    // Taken out of the ground it lay in.
    CHECK_FALSE(solid_at(region, store.layers(), rock.middle));

    DVec3 last = region.parcels().front().at;
    double radius = region.parcels().front().radius;
    for (int step = 0; step < 700 && !region.parcels().empty(); ++step) {
        last = region.parcels().front().at;
        region.step(tick);
    }
    REQUIRE(region.parcels().empty());
    // It came down into the hollow, and was built into the ground where it stopped.
    CHECK(glm::length(last) < glm::length(rock.middle) - 0.5);
    CHECK(solid_at(region, store.layers(), last - (glm::normalize(last) * 0.4 * radius)));
}
