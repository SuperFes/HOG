#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/tree.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <tuple>
#include <vector>

using hog::baked::Bands;
using hog::baked::ChunkFiller;
using hog::baked::ChunkStore;
using hog::baked::Region;
using hog::baked::Regions;
using hog::grid::Key;
using hog::grid::Tree;

namespace {

// Pumps until nothing is asked for or being filled, giving the regions written.
std::vector<Region> settle(ChunkFiller& filler) {
    std::vector<Region> written;
    while (!filler.idle()) {
        const auto pumped = filler.pump(true);
        REQUIRE(pumped.has_value());
        written.insert(written.end(), pumped->begin(), pumped->end());
    }
    std::ranges::sort(written);
    return written;
}

ChunkStore scratch(const hog::globe::Generator& generator) {
    auto store =
        ChunkStore::scratch(hog::baked::baking_of(generator), {.map_bytes = std::uint64_t{1} << 30U,
                                                               .readers = 64,
                                                               .read_only = false,
                                                               .band_height = 3,
                                                               .budget = std::size_t{16} << 10U});
    REQUIRE(store.has_value());
    return std::move(*store);
}

// A place on the ground along a great circle from a fixed start, `along` metres on.
hog::DVec3 on_ground(const hog::globe::Generator& generator, double along) {
    const hog::DVec3 start = glm::normalize(hog::DVec3{-0.2, 0.9, 0.38});
    const hog::DVec3 east = glm::normalize(glm::cross(hog::DVec3{0.0, 0.0, 1.0}, start));
    const double radius = generator.column(start, hog::globe::voxel_lod(generator)).ground();
    const hog::DVec3 direction = glm::normalize(start + (east * (along / radius)));
    return direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
}

} // namespace

TEST_CASE("regions sit as wide as a fill's levels allow, under what the band above leaves open",
          "[baked][chunk-filling]") {
    const Regions regions{Bands{3, 11}, 6};
    CHECK(regions.depth(0) == 0);
    CHECK(regions.depth(1) == 0);
    CHECK(regions.depth(2) == 3);
    CHECK(regions.depth(3) == 5);
    const Key node{.depth = 7, .x = 100, .y = 21, .z = 64};
    CHECK(regions.over(2, node) == Region{.band = 2, .root = node.ancestor(3)});
    CHECK(regions.over(3, node.ancestor(4)) == Region{.band = 3, .root = node.ancestor(4)});

    // Under the root, three levels: the first octant an open leaf, the second closed, the rest
    // split to the foot, open only along their first child's first child.
    using Material = Tree<hog::grid::Material>;
    const Material coarser = Material::grow(
        3,
        [](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
           std::uint32_t z) -> Material::Decided {
            if (depth == 0) {
                return {.value = 1, .open = false, .split = true};
            }
            const std::uint32_t top =
                (x >> (depth - 1U)) + (2 * (y >> (depth - 1U))) + (4 * (z >> (depth - 1U)));
            if (top == 0) {
                return {.value = 2, .open = true, .split = false};
            }
            if (top == 1) {
                return {.value = 3, .open = false, .split = false};
            }
            const std::uint32_t mask = (1U << (depth - 1U)) - 1U;
            const bool corner = (x & mask) == 0 && (y & mask) == 0 && (z & mask) == 0;
            return {.value = static_cast<std::uint16_t>(4 + depth),
                    .open = depth == 3 && corner,
                    .split = depth < 3};
        });
    const std::vector<Region> found = regions.under(2, Key{}, coarser);
    std::set<Key> roots;
    for (const Region& region : found) {
        CHECK(region.band == 2);
        roots.insert(region.root);
    }
    CHECK(found.size() == roots.size());
    // The open leaf over the fill depth is a region itself; each split octant gives its corner.
    CHECK(roots.contains(Key{.depth = 1}));
    CHECK(roots.size() == 7);
    for (std::size_t octant = 2; octant < 8; ++octant) {
        const Key top = Key{}.child(octant);
        CHECK(roots.contains(top.child(0).child(0)));
    }

    // A node narrower than a region gives the region over it, where anything under it is open.
    const Key deep{.depth = 4, .x = 9, .y = 2, .z = 7};
    CHECK(regions.under(2, deep, Material{2, 5, true}) ==
          std::vector<Region>{{.band = 2, .root = deep.ancestor(3)}});
    CHECK(regions.under(2, deep, Material{2, 5, false}).empty());
}

TEST_CASE("an ask is filled down to its depth, each region as the generator fills it",
          "[baked][chunk-filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    ChunkStore store = scratch(generator);
    hog::jobs::JobSystem jobs{2};
    ChunkFiller filler{generator, store, jobs, {.fill_levels = 4, .layered = {}}};
    const Bands& bands = store.bands();
    const Regions regions{bands, 4};

    const hog::DVec3 place = on_ground(generator, 0.0);
    const Key node = grid.key_at(place, static_cast<std::uint8_t>(grid.depth() - 6)).value();
    const auto depth = static_cast<std::uint8_t>(node.depth + 3);
    filler.want(node, depth);
    const std::vector<Region> written = settle(filler);
    CHECK(filler.take_answered() ==
          std::vector<ChunkFiller::Asked>{{.node = node, .depth = depth}});
    CHECK(std::ranges::adjacent_find(written) == written.end());
    for (const Region& region : written) {
        CHECK(region.band >= bands.band_of(node.depth));
        CHECK(region.band <= bands.band_of(depth));
    }

    auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    const auto piece = snapshot->piece<hog::grid::Shape>(node, depth);
    REQUIRE(piece.has_value());
    CHECK(piece.value().tree.levels() == depth - node.depth);
    // The regions over the place in each band read as the generator fills them.
    for (std::uint8_t band = bands.band_of(node.depth); band <= bands.band_of(depth); ++band) {
        const Region region = regions.over(band, grid.key_at(place, bands.foot(band)).value());
        CAPTURE(band, region.root.depth);
        REQUIRE(std::ranges::binary_search(written, region));
        const hog::globe::Filled filled = hog::baked::fill_region(generator, grid, bands, region);
        const auto shape = snapshot->piece<hog::grid::Shape>(region.root, bands.foot(band));
        const auto material = snapshot->piece<hog::grid::Material>(region.root, bands.foot(band));
        const auto water = snapshot->piece<hog::grid::Water>(region.root, bands.foot(band));
        REQUIRE(shape.has_value());
        REQUIRE(material.has_value());
        REQUIRE(water.has_value());
        CHECK(shape.value().tree.same_as(filled.shape));
        CHECK(material.value().tree.same_as(filled.material));
        CHECK(water.value().tree.same_as(filled.water));
    }
}

TEST_CASE("a filler asked to layer fills each region layered", "[baked][chunk-filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    ChunkStore store = scratch(generator);
    hog::jobs::JobSystem jobs{2};
    const hog::globe::Layered layered{.below =
                                          grid.size(static_cast<std::uint8_t>(grid.depth() - 2))};
    ChunkFiller filler{generator, store, jobs, {.fill_levels = 4, .layered = layered}};
    const Bands& bands = store.bands();

    const hog::DVec3 place = on_ground(generator, 0.0);
    const Key node = grid.key_at(place, static_cast<std::uint8_t>(grid.depth() - 4)).value();
    filler.want(node, grid.depth());
    const std::vector<Region> written = settle(filler);
    REQUIRE_FALSE(written.empty());
    auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    bool unlike = false;
    for (const Region& region : written) {
        const auto shape = snapshot->piece<hog::grid::Shape>(region.root, bands.foot(region.band));
        REQUIRE(shape.has_value());
        CHECK(shape.value().tree.same_as(
            hog::baked::fill_region(generator, grid, bands, region, layered).shape));
        unlike = unlike || !shape.value().tree.same_as(
                               hog::baked::fill_region(generator, grid, bands, region).shape);
    }
    // Layered under its base, so not as filled plain.
    CHECK(unlike);
}

TEST_CASE("asking deeper fills only the bands under, never again those over",
          "[baked][chunk-filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    ChunkStore store = scratch(generator);
    hog::jobs::JobSystem jobs{2};
    ChunkFiller filler{generator, store, jobs, {.fill_levels = 3, .layered = {}}};
    const Bands& bands = store.bands();

    const Key node =
        grid.key_at(on_ground(generator, 0.0), static_cast<std::uint8_t>(grid.depth() - 4)).value();
    const auto shallow = static_cast<std::uint8_t>(bands.foot(bands.band_of(node.depth)));
    filler.want(node, shallow);
    REQUIRE_FALSE(settle(filler).empty());
    const auto versions = [&] {
        auto snapshot = store.read();
        REQUIRE(snapshot.has_value());
        return snapshot->chunks<hog::grid::Shape>(bands.band_of(node.depth), node);
    };
    const auto before = versions();
    REQUIRE_FALSE(before.empty());

    filler.want(node, grid.depth());
    const std::vector<Region> deeper = settle(filler);
    REQUIRE_FALSE(deeper.empty());
    for (const Region& region : deeper) {
        CHECK(region.band > bands.band_of(node.depth));
    }
    CHECK(versions() == before);

    filler.want(node, shallow);
    filler.want(node, grid.depth());
    CHECK(settle(filler).empty());
    CHECK(filler.take_answered() ==
          std::vector<ChunkFiller::Asked>{{.node = node, .depth = shallow},
                                          {.node = node, .depth = grid.depth()}});
}

TEST_CASE("a node in ground the generator closes is answered with nothing filled under it",
          "[baked][chunk-filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    ChunkStore store = scratch(generator);
    hog::jobs::JobSystem jobs{2};
    ChunkFiller filler{generator, store, jobs};

    const hog::DVec3 ground = on_ground(generator, 0.0);
    const hog::DVec3 sky = glm::normalize(ground) * (glm::length(ground) + 2000.0);
    const Key node = grid.key_at(sky, static_cast<std::uint8_t>(grid.depth() - 2)).value();
    filler.want(node, grid.depth());
    CHECK(settle(filler).size() == 1);
    auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    const auto piece = snapshot->piece<hog::grid::Shape>(node, grid.depth());
    REQUIRE(piece.has_value());
    CHECK(piece.value().tree.nodes() == 1);
    CHECK_FALSE(piece.value().tree.root().open);
}

TEST_CASE("fills start by rank, a later round first, a region taking its highest asked",
          "[baked][chunk-filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    ChunkStore store = scratch(generator);
    // One worker, so two fills at a time.
    hog::jobs::JobSystem jobs{1};
    ChunkFiller filler{generator, store, jobs, {.fill_levels = 3, .layered = {}}};
    const Bands& bands = store.bands();
    const Regions regions{bands, 3};
    const std::uint8_t last = bands.count() - 1;

    // Six regions of the finest band along the ground, those over them held already.
    std::vector<Key> roots;
    const double size = grid.size(regions.depth(last));
    for (int step = 0; roots.size() < 6; ++step) {
        const Key root =
            grid.key_at(on_ground(generator, step * 1.5 * size), regions.depth(last)).value();
        if (std::ranges::find(roots, root) == roots.end()) {
            roots.push_back(root);
        }
    }
    for (const Key root : roots) {
        filler.want(root, bands.roots(last));
    }
    settle(filler);

    for (std::size_t n = 0; n < roots.size(); ++n) {
        filler.want(roots[n], grid.depth(), {.round = 1, .priority = static_cast<double>(n)});
    }
    filler.want(roots[0], grid.depth(), {.round = 2, .priority = 0.0});
    filler.want(roots[2], grid.depth(), {.round = 3, .priority = 0.0});
    // The first two start; the rest wait in rank order.
    REQUIRE(filler.pump().has_value());
    const auto region = [&](std::size_t n) { return Region{.band = last, .root = roots[n]}; };
    CHECK(filler.queued() == std::vector<Region>{region(5), region(4), region(3), region(1)});
    CHECK(settle(filler).size() == 6);
}

TEST_CASE("asks forgotten leave their fills not yet started unstarted", "[baked][chunk-filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    ChunkStore store = scratch(generator);
    // One worker, so two fills at a time.
    hog::jobs::JobSystem jobs{1};
    ChunkFiller filler{generator, store, jobs, {.fill_levels = 3, .layered = {}}};
    const Bands& bands = store.bands();
    const Regions regions{bands, 3};
    const std::uint8_t last = bands.count() - 1;

    std::vector<Key> roots;
    const double size = grid.size(regions.depth(last));
    for (int step = 0; roots.size() < 5; ++step) {
        const Key root =
            grid.key_at(on_ground(generator, step * 1.5 * size), regions.depth(last)).value();
        if (std::ranges::find(roots, root) == roots.end()) {
            roots.push_back(root);
        }
    }
    for (const Key root : roots) {
        filler.want(root, bands.roots(last));
    }
    settle(filler);
    std::ignore = filler.take_answered();

    for (std::size_t n = 0; n < roots.size(); ++n) {
        filler.want(roots[n], grid.depth(), {.round = 1, .priority = static_cast<double>(n)});
    }
    // One forgotten before it's taken in at all.
    std::vector<ChunkFiller::Asked> gone{{.node = roots[0], .depth = grid.depth()}};
    filler.forget(gone);
    REQUIRE(filler.pump().has_value());
    const auto region = [&](std::size_t n) { return Region{.band = last, .root = roots[n]}; };
    // The two ranked highest start; of those waiting, forgetting one leaves the other.
    CHECK(filler.queued() == std::vector<Region>{region(2), region(1)});
    gone = {{.node = roots[2], .depth = grid.depth()}, {.node = roots[4], .depth = grid.depth()}};
    filler.forget(gone);
    CHECK(filler.queued() == std::vector<Region>{region(1)});
    // What was under way is still written, with what's left.
    std::vector<Region> expected{region(1), region(3), region(4)};
    std::ranges::sort(expected);
    CHECK(settle(filler) == expected);
    // Only what's still asked for is answered.
    std::vector<ChunkFiller::Asked> answered = filler.take_answered();
    std::ranges::sort(answered);
    std::vector<ChunkFiller::Asked> asked{{.node = roots[1], .depth = grid.depth()},
                                          {.node = roots[3], .depth = grid.depth()}};
    std::ranges::sort(asked);
    CHECK(answered == asked);
}
