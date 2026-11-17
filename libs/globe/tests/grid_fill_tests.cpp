#include "../src/formed.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/testing/quick.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::globe::fill;
using hog::globe::Filled;
using hog::globe::grid_of;
using hog::grid::Key;

namespace {

constexpr std::uint8_t levels = 5;

// A direction whose ground stands over the sea, or under it by `under` metres or more.
DVec3 spread_until(bool land, double under = 0.0) {
    const auto& world = hog::testing::small_world();
    const double sea = world.sea_level().value_or(world.planet().body.radius);
    for (std::uint64_t n = 0;; ++n) {
        const DVec3 up =
            hog::globe::center(hog::globe::cell_at_index(7, (n * 2'654'435'761U) % 98'304U));
        const double ground =
            hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value_or(0.0);
        if (land ? ground > sea + 5.0 : ground < sea - under) {
            return up;
        }
    }
}

// How far a point lies out of the solid as the filled shape has it, in metres: by the plane of
// the leaf over it, or a node's side either way where the leaf is all solid or all open.
double held(const hog::grid::Grid& grid, Key key, const Filled& filled, DVec3 point) {
    const Key foot = grid.key_at(point, static_cast<std::uint8_t>(key.depth + levels)).value();
    const auto down = [&](std::uint32_t at, std::uint32_t from) { return at - (from << levels); };
    const auto leaf =
        filled.shape.at(levels, down(foot.x, key.x), down(foot.y, key.y), down(foot.z, key.z));
    const Key over = foot.ancestor(static_cast<std::uint8_t>(key.depth + leaf.depth));
    const double side = grid.size(over.depth);
    if (!leaf.value.crosses()) {
        return leaf.value.is_open() ? side : -side;
    }
    return (leaf.value.distance() * side) +
           glm::dot(leaf.value.normal(), point - grid.centre(over));
}

// The water body the filled water tree has at a point.
hog::grid::Water::Value body_at(const hog::grid::Grid& grid, Key key, const Filled& filled,
                                DVec3 point) {
    const Key foot = grid.key_at(point, static_cast<std::uint8_t>(key.depth + levels)).value();
    const auto down = [&](std::uint32_t at, std::uint32_t from) { return at - (from << levels); };
    return filled.water.at(levels, down(foot.x, key.x), down(foot.y, key.y), down(foot.z, key.z))
        .value;
}

// Whether any of the finest nodes filled holds water of `body`.
bool holds(const Filled& filled, hog::grid::Water::Value body) {
    const std::uint32_t across = 1U << levels;
    for (std::uint32_t z = 0; z < across; ++z) {
        for (std::uint32_t y = 0; y < across; ++y) {
            for (std::uint32_t x = 0; x < across; ++x) {
                if (filled.water.at(levels, x, y, z).value == body) {
                    return true;
                }
            }
        }
    }
    return false;
}

// A river crossed a quarter, half and three quarters of the way from the point it runs from to
// the next: where water stands, how much of it is the river's own, and where it stands deepest.
struct Crossing {
    int wet = 0;
    int own = 0;
    std::optional<DVec3> deepest;
};

Crossing cross(const hog::globe::Formed& formed, std::uint32_t point, double lod, double radius) {
    const hog::formation::World& world = formed.world();
    const DVec3 from = world.sphere.point(point);
    const DVec3 to = world.sphere.point(world.rivers.receiver[point]);
    const int half = static_cast<int>(0.5 * glm::length(to - from) * radius);
    Crossing crossing;
    double deepest = 0.0;
    for (const double along : {0.25, 0.5, 0.75}) {
        const DVec3 middle = glm::normalize(from + ((to - from) * along));
        const DVec3 across = glm::normalize(glm::cross(to - from, middle));
        for (int step = -half; step < half; ++step) {
            const DVec3 here = glm::normalize(middle + (across * (step / radius)));
            const hog::globe::Formed::Here read = formed.at(here, lod);
            if (!read.river || read.lake) {
                continue;
            }
            ++crossing.wet;
            crossing.own += read.river->stem == formed.stem_of(point) ? 1 : 0;
            if (const double standing = read.river->level - read.top.value; standing > deepest) {
                deepest = standing;
                crossing.deepest = here;
            }
        }
    }
    return crossing;
}

// How much of what two palette entries show differs, from 0 alike to 1 nothing shared.
double unlike(std::uint16_t one, std::uint16_t other) {
    const hog::grid::Blend a = hog::grid::Blend::of(one);
    const hog::grid::Blend b = hog::grid::Blend::of(other);
    const auto share = [](const hog::grid::Blend& blend, std::uint16_t entry) {
        return (blend.first == entry ? blend.share : 0.0) +
               (blend.second == entry && blend.second != blend.first ? 1.0 - blend.share : 0.0);
    };
    const std::array named{a.first, a.second, b.first, b.second};
    double apart = 0.0;
    for (std::size_t k = 0; k < named.size(); ++k) {
        bool counted = false;
        for (std::size_t j = 0; j < k; ++j) {
            counted = counted || named.at(j) == named.at(k);
        }
        if (!counted) {
            apart += std::abs(share(a, named.at(k)) - share(b, named.at(k)));
        }
    }
    return apart / 2.0;
}

// How a node filled coarse compares with it filled finer, over many nodes. Materials count only
// on nodes up to 8 m: the generator covers coarser ground by its smoothed slope, gentler than the
// finer, so it lays clay where finer shows bare rock.
struct Agreement {
    int crossing = 0;
    double apart = 0.0; // the planes' distances, in sides
    double alike = 0.0; // their normals' dot
    int crossing_fine = 0;
    double shown = 0.0; // of those, how unlike what they show
    int solid = 0;
    int holds = 0; // of nodes not all open, holding the same material

    void add(const Filled& coarse, const Filled& fine, bool fine_enough) {
        const auto was = coarse.shape.root();
        const auto is = fine.shape.root();
        // Nothing finer comes where the coarse said none would, and solid never turns open.
        if (!was.value.crosses() && !was.open) {
            CHECK(fine.shape.nodes() == 1);
            CHECK(is.value == was.value);
        }
        CHECK_FALSE((was.value.is_solid() && is.value.is_open()));
        CHECK_FALSE((was.value.is_open() && is.value.is_solid()));
        if (was.value.crosses() && is.value.crosses()) {
            ++crossing;
            const double off = std::abs(was.value.distance() - is.value.distance());
            CHECK(off < 1.0);
            apart += off;
            alike += glm::dot(was.value.normal(), is.value.normal());
            if (fine_enough) {
                ++crossing_fine;
                shown += unlike(was.value.material(), is.value.material());
            }
        }
        // The sea's basin is never missed coarse, and solid holds its material at both.
        if (fine.water.root().value != 0) {
            CHECK(coarse.water.root().value == fine.water.root().value);
        }
        const auto air = hog::globe::palette_of(hog::globe::Material::Air);
        CHECK((coarse.material.root().value == air) == was.value.is_open());
        CHECK((fine.material.root().value == air) == is.value.is_open());
        if (fine_enough && !was.value.is_open() && !is.value.is_open()) {
            ++solid;
            holds += coarse.material.root().value == fine.material.root().value ? 1 : 0;
        }
    }
};

} // namespace

TEST_CASE("the grid holds the world down to the layers' thickness", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    CHECK(grid.finest() == world.layers().thickness);
    CHECK(grid.size(0) >= 2.0 * world.layers().ceiling());
}

TEST_CASE("open sky and deep rock are each one leaf", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const auto depth = static_cast<std::uint8_t>(grid.depth() - levels);
    const DVec3 up = spread_until(true);

    const Key sky = grid.key_at(up * (world.layers().ceiling() - 50.0), depth).value();
    const Filled air = fill(world, grid, sky, levels);
    CHECK(air.shape.nodes() == 1);
    CHECK(air.shape.root().value.is_open());
    CHECK_FALSE(air.shape.root().open);
    CHECK(air.material.root().value == hog::globe::palette_of(hog::globe::Material::Air));

    const Key deep = grid.key_at(up * (world.planet().body.radius * 0.5), depth).value();
    const Filled rock = fill(world, grid, deep, levels);
    CHECK(rock.shape.nodes() == 1);
    CHECK(rock.shape.root().value.is_solid());
    CHECK(rock.material.root().value != hog::globe::palette_of(hog::globe::Material::Air));
    CHECK(rock.water.root().value == 0);
}

TEST_CASE("ground splits only near its surface and follows the generator", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const double ground = hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    const Key key =
        grid.key_at(up * ground, static_cast<std::uint8_t>(grid.depth() - levels)).value();
    const Filled filled = fill(world, grid, key, levels);

    const std::size_t foot = std::size_t{1} << (3U * levels);
    CHECK(filled.shape.nodes() > 1);
    CHECK(filled.shape.leaves() < foot / 4);

    const double side = grid.size(key.depth);
    const double fine = grid.size(static_cast<std::uint8_t>(key.depth + levels));
    const DVec3 low = grid.centre(key) - (side / 2.0);
    int checked = 0;
    for (std::uint64_t n = 0; n < 400; ++n) {
        const DVec3 point = low + (DVec3{static_cast<double>(hog::hash_values(1, n) % 1000),
                                         static_cast<double>(hog::hash_values(2, n) % 1000),
                                         static_cast<double>(hog::hash_values(3, n) % 1000)} *
                                   (side / 1000.0));
        const double truth = world.sample(point, fine).distance;
        if (std::abs(truth) < 2.0 * fine) {
            continue;
        }
        ++checked;
        CHECK((held(grid, key, filled, point) > 0.0) == (truth > 0.0));
    }
    CHECK(checked > 100);
}

TEST_CASE("solid ground has its material and open space none", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const double ground = hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    const Key key =
        grid.key_at(up * ground, static_cast<std::uint8_t>(grid.depth() - levels)).value();
    const Filled filled = fill(world, grid, key, levels);
    const auto air = hog::globe::palette_of(hog::globe::Material::Air);
    const auto foot = static_cast<std::uint8_t>(key.depth + levels);
    const double fine = grid.size(foot);
    const std::uint32_t across = 1U << levels;
    for (std::uint32_t z = 0; z < across; z += 3) {
        for (std::uint32_t y = 0; y < across; y += 3) {
            for (std::uint32_t x = 0; x < across; x += 3) {
                const Key at{.depth = foot,
                             .x = (key.x << levels) + x,
                             .y = (key.y << levels) + y,
                             .z = (key.z << levels) + z};
                const double out = held(grid, key, filled, grid.centre(at));
                const auto material = filled.material.at(levels, x, y, z).value;
                if (out > fine) {
                    CHECK(material == air);
                } else if (out < -fine) {
                    CHECK(material != air);
                }
            }
        }
    }
}

TEST_CASE("open space under the sea is the sea's basin", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const double sea = world.sea_level().value();
    const DVec3 up = spread_until(false, 40.0);
    const auto depth = static_cast<std::uint8_t>(grid.depth() - levels);

    const Key under = grid.key_at(up * (sea - 10.0), depth).value();
    const Filled water = fill(world, grid, under, levels);
    CHECK(water.shape.root().value.is_open());
    CHECK(water.water.nodes() == 1);
    CHECK(water.water.root().value == hog::globe::sea_body);

    const Key sky = grid.key_at(up * (sea + 200.0), depth).value();
    CHECK(fill(world, grid, sky, levels).water.root().value == 0);
}

TEST_CASE("a node filled coarse agrees with the fine ones under it", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const double ground = hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    Agreement agreed;
    const int step = hog::testing::full_or(1, 4);
    for (int at = 4; std::cmp_less(at, grid.depth()); at += step) {
        CAPTURE(at);
        const auto depth = static_cast<std::uint8_t>(at);
        const auto under = static_cast<std::uint8_t>(std::min(3, grid.depth() - depth));
        const Key middle = grid.key_at(up * ground, depth).value();
        // The node on the ground and those beside its faces.
        const std::array<std::array<int, 3>, 7> around{
            {{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
        for (const auto& [dx, dy, dz] : around) {
            if (const auto key = middle.beside(dx, dy, dz)) {
                agreed.add(fill(world, grid, *key, 0), fill(world, grid, *key, under),
                           grid.size(depth) <= 8.0);
            }
        }
    }
    // Over every depth, as caves showing only finer turn a few nodes' planes.
    REQUIRE(agreed.crossing > 0);
    CHECK(agreed.apart / agreed.crossing < 0.25);
    CHECK(agreed.alike / agreed.crossing > 0.8);
    // As shares only with enough nodes to tell, which quick runs haven't.
    if (agreed.crossing_fine >= 10) {
        CHECK(agreed.shown / agreed.crossing_fine < 0.25);
    }
    if (agreed.solid >= 10) {
        CHECK(4 * agreed.holds >= 3 * agreed.solid);
    }
}

TEST_CASE("every lake is its own body at its level", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const double radius = world.planet().body.radius;
    const double lod = hog::globe::voxel_lod(world);
    const auto depth = static_cast<std::uint8_t>(grid.depth() - levels);
    const hog::globe::Formed& formed = *world.shape().formed;
    // Filled about a point a metre under the water standing over `up`, the body it's in.
    const auto filled_in = [&](DVec3 up, const hog::globe::Generator::Column& column) {
        const DVec3 point = up * (column.water() - 1.0);
        const Key key = grid.key_at(point, depth).value();
        return body_at(grid, key, fill(world, grid, key, levels), point);
    };

    const std::vector<hog::globe::Formed::LakeAt> lakes = formed.lakes();
    REQUIRE_FALSE(lakes.empty());
    int found = 0;
    for (std::uint32_t lake = 0; lake < lakes.size(); ++lake) {
        CAPTURE(lake);
        const hog::globe::Generator::Column column = world.column(lakes[lake].in, lod);
        const hog::globe::WaterBody body{.kind = hog::globe::WaterBody::Kind::Lake, .index = lake};
        if (column.water_body() != body || column.water() < column.ground() + 2.0) {
            continue;
        }
        ++found;
        CHECK(column.water() == Catch::Approx(radius + lakes[lake].level).margin(1e-6));
        CHECK(filled_in(lakes[lake].in, column) == hog::globe::body_value(body));
    }
    // Some lakes' middles lie where another's water or the shore wins.
    CHECK(4 * found >= 3 * static_cast<int>(lakes.size()));
}

TEST_CASE("the largest rivers are their own bodies where their water stands", "[globe][grid]") {
    // The small world's rivers sized up, as most of its own are too small to stand in.
    static const hog::globe::Generator world{[] {
        hog::globe::Planet planet = hog::testing::small_planet();
        planet.river_size = 100.0;
        return planet;
    }()};
    const hog::grid::Grid grid = grid_of(world);
    const double radius = world.planet().body.radius;
    const double lod = hog::globe::voxel_lod(world);
    const auto depth = static_cast<std::uint8_t>(grid.depth() - levels);
    const hog::globe::Formed& formed = *world.shape().formed;

    // The rivers carrying the most water, crossed between the points they run between: where water
    // stands, it's the river's own, but where a bigger one's runs beside it.
    const hog::formation::World& formed_world = formed.world();
    std::vector<std::uint32_t> largest;
    for (std::uint32_t point = 0; point < formed_world.sphere.size(); ++point) {
        const std::uint32_t down = formed_world.rivers.receiver[point];
        if (down != point && formed_world.lakes.lake[point] == hog::formation::Lakes::none &&
            formed_world.lakes.lake[down] == hog::formation::Lakes::none) {
            largest.push_back(point);
        }
    }
    std::ranges::sort(largest, [&](std::uint32_t a, std::uint32_t b) {
        return formed_world.lakes.discharge[a] > formed_world.lakes.discharge[b];
    });
    largest.resize(std::min<std::size_t>(largest.size(), 20));
    int crossed = 0;
    int deep = 0;
    for (const std::uint32_t point : largest) {
        CAPTURE(point);
        const Crossing crossing = cross(formed, point, lod, radius);
        const std::optional<DVec3> deepest = crossing.deepest;
        if (!deepest) {
            continue;
        }
        ++crossed;
        CHECK(2 * crossing.own >= crossing.wet);
        const hog::globe::Generator::Column column = world.column(*deepest, lod);
        if (column.water() > column.ground()) {
            ++deep;
            const auto body = column.water_body();
            CHECK(body.kind == hog::globe::WaterBody::Kind::River);
            CHECK(formed.stem_of(body.index) == body.index);
            const DVec3 point_in = *deepest * ((column.water() + column.ground()) / 2.0);
            const Key key = grid.key_at(point_in, depth).value();
            // A channel narrower than a node may wind between the nodes' middles there, so
            // anywhere in the node filled about it.
            CHECK(holds(fill(world, grid, key, levels), hog::globe::body_value(body)));
        }
    }
    CHECK(crossed >= 3);
    CHECK(deep >= 2);
}
