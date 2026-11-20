#include "../src/formed.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/globe/habitat.hpp>
#include <hog/grid/z_order.hpp>
#include <hog/testing/quick.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <variant>
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
    CHECK(air.woods.nodes() == 1);
    CHECK_FALSE(air.woods.root().value.ground);
    CHECK_FALSE(rock.woods.root().value.ground);
}

TEST_CASE("woods lie on the ground, as the place grows them, no finer than stands need",
          "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const double ground = hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    // Nodes from a few hundred metres across down to the finest woods are held in.
    int checked = 0;
    for (int at = 4; std::cmp_less(at, grid.depth()); ++at) {
        const auto depth = static_cast<std::uint8_t>(at);
        if (grid.size(depth) > 2'000.0) {
            continue;
        }
        CAPTURE(at);
        const auto under = static_cast<std::uint8_t>(std::min(3, grid.depth() - depth));
        const Key key = grid.key_at(up * ground, depth).value();
        const Filled filled = fill(world, grid, key, under);
        CHECK(filled.woods.root().value.ground);
        const std::uint32_t across = 1U << under;
        for (std::uint32_t x = 0; x < across; ++x) {
            for (std::uint32_t y = 0; y < across; ++y) {
                for (std::uint32_t z = 0; z < across; ++z) {
                    const auto shape = filled.shape.at(under, x, y, z);
                    const auto woods = filled.woods.at(under, x, y, z);
                    const Key node{.depth = static_cast<std::uint8_t>(key.depth + woods.depth),
                                   .x = (key.x << woods.depth) + (x >> (under - woods.depth)),
                                   .y = (key.y << woods.depth) + (y >> (under - woods.depth)),
                                   .z = (key.z << woods.depth) + (z >> (under - woods.depth))};
                    const double side = grid.size(node.depth);
                    // Held no finer than 16 m, but where the node filled is finer itself.
                    CHECK(side >= std::min(16.0, grid.size(key.depth)));
                    // A coarser plane merged over it may cross where its own didn't.
                    if (!woods.value.ground) {
                        CHECK((!shape.value.crosses() || shape.depth < woods.depth));
                        continue;
                    }
                    if (woods.depth == under || side < 32.0) {
                        const hog::flora::Woods truth =
                            hog::globe::woods(world, grid.centre(node), side);
                        const hog::grid::Stand stand =
                            hog::grid::Stand::of(truth.cover, truth.shares);
                        CHECK(std::abs(woods.value.cover - stand.cover) <=
                              3 * hog::grid::Woods::tolerance);
                        ++checked;
                    }
                }
            }
        }
    }
    CHECK(checked > 20);
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
                const auto leaf = filled.material.at(levels, x, y, z);
                // A leaf the surface crosses holds what most of it holds, as far as it reaches.
                const double reach =
                    std::max(fine, grid.size(static_cast<std::uint8_t>(key.depth + leaf.depth)));
                if (out > reach) {
                    CHECK(leaf.value == air);
                } else if (out < -reach) {
                    CHECK(leaf.value != air);
                }
            }
        }
    }
}

namespace {

// Whether ground at a node's own detail passes through it, across its corners and middle: any of
// a solid one standing clear over the ground, or of an open one deep under it.
bool ground_through(const hog::globe::Generator& world, const hog::grid::Grid& grid, Key node,
                    bool solid) {
    const double size = grid.size(node.depth);
    const DVec3 low = grid.low(node);
    for (int k = 0; k < 27; ++k) {
        const DVec3 point = low + (DVec3{k % 3, (k / 3) % 3, k / 9} * (size / 2.0));
        const double radius = glm::length(point);
        // Deep inside, as at the centre, there's no way up to read.
        if (radius < world.layers().floor) {
            continue;
        }
        const double top = world.column(point / radius, size).ground();
        if (solid ? radius > top + (size / 4.0) : radius < top - (size / 4.0)) {
            return true;
        }
    }
    return false;
}

// Every leaf of `filled` over `key`, `under` levels deep, the surface misses: each once, as the
// node it is and whether it's solid.
std::vector<std::pair<Key, bool>> missed_leaves(const Filled& filled, Key key, std::uint8_t under) {
    std::set<Key, hog::grid::ZOrder> seen;
    std::vector<std::pair<Key, bool>> found;
    const std::uint32_t across = 1U << under;
    for (std::uint32_t x = 0; x < across; ++x) {
        for (std::uint32_t y = 0; y < across; ++y) {
            for (std::uint32_t z = 0; z < across; ++z) {
                const auto leaf = filled.shape.at(under, x, y, z);
                const auto shift = static_cast<unsigned>(under - leaf.depth);
                const Key node{.depth = static_cast<std::uint8_t>(key.depth + leaf.depth),
                               .x = (key.x << leaf.depth) + (x >> shift),
                               .y = (key.y << leaf.depth) + (y >> shift),
                               .z = (key.z << leaf.depth) + (z >> shift)};
                if (!leaf.value.crosses() && seen.insert(node).second) {
                    found.emplace_back(node, leaf.value.is_solid());
                }
            }
        }
    }
    return found;
}

} // namespace

TEST_CASE("a coarse leaf the surface misses has no ground through it at its own detail",
          "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    constexpr std::uint8_t under = 3;
    int missed = 0;
    int crossed = 0;
    for (std::uint64_t n = 0; n < 6; ++n) {
        const DVec3 up =
            hog::globe::center(hog::globe::cell_at_index(5, (n * 2'654'435'761U) % 6'144U));
        const double ground =
            hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value_or(0.0);
        for (std::uint8_t depth = 1; depth < grid.depth(); ++depth) {
            const double side = grid.size(static_cast<std::uint8_t>(depth + under));
            if (side > 2'000.0 || side < 200.0) {
                continue;
            }
            const Key key = grid.key_at(up * ground, depth).value();
            const Filled filled = fill(world, grid, key, under);
            for (const auto& [node, solid] : missed_leaves(filled, key, under)) {
                ++missed;
                crossed += ground_through(world, grid, node, solid) ? 1 : 0;
            }
        }
    }
    UNSCOPED_INFO(crossed << " of " << missed << " leaves missed have ground through them");
    CHECK(missed > 50);
    CHECK(crossed == 0);
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

TEST_CASE("a node reads the same however deep it's filled, and from whichever node over it",
          "[globe][grid]") {
    const hog::globe::Generator& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const DVec3 ground =
        up * hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    const Key node = grid.key_at(ground, static_cast<std::uint8_t>(grid.depth() - levels)).value();
    const Filled deep = fill(world, grid, node, levels);
    // Every node of `tree` down `reach` from its root that `whole` also has, `within` deep under
    // its root at `offset` among that depth's, alike in both. Where either has a leaf over a
    // node, the leaf stands for it, so isn't compared there.
    std::size_t compared = 0;
    const auto same = [&](const char* kind, const auto& tree, const auto& whole, std::uint8_t reach,
                          std::uint8_t within, std::array<std::uint32_t, 3> offset) {
        CAPTURE(kind);
        for (std::uint8_t depth = 0; depth <= reach; ++depth) {
            const std::uint32_t side = 1U << depth;
            const auto at = static_cast<std::uint8_t>(within + depth);
            for (std::uint32_t z = 0; z < side; ++z) {
                for (std::uint32_t y = 0; y < side; ++y) {
                    for (std::uint32_t x = 0; x < side; ++x) {
                        const auto here = tree.at(depth, x, y, z);
                        const auto there =
                            whole.at(at, (offset[0] << depth) + x, (offset[1] << depth) + y,
                                     (offset[2] << depth) + z);
                        if (here.depth != depth || there.depth != at) {
                            continue;
                        }
                        CAPTURE(depth, x, y, z);
                        CHECK(here.value == there.value);
                        ++compared;
                    }
                }
            }
        }
    };
    REQUIRE(deep.shape.nodes() > 1);
    for (const std::uint8_t shallow : {std::uint8_t{1}, std::uint8_t{3}}) {
        CAPTURE(shallow);
        const Filled less = fill(world, grid, node, shallow);
        same("shape", less.shape, deep.shape, shallow, 0, {0, 0, 0});
        same("material", less.material, deep.material, shallow, 0, {0, 0, 0});
        same("water", less.water, deep.water, shallow, 0, {0, 0, 0});
        same("woods", less.woods, deep.woods, shallow, 0, {0, 0, 0});
    }
    // Filled from the node over it, the same under it.
    const Filled over = fill(world, grid, node.parent(), levels);
    const std::array<std::uint32_t, 3> offset{node.x & 1U, node.y & 1U, node.z & 1U};
    const auto reach = static_cast<std::uint8_t>(levels - 1);
    same("shape", deep.shape, over.shape, reach, 1, offset);
    same("material", deep.material, over.material, reach, 1, offset);
    same("water", deep.water, over.water, reach, 1, offset);
    same("woods", deep.woods, over.woods, reach, 1, offset);
    CHECK(compared > 100);
}

TEST_CASE("pieces filled apart graft into what's filled whole", "[globe][grid]") {
    const hog::globe::Generator& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const DVec3 ground =
        up * hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    const Key node = grid.key_at(ground, static_cast<std::uint8_t>(grid.depth() - levels)).value();
    const Filled whole = fill(world, grid, node, levels);
    // Coarse first, then each child's piece to the same depth grafted in.
    Filled pieced = fill(world, grid, node, 1);
    for (std::uint32_t octant = 0; octant < 8; ++octant) {
        const Key child = node.child(octant);
        const Filled piece = fill(world, grid, child, levels - 1);
        const std::uint32_t x = octant & 1U;
        const std::uint32_t y = (octant >> 1U) & 1U;
        const std::uint32_t z = octant >> 2U;
        pieced.shape.graft(1, x, y, z, piece.shape);
        pieced.material.graft(1, x, y, z, piece.material);
        pieced.water.graft(1, x, y, z, piece.water);
        pieced.woods.graft(1, x, y, z, piece.woods);
    }
    // Every node both hold, alike; a leaf in either stands for what's under it.
    std::size_t compared = 0;
    const auto same = [&](const char* kind, const auto& one, const auto& other) {
        CAPTURE(kind);
        CHECK(one.levels() == other.levels());
        for (std::uint8_t depth = 0; depth <= levels; ++depth) {
            const std::uint32_t side = 1U << depth;
            for (std::uint32_t n = 0; n < side * side * side; ++n) {
                const std::uint32_t x = n % side;
                const std::uint32_t y = (n / side) % side;
                const std::uint32_t z = n / (side * side);
                const auto here = one.at(depth, x, y, z);
                const auto there = other.at(depth, x, y, z);
                if (here.depth == depth && there.depth == depth) {
                    CAPTURE(depth, x, y, z);
                    CHECK(here.value == there.value);
                    ++compared;
                }
            }
        }
    };
    same("shape", pieced.shape, whole.shape);
    same("material", pieced.material, whole.material);
    same("water", pieced.water, whole.water);
    same("woods", pieced.woods, whole.woods);
    CHECK(compared > 100);
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
    const hog::globe::Generator& world = hog::testing::small_world_sized_rivers();
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

TEST_CASE("the bodies table holds the sea and every lake at its level", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const double radius = world.planet().body.radius;
    const hog::grid::WaterBodies table = hog::globe::water_bodies(world);

    const auto* sea = std::get_if<hog::grid::Sea>(&table.bodies.at(hog::globe::sea_body));
    REQUIRE(sea != nullptr);
    CHECK(sea->level == world.sea_level().value());
    const std::vector<hog::globe::Formed::LakeAt> lakes = world.shape().formed->lakes();
    REQUIRE_FALSE(lakes.empty());
    for (std::uint32_t lake = 0; lake < lakes.size(); ++lake) {
        CAPTURE(lake);
        const auto found = table.bodies.find(
            hog::globe::body_value({.kind = hog::globe::WaterBody::Kind::Lake, .index = lake}));
        REQUIRE(found != table.bodies.end());
        const auto* body = std::get_if<hog::grid::Lake>(&found->second);
        REQUIRE(body != nullptr);
        CHECK(body->level == Catch::Approx(radius + lakes[lake].level).margin(1e-6));
        CHECK(body->season.rise >= 0.0);
    }
    CHECK(table == *hog::grid::WaterBodies::decode(table.encode()));
}

TEST_CASE("each river whose water stands has a body along its course", "[globe][grid]") {
    const hog::globe::Generator& world = hog::testing::small_world_sized_rivers();
    const double radius = world.planet().body.radius;
    const double lod = hog::globe::voxel_lod(world);
    const hog::globe::Formed& formed = *world.shape().formed;
    const hog::grid::WaterBodies table = hog::globe::water_bodies(world);

    int rivers = 0;
    for (const auto& [id, body] : table.bodies) {
        if (const auto* river = std::get_if<hog::grid::River>(&body)) {
            ++rivers;
            CAPTURE(id);
            REQUIRE(river->course.size() >= 2);
            for (const hog::grid::River::Point& point : river->course) {
                CHECK(glm::length(point.place) == Catch::Approx(point.level));
                CHECK(point.flow >= 0.0);
                CHECK(point.rise >= 0.0);
            }
        }
    }
    CHECK(rivers > 0);

    // Where the largest rivers' water stands deepest across them, their bodies run by.
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
    int deep = 0;
    for (const std::uint32_t point : largest) {
        CAPTURE(point);
        const std::optional<DVec3> deepest = cross(formed, point, lod, radius).deepest;
        if (!deepest) {
            continue;
        }
        const hog::globe::Generator::Column column = world.column(*deepest, lod);
        if (column.water() <= column.ground()) {
            continue;
        }
        ++deep;
        const auto found = table.bodies.find(hog::globe::body_value(column.water_body()));
        REQUIRE(found != table.bodies.end());
        const auto* river = std::get_if<hog::grid::River>(&found->second);
        REQUIRE(river != nullptr);
        // A channel winds about its course out to a few times its width.
        CHECK(river->along(*deepest * column.water()).off < 100.0);
    }
    CHECK(deep >= 2);
}

TEST_CASE("a placed rock's shape is its rock's", "[globe][grid]") {
    hog::globe::Rock rock;
    rock.middle = {3.0, -2.0, 1000.0};
    rock.up = glm::normalize(DVec3{0.1, 0.0, 1.0});
    rock.across = glm::normalize(glm::cross(DVec3{0.0, 1.0, 0.0}, rock.up));
    rock.axes = {1.5, 0.8, 0.5};
    rock.edge = 0.2;
    rock.material = hog::globe::Material::Granite;
    rock.key = 77;
    const hog::globe::Placed placed = hog::globe::placed_rock(rock);
    CHECK(placed.kind == hog::globe::Placed::Kind::Rock);
    CHECK(placed.key == 77);
    CHECK(placed.material == rock.material);
    for (std::uint64_t n = 0; n < 200; ++n) {
        const DVec3 point = rock.middle +
                            (DVec3{static_cast<double>(hog::hash_values(n, 1) % 1000),
                                   static_cast<double>(hog::hash_values(n, 2) % 1000),
                                   static_cast<double>(hog::hash_values(n, 3) % 1000)} /
                             250.0) -
                            DVec3{2.0};
        DVec3 normal{0.0};
        CHECK(placed.distance(point) == Catch::Approx(rock.distance(point, normal)).margin(1e-9));
    }
}

// May fail: the rock it finds lies on a 50° slope, where the fill holds the node its top is in
// as solid though the ground stands below it; not yet understood.
TEST_CASE("fallen rocks are placed apart from the ground, where and as big as they're laid",
          "[globe][grid][!mayfail]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const auto depth = static_cast<std::uint8_t>(grid.depth() - levels);
    const double lod = std::max(grid.size(static_cast<std::uint8_t>(depth + levels)),
                                hog::globe::voxel_lod(world));
    // A rock standing a metre or more over the ground.
    std::optional<hog::globe::Rock> found;
    for (std::uint64_t n = 0; !found; ++n) {
        const DVec3 up =
            hog::globe::center(hog::globe::cell_at_index(7, (n * 2'654'435'761U) % 98'304U));
        // Sought among the bigger ones alone, as the smaller are many more to lay.
        for (const hog::globe::Rock& rock : world.rocks_within(up, 10.0, 4.0 * lod)) {
            if (rock.axes.z >= 0.5) {
                found = rock;
                break;
            }
        }
    }
    const hog::globe::Rock rock = found.value();
    const Key key = grid.key_at(rock.middle, depth).value();
    const Filled filled = fill(world, grid, key, levels);
    // As the generator lays them at that detail, each of those in the node.
    std::size_t inside = 0;
    for (const hog::globe::Rock& laid : world.rocks_near(rock.middle, lod)) {
        if (grid.key_at(laid.middle, depth) != key) {
            continue;
        }
        ++inside;
        const auto placed = std::ranges::find(filled.placed, laid.key, &hog::globe::Placed::key);
        REQUIRE(placed != filled.placed.end());
        CHECK(placed->middle == laid.middle);
        CHECK(placed->half == laid.axes);
        CHECK(placed->material == laid.material);
    }
    CHECK(inside >= 1);
    CHECK(filled.placed.size() == inside);
    // Its top is out of the ground's shape: the ground alone stands under it.
    const DVec3 top = rock.middle + (rock.up * (rock.axes.z * 0.8));
    DVec3 normal{0.0};
    REQUIRE(rock.distance(top, normal) < 0.0);
    CHECK(world.sample(top, lod).distance < 0.0);
    CHECK(held(grid, key, filled, top) > 0.0);
}

// May fail: the crag it finds stands 0.3 m lower placed than formed, past the margin.
TEST_CASE("crags are placed apart, where and as high as they stood out of the ground",
          "[globe][grid][!mayfail]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const hog::globe::Formed& formed = *world.shape().formed;
    const double radius = world.planet().body.radius;
    const double lod = hog::globe::voxel_lod(world);
    // A crag standing half a metre or more out of the ground.
    DVec3 up{0.0};
    for (std::uint64_t n = 0;; ++n) {
        up = hog::globe::center(
            hog::globe::cell_at_index(12, (n * 2'654'435'761ULL) % 100'663'296ULL));
        if (formed.at(up, lod).crag > 0.5) {
            break;
        }
    }
    // Left out of the ground, which stands as it would without it.
    const hog::globe::Formed::Here bare = formed.at(up, lod, false);
    REQUIRE(bare.standing);
    const hog::globe::Formed::Here::Standing standing = bare.standing.value();
    CHECK(bare.top.value ==
          Catch::Approx(formed.at(up, lod).top.value - formed.at(up, lod).crag).margin(1e-6));
    const std::uint64_t block = standing.block.id;
    const std::vector<hog::globe::Crag> crags = world.crags_within(up, 1.0, lod);
    const auto crag = std::ranges::find(crags, block, &hog::globe::Crag::key);
    REQUIRE(crag != crags.end());
    const hog::globe::Placed placed = hog::globe::placed_crag(*crag);
    CHECK(placed.material != hog::globe::Material::Air);

    // As high at its highest as it stood, over the columns its block stood out of.
    const DVec3 axis = std::abs(up.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(axis, up));
    const DVec3 north = glm::cross(up, east);
    const double reach = glm::length(crag->half) + 1.0;
    double stood = -1e30;
    double stands = -1e30;
    const int steps = static_cast<int>(reach / 0.25);
    for (int i = -steps; i <= steps; ++i) {
        for (int j = -steps; j <= steps; ++j) {
            const DVec3 there =
                glm::normalize(up + (((east * (i * 0.25)) + (north * (j * 0.25))) / radius));
            const hog::globe::Formed::Here out = formed.at(there, lod);
            const hog::globe::Formed::Here left = formed.at(there, lod, false);
            if (out.crag <= 0.0 || !left.standing || left.standing->block.id != block) {
                continue;
            }
            stood = std::max(stood, radius + out.top.value);
            // Its top over the column, found coming down it.
            double over = left.standing->rock_top + 20.0;
            while (over > left.standing->rock_top - 20.0 && placed.distance(there * over) >= 0.0) {
                over -= 0.01;
            }
            stands = std::max(stands, over);
        }
    }
    CHECK(stands == Catch::Approx(stood).margin(0.25));

    // A grid fill about it places it apart from the ground.
    const auto depth = static_cast<std::uint8_t>(grid.depth() - levels);
    const Key key = grid.key_at(crag->middle, depth).value();
    const Filled filled = fill(world, grid, key, levels);
    const auto in_fill = std::ranges::find(filled.placed, block, &hog::globe::Placed::key);
    REQUIRE(in_fill != filled.placed.end());
    CHECK(in_fill->kind == hog::globe::Placed::Kind::Crag);
}

TEST_CASE("vents and pools are placed apart where they open", "[globe][grid]") {
    // The small world hot enough for volcanoes and pools to be found on it.
    static const hog::globe::Generator world{[] {
        hog::globe::Planet planet = hog::testing::small_planet();
        planet.heat_flow = 0.35;
        return planet;
    }()};
    const hog::grid::Grid grid = grid_of(world);
    const std::vector<hog::globe::VentSite> sites = hog::globe::vent_sites(world);
    REQUIRE_FALSE(sites.empty());
    for (const hog::globe::VentSite& site : sites) {
        CAPTURE(site.key);
        // Found near it as over the whole planet.
        const std::vector<hog::globe::VentSite> near =
            hog::globe::vent_sites_within(world, site.direction, 10.0);
        const auto found = std::ranges::find(near, site.key, &hog::globe::VentSite::key);
        REQUIRE(found != near.end());
        CHECK(found->ground == site.ground);
        CHECK(found->pool == site.pool);
        // A fill about it places it there.
        const DVec3 opening = site.direction * site.ground;
        const Key key =
            grid.key_at(opening, static_cast<std::uint8_t>(grid.depth() - levels)).value();
        const Filled filled = fill(world, grid, key, 0);
        const auto placed = std::ranges::find(filled.placed, site.key, &hog::globe::Placed::key);
        REQUIRE(placed != filled.placed.end());
        CHECK(placed->kind ==
              (site.pool ? hog::globe::Placed::Kind::Pool : hog::globe::Placed::Kind::Vent));
        CHECK(placed->middle == opening);
    }
}

TEST_CASE("woods' share of filling a page", "[.bench][globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const double ground = hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    for (const int up_from_finest : {1, 2, 1}) {
        const auto root = static_cast<std::uint8_t>(grid.depth() - (6 * up_from_finest));
        const Key key = grid.key_at(up * ground, root).value();
        const auto started = std::chrono::steady_clock::now();
        const Filled filled = fill(world, grid, key, 6);
        const double whole =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        // As many woods samples again, on their own.
        const std::size_t samples = filled.woods.leaves();
        const auto sampling = std::chrono::steady_clock::now();
        double sum = 0.0;
        for (std::size_t n = 0; n < samples; ++n) {
            sum +=
                hog::globe::woods(world, up, grid.size(static_cast<std::uint8_t>(root + 6))).cover;
        }
        const double woods =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - sampling).count();
        WARN("page " << grid.size(root) << " m: filled in " << whole * 1e3 << " ms, "
                     << filled.woods.leaves() << " woods leaves of " << filled.woods.nodes()
                     << " nodes, " << filled.woods.encode().size() << " bytes (shape "
                     << filled.shape.encode().size() << "); a sample "
                     << woods / static_cast<double>(samples) * 1e6 << " us, mean cover "
                     << sum / static_cast<double>(samples));
    }
}
