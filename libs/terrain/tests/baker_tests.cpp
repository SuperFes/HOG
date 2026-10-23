#include "shared_world.hpp"

#include <hog/jobs/job_system.hpp>
#include <hog/terrain/baker.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/terrain/stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <thread>
#include <vector>

using hog::planet::CellId;
using hog::terrain::Patch;
using hog::terrain::PatchBaker;
using hog::terrain::testing::land_patch;
using hog::terrain::testing::shared_world;

namespace {

std::vector<CellId> cells() {
    const CellId land = land_patch();
    return {land, land.child(0), land.child(1), land.child(2), land.child(3), land.parent()};
}

bool same(const Patch& a, const Patch& b) {
    return a.cell == b.cell && a.origin.metres() == b.origin.metres() && a.heights == b.heights &&
           std::ranges::equal(a.vertices, b.vertices, [](const auto& v, const auto& w) {
               return v.position == w.position && v.normal == w.normal && v.biome == w.biome;
           });
}

} // namespace

TEST_CASE("patches baked in the background come back as baked in place", "[terrain][baker]") {
    hog::jobs::JobSystem jobs{4};
    PatchBaker baker{shared_world(), jobs};
    for (const CellId cell : cells()) {
        baker.start(cell);
    }
    CHECK(baker.in_flight() == cells().size());

    std::vector<Patch> baked;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (baker.in_flight() > 0 && std::chrono::steady_clock::now() < give_up) {
        for (Patch& patch : baker.collect()) {
            baked.push_back(std::move(patch));
        }
        std::this_thread::yield();
    }
    REQUIRE(baked.size() == cells().size());
    for (const CellId cell : cells()) {
        const auto found = std::ranges::find(baked, cell, &Patch::cell);
        REQUIRE(found != baked.end());
        CHECK(same(*found, hog::terrain::bake_patch(shared_world(), cell)));
    }
    CHECK(baker.collect().empty());
}

TEST_CASE("finishing hands over every bake in flight", "[terrain][baker]") {
    hog::jobs::JobSystem jobs{1};
    PatchBaker baker{shared_world(), jobs};
    for (const CellId cell : cells()) {
        baker.start(cell);
    }
    // Without workers, nothing bakes until waited on.
    CHECK(baker.collect().empty());
    const auto baked = baker.finish();
    CHECK(baked.size() == cells().size());
    CHECK(baker.in_flight() == 0);
}

TEST_CASE("a view streamed from the world settles on what it selects, changes and all",
          "[terrain][baker]") {
    hog::planet::World& world = shared_world();
    hog::jobs::JobSystem jobs{4};
    PatchBaker baker{world, jobs};
    hog::terrain::PatchStream stream{{.capacity = 2'048, .uploads = 64, .bakes = 32}};
    const hog::DVec3 up = hog::planet::cell_center(land_patch());
    const double radius = hog::terrain::testing::planet.radius;
    const hog::DVec3 eye = up * (radius + 3'000.0);
    const hog::DVec3 ahead = glm::normalize(glm::cross(up, hog::DVec3{0.0, 0.0, 1.0}));
    const hog::terrain::Viewpoint view{.eye = hog::WorldPosition{eye},
                                       .forward =
                                           glm::normalize((up * radius) + (ahead * 8'000.0) - eye),
                                       .up = up,
                                       // Coarse, to keep the bakes few.
                                       .viewport_height = 120.0};

    std::vector<hog::terrain::SelectedPatch> selected;
    hog::terrain::StreamFrame frame;
    std::map<CellId, Patch> uploaded;
    const auto settle = [&] {
        for (int round = 0; round < 10'000; ++round) {
            for (Patch& patch : baker.collect()) {
                stream.baked(std::move(patch));
            }
            selected = select_patches(view, radius, {}, stream.known_ground());
            frame = stream.update(selected);
            for (const CellId cell : frame.evicted) {
                uploaded.erase(cell);
            }
            for (Patch& patch : frame.arrived) {
                uploaded.insert_or_assign(patch.cell, std::move(patch));
            }
            for (const CellId cell : frame.bakes) {
                baker.start(cell);
            }
            if (frame.arrived.empty() && frame.bakes.empty() && baker.in_flight() == 0) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    };

    REQUIRE(settle());
    REQUIRE(frame.drawn.size() == selected.size());
    CHECK(stream.held_count() <= 2'048);
    CHECK(uploaded.size() == stream.held_count());
    for (std::size_t at = 0; at < selected.size(); ++at) {
        CHECK(frame.drawn.at(at).cell == selected.at(at).cell);
        CHECK(frame.drawn.at(at).coarser_edges == selected.at(at).coarser_edges);
    }

    // Dug out ahead, with nothing baking.
    const CellId dug = hog::planet::cell_at(glm::normalize(up + (ahead * 0.01)), 10);
    REQUIRE(world.apply({.cell = dug,
                         .time = {},
                         .author = {},
                         .layer = hog::planet::Layer::Player,
                         .payload = hog::planet::Excavate{.depth = 40 * hog::planet::fixed_one}}));
    stream.changed(dug);
    const bool resettled = settle();
    std::vector<CellId> differing;
    std::size_t reached = 0;
    for (const auto& patch : frame.drawn) {
        if (hog::terrain::reaches(patch.cell, dug)) {
            ++reached;
        }
        if (!same(uploaded.at(patch.cell), hog::terrain::bake_patch(world, patch.cell))) {
            differing.push_back(patch.cell);
        }
    }
    world.roll_back({});
    REQUIRE(resettled);
    CHECK(reached > 0);
    CHECK(differing.empty());
}
