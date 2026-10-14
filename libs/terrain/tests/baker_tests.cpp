#include "shared_world.hpp"

#include <hog/jobs/job_system.hpp>
#include <hog/terrain/baker.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/terrain/stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
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

TEST_CASE("a view streamed from the world settles on what it selects", "[terrain][baker]") {
    hog::jobs::JobSystem jobs{4};
    PatchBaker baker{shared_world(), jobs};
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
    bool settled = false;
    for (int round = 0; round < 10'000 && !settled; ++round) {
        for (Patch& patch : baker.collect()) {
            stream.baked(std::move(patch));
        }
        selected = select_patches(view, radius, {}, stream.known_heights());
        frame = stream.update(selected);
        for (const CellId cell : frame.bakes) {
            baker.start(cell);
        }
        settled = frame.arrived.empty() && frame.bakes.empty() && baker.in_flight() == 0;
        std::this_thread::yield();
    }
    REQUIRE(settled);
    REQUIRE(frame.drawn.size() == selected.size());
    CHECK(stream.held_count() <= 2'048);
    for (std::size_t at = 0; at < selected.size(); ++at) {
        CHECK(frame.drawn.at(at).cell == selected.at(at).cell);
        CHECK(frame.drawn.at(at).coarser_edges == selected.at(at).coarser_edges);
    }
}
