#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <set>
#include <vector>

using hog::planet::CellId;
using hog::terrain::HeightRange;
using hog::terrain::Patch;
using hog::terrain::PatchStream;
using hog::terrain::SelectedPatch;
using hog::terrain::StreamFrame;

namespace {

constexpr double radius = 115'000.0;

constexpr CellId parent = CellId::from_face_ij(2, 5, 9, 6);

Patch fake_bake(CellId cell, HeightRange heights = {}) {
    return {.cell = cell,
            .origin = hog::WorldPosition{hog::planet::cell_center(cell) * radius},
            .heights = heights,
            .vertices = {}};
}

// Each cell a metre further than the last.
std::vector<SelectedPatch> selecting(std::initializer_list<CellId> cells) {
    std::vector<SelectedPatch> selected;
    for (const CellId cell : cells) {
        selected.push_back({.cell = cell, .distance = static_cast<double>(selected.size())});
    }
    return selected;
}

std::set<CellId> children(CellId cell) {
    return {cell.child(0), cell.child(1), cell.child(2), cell.child(3)};
}

std::vector<SelectedPatch> selecting_children(CellId cell) {
    return selecting({cell.child(0), cell.child(1), cell.child(2), cell.child(3)});
}

std::set<CellId> drawn(const StreamFrame& frame) {
    std::set<CellId> cells;
    for (const auto& patch : frame.drawn) {
        cells.insert(patch.cell);
    }
    return cells;
}

std::vector<CellId> arrived(const StreamFrame& frame) {
    std::vector<CellId> cells;
    cells.reserve(frame.arrived.size());
    for (const Patch& patch : frame.arrived) {
        cells.push_back(patch.cell);
    }
    return cells;
}

// Updates until everything selected is held, baking whatever's asked for at once.
StreamFrame settle(PatchStream& stream, const std::vector<SelectedPatch>& selected) {
    StreamFrame frame = stream.update(selected);
    for (int round = 0; round < 100 && (!frame.bakes.empty() || !frame.arrived.empty()); ++round) {
        for (const CellId cell : frame.bakes) {
            stream.baked(fake_bake(cell));
        }
        frame = stream.update(selected);
    }
    return frame;
}

} // namespace

TEST_CASE("a stream bakes what's selected, nearest first, so many at a time", "[terrain][stream]") {
    PatchStream stream{{.bakes = 3}};
    const auto selected = selecting_children(parent);
    auto frame = stream.update(selected);
    CHECK(frame.bakes == std::vector{parent.child(0), parent.child(1), parent.child(2)});
    CHECK(frame.drawn.empty());
    CHECK(stream.baking_count() == 3);

    // What's in flight isn't asked for again.
    frame = stream.update(selected);
    CHECK(frame.bakes.empty());
    stream.baked(fake_bake(parent.child(1)));
    frame = stream.update(selected);
    CHECK(arrived(frame) == std::vector{parent.child(1)});
    CHECK(frame.bakes == std::vector{parent.child(3)});
}

TEST_CASE("bakes arrive within the upload budget, nearest first, and are drawn",
          "[terrain][stream]") {
    PatchStream stream{{.uploads = 2}};
    const auto selected = selecting_children(parent);
    for (const CellId cell : stream.update(selected).bakes) {
        stream.baked(fake_bake(cell));
    }
    auto frame = stream.update(selected);
    CHECK(arrived(frame) == std::vector{parent.child(0), parent.child(1)});
    CHECK(drawn(frame) == std::set{parent.child(0), parent.child(1)});
    frame = stream.update(selected);
    CHECK(arrived(frame) == std::vector{parent.child(2), parent.child(3)});
    REQUIRE(frame.drawn.size() == 4);
    CHECK(frame.drawn.front().cell == parent.child(0));
    CHECK(frame.drawn.front().origin.metres() == fake_bake(parent.child(0)).origin.metres());
    CHECK(stream.held_count() == 4);
}

TEST_CASE("a parent stands in for its children until all of them are held", "[terrain][stream]") {
    PatchStream stream;
    settle(stream, selecting({parent}));
    const auto selected = selecting_children(parent);
    auto frame = stream.update(selected);
    CHECK(drawn(frame) == std::set{parent});
    REQUIRE(frame.bakes.size() == 4);
    for (std::size_t child = 0; child < 3; ++child) {
        stream.baked(fake_bake(frame.bakes.at(child)));
    }
    const CellId last = frame.bakes.back();
    frame = stream.update(selected);
    CHECK(frame.arrived.size() == 3);
    CHECK(drawn(frame) == std::set{parent});
    stream.baked(fake_bake(last));
    frame = stream.update(selected);
    CHECK(drawn(frame) == children(parent));
}

TEST_CASE("held children stand in for their parent until it's held", "[terrain][stream]") {
    PatchStream stream;
    settle(stream, selecting_children(parent));
    auto frame = stream.update(selecting({parent}));
    CHECK(drawn(frame) == children(parent));
    CHECK(frame.bakes == std::vector{parent});
    stream.baked(fake_bake(parent));
    frame = stream.update(selecting({parent}));
    CHECK(drawn(frame) == std::set{parent});
}

TEST_CASE("ground nothing stands in for is baked first", "[terrain][stream]") {
    PatchStream stream{{.bakes = 1}};
    const CellId elsewhere = CellId::from_face_ij(4, 1, 1, 6);
    settle(stream, selecting({parent}));
    // The nearer is covered by its parent meanwhile.
    const auto frame = stream.update(selecting({parent.child(0), elsewhere}));
    CHECK(frame.bakes == std::vector{elsewhere});
}

TEST_CASE("a parent standing in is stitched to what's drawn beside it", "[terrain][stream]") {
    const CellId inside = parent.child(0);
    CellId beside{};
    for (const CellId next : hog::planet::edge_neighbors(inside)) {
        if (!parent.contains(next)) {
            beside = next;
        }
    }
    REQUIRE(beside.is_valid());
    PatchStream stream;
    settle(stream, selecting({parent, beside}));

    auto selected = selecting_children(parent);
    selected.push_back({.cell = beside, .distance = 10.0});
    const auto frame = stream.update(selected);
    REQUIRE(drawn(frame) == std::set{parent, beside});
    for (const auto& patch : frame.drawn) {
        if (patch.cell == parent) {
            CHECK(patch.coarser_edges == 0);
            continue;
        }
        const auto neighbours = hog::planet::edge_neighbors(beside);
        for (std::size_t edge = 0; edge < neighbours.size(); ++edge) {
            const bool coarser = ((patch.coarser_edges >> edge) & 1U) != 0;
            CHECK(coarser == parent.contains(neighbours.at(edge)));
        }
    }
}

TEST_CASE("room is made from the patches drawn least recently", "[terrain][stream]") {
    PatchStream stream{{.capacity = 2}};
    const CellId first = parent.child(0);
    const CellId second = parent.child(1);
    const CellId third = parent.child(2);
    settle(stream, selecting({first, second}));
    (void)stream.update(selecting({second}));
    const auto frame = settle(stream, selecting({second, third}));
    CHECK(stream.held(second));
    CHECK(stream.held(third));
    CHECK(!stream.held(first));
    CHECK(drawn(frame) == std::set{second, third});
}

TEST_CASE("what's drawn isn't evicted, so arrivals wait for room", "[terrain][stream]") {
    PatchStream stream{{.capacity = 1}};
    settle(stream, selecting({parent.child(0)}));
    const auto selected = selecting({parent.child(0), parent.child(1)});
    auto frame = stream.update(selected);
    REQUIRE(frame.bakes == std::vector{parent.child(1)});
    stream.baked(fake_bake(parent.child(1)));
    frame = stream.update(selected);
    CHECK(frame.arrived.empty());
    CHECK(frame.evicted.empty());
    CHECK(drawn(frame) == std::set{parent.child(0)});
    // Once it's no longer drawn, it gives way.
    frame = stream.update(selecting({parent.child(1)}));
    CHECK(frame.evicted == std::vector{parent.child(0)});
    CHECK(arrived(frame) == std::vector{parent.child(1)});
}

TEST_CASE("bakes no longer selected when they arrive are dropped", "[terrain][stream]") {
    PatchStream stream;
    (void)stream.update(selecting({parent.child(0)}));
    stream.baked(fake_bake(parent.child(0)));
    const auto frame = stream.update(selecting({parent.child(1)}));
    CHECK(frame.arrived.empty());
    CHECK(!stream.held(parent.child(0)));
    CHECK(stream.heights(parent.child(0)).has_value());
}

TEST_CASE("heights are known from bakes, and a parent's from all its children's",
          "[terrain][stream]") {
    PatchStream stream;
    CHECK(!stream.heights(parent));
    for (int child = 0; child < 4; ++child) {
        CHECK(!stream.heights(parent));
        stream.baked(fake_bake(parent.child(child), {.lowest = child * 10.0, .highest = 100.0}));
    }
    CHECK(stream.heights(parent) == HeightRange{.lowest = 0.0, .highest = 100.0});
    CHECK(!stream.heights(parent.parent()));
    // Its own are tighter, and stay.
    stream.baked(fake_bake(parent, {.lowest = 5.0, .highest = 90.0}));
    stream.baked(fake_bake(parent.child(0), {.lowest = 0.0, .highest = 100.0}));
    CHECK(stream.heights(parent) == HeightRange{.lowest = 5.0, .highest = 90.0});
    const auto known = stream.known_heights();
    CHECK(known(parent) == stream.heights(parent));
}

TEST_CASE("edges towards nothing drawn keep the selection's stitching", "[terrain][stream]") {
    PatchStream stream;
    const std::vector<SelectedPatch> selected{
        {.cell = parent, .coarser_edges = hog::terrain::edge_plus_v, .distance = 0.0}};
    const auto frame = settle(stream, selected);
    REQUIRE(frame.drawn.size() == 1);
    CHECK(frame.drawn.front().coarser_edges == hog::terrain::edge_plus_v);
}
