#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/terrain/stream.hpp>
#include <hog/testing/quick.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <numbers>
#include <set>
#include <utility>
#include <vector>

using Catch::Approx;
using hog::planet::CellId;
using hog::terrain::HeightRange;
using hog::terrain::Patch;
using hog::terrain::PatchStream;
using hog::terrain::SelectedPatch;
using hog::terrain::StreamFrame;

namespace {

constexpr double radius = 115'000.0;

constexpr CellId parent = CellId::from_face_ij(2, 5, 9, 6);

// Baking the selection straight away, without coarser ground first.
constexpr hog::terrain::StreamSettings fine_first{.coarse_steps = 0};

Patch fake_bake(CellId cell, HeightRange heights = {}, double coarser_error = 0.0) {
    return {.cell = cell,
            .origin = hog::WorldPosition{hog::planet::cell_center(cell) * radius},
            .heights = heights,
            .coarser_error = coarser_error,
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
    PatchStream stream{{.bakes = 3, .coarse_steps = 0}};
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
    PatchStream stream{{.uploads = 2, .coarse_steps = 0}};
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
    PatchStream stream{fine_first};
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
    PatchStream stream{fine_first};
    settle(stream, selecting_children(parent));
    auto frame = stream.update(selecting({parent}));
    CHECK(drawn(frame) == children(parent));
    CHECK(frame.bakes == std::vector{parent});
    stream.baked(fake_bake(parent));
    frame = stream.update(selecting({parent}));
    CHECK(drawn(frame) == std::set{parent});
}

TEST_CASE("ground drawn finely isn't turned coarse again for ground still to bake",
          "[terrain][stream]") {
    PatchStream stream{fine_first};
    const CellId near = parent.child(0);
    const CellId beside = parent.child(1);
    settle(stream, selecting({parent}));
    settle(stream, selecting_children(near));

    // The parent is all that's held over the ground beside, but it would hide what's drawn.
    std::vector<SelectedPatch> wider = selecting_children(near);
    wider.push_back({.cell = beside, .distance = 10.0});
    auto frame = stream.update(wider);
    CHECK(drawn(frame) == children(near));
    CHECK(frame.bakes == std::vector{beside});
    stream.baked(fake_bake(beside));
    frame = stream.update(wider);
    std::set<CellId> expected = children(near);
    expected.insert(beside);
    CHECK(drawn(frame) == expected);
}

TEST_CASE("ground selected out of view is baked and held, but not drawn", "[terrain][stream]") {
    PatchStream stream{fine_first};
    std::vector<SelectedPatch> selected = selecting_children(parent);
    selected.at(2).in_view = false;
    selected.at(3).in_view = false;
    const StreamFrame frame = settle(stream, selected);
    CHECK(drawn(frame) == std::set{parent.child(0), parent.child(1)});
    CHECK(stream.held(parent.child(2)));
    CHECK(stream.held(parent.child(3)));

    // Turning to it, it's drawn at once.
    for (SelectedPatch& patch : selected) {
        patch.in_view = !patch.in_view;
    }
    const StreamFrame turned = stream.update(selected);
    CHECK(drawn(turned) == std::set{parent.child(2), parent.child(3)});
    CHECK(turned.bakes.empty());
}

TEST_CASE("ground selected under the sea is baked and held, but not drawn", "[terrain][stream]") {
    PatchStream stream{fine_first};
    std::vector<SelectedPatch> selected = selecting_children(parent);
    selected.at(1).under_sea = true;
    const StreamFrame frame = settle(stream, selected);
    CHECK(drawn(frame) == std::set{parent.child(0), parent.child(2), parent.child(3)});
    CHECK(stream.held(parent.child(1)));
}

TEST_CASE("turning on the spot never turns what's drawn coarser", "[terrain][stream]") {
    // Selected in view only, ground turned to has nothing baked finer than the first, coarsest
    // patches; selected all round, it's baked before it's turned to.
    const bool only_in_view = GENERATE(true, false);
    CAPTURE(only_in_view);
    const hog::DVec3 up = hog::planet::direction_at(0.3, 0.5);
    const hog::DVec3 east = glm::normalize(glm::cross(hog::DVec3{0.0, 0.0, 1.0}, up));
    const hog::DVec3 north = glm::cross(up, east);
    const hog::WorldPosition eye{up * (radius + 1'500.0)};
    constexpr double pitch = 0.2;
    const double turn = only_in_view ? 0.01 : 0.05; // radians a frame
    const int bake_frames = only_in_view ? 3 : 5;
    // Hills, so what's known of the heights changes what's selected as bakes arrive.
    const auto hilly_bake = [](CellId cell) {
        const double middle = 300.0 * std::sin(static_cast<double>(cell.raw() % 997));
        return fake_bake(cell, {.lowest = middle - 300.0, .highest = middle + 300.0});
    };
    PatchStream stream;
    std::deque<std::pair<int, CellId>> baking;
    std::set<CellId> before;
    int coarsened = 0;
    for (int frame = 0; frame < hog::testing::full_or(400, 200); ++frame) {
        const double bearing = turn * frame;
        const hog::DVec3 level = (east * std::cos(bearing)) + (north * std::sin(bearing));
        const hog::DVec3 forward = (level * std::cos(pitch)) - (up * std::sin(pitch));
        hog::terrain::Viewpoint view{
            .eye = eye,
            .forward = forward,
            .up = glm::cross(glm::normalize(glm::cross(forward, up)), forward)};
        view.bend = hog::terrain::bend_from(eye, radius, 0.0);
        while (!baking.empty() && baking.front().first <= frame) {
            stream.baked(hilly_bake(baking.front().second));
            baking.pop_front();
        }
        const auto selected = hog::terrain::select_patches(
            view, radius, {.only_in_view = only_in_view}, stream.known_ground());
        const StreamFrame streamed = stream.update(selected);
        for (const CellId cell : streamed.bakes) {
            baking.emplace_back(frame + bake_frames, cell);
        }
        const std::set<CellId> now = drawn(streamed);
        for (const CellId cell : now) {
            const auto within = before.lower_bound(cell.range_min());
            if (within != before.end() && *within <= cell.range_max() && *within != cell) {
                ++coarsened;
            }
        }
        before = now;
    }
    CHECK(coarsened == 0);
    CHECK(before.size() > 20);
}

TEST_CASE("ground nothing stands in for is baked first", "[terrain][stream]") {
    PatchStream stream{{.bakes = 1, .coarse_steps = 0}};
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
    PatchStream stream{fine_first};
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
    PatchStream stream{{.capacity = 2, .coarse_steps = 0}};
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
    PatchStream stream{{.capacity = 1, .coarse_steps = 0}};
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
    PatchStream stream{fine_first};
    (void)stream.update(selecting({parent.child(0)}));
    stream.baked(fake_bake(parent.child(0)));
    const auto frame = stream.update(selecting({parent.child(1)}));
    CHECK(frame.arrived.empty());
    CHECK(!stream.held(parent.child(0)));
    CHECK(stream.heights(parent.child(0)).has_value());
}

TEST_CASE("heights are known from bakes, and a parent's from all its children's",
          "[terrain][stream]") {
    PatchStream stream{fine_first};
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
    const auto known = stream.known_ground();
    CHECK(known(parent).value_or(hog::terrain::GroundBounds{}).heights == stream.heights(parent));
}

TEST_CASE("edges towards nothing drawn keep the selection's stitching", "[terrain][stream]") {
    PatchStream stream{fine_first};
    const std::vector<SelectedPatch> selected{
        {.cell = parent, .coarser_edges = hog::terrain::edge_plus_v, .distance = 0.0}};
    const auto frame = settle(stream, selected);
    REQUIRE(frame.drawn.size() == 1);
    CHECK(frame.drawn.front().coarser_edges == hog::terrain::edge_plus_v);
}

TEST_CASE("changed ground stays drawn until it's baked again", "[terrain][stream]") {
    PatchStream stream{fine_first};
    const auto selected = selecting({parent});
    settle(stream, selected);
    stream.changed(parent.child(0).child(3));
    auto frame = stream.update(selected);
    CHECK(frame.bakes == std::vector{parent});
    CHECK(drawn(frame) == std::set{parent});
    stream.baked(fake_bake(parent));
    frame = stream.update(selected);
    CHECK(arrived(frame) == std::vector{parent});
    CHECK(frame.evicted.empty());
    CHECK(drawn(frame) == std::set{parent});
    CHECK(stream.held_count() == 1);
    CHECK(stream.update(selected).bakes.empty());
}

TEST_CASE("bakes from before a change are baked again", "[terrain][stream]") {
    PatchStream stream{fine_first};
    const auto selected = selecting({parent.child(0), parent.child(1)});
    REQUIRE(stream.update(selected).bakes.size() == 2);
    // One waiting when the ground changed, one in flight.
    stream.baked(fake_bake(parent.child(0)));
    stream.changed(parent);
    stream.baked(fake_bake(parent.child(1)));
    auto frame = stream.update(selected);
    CHECK(frame.arrived.empty());
    CHECK(frame.bakes == std::vector{parent.child(0), parent.child(1)});
    CHECK(!stream.heights(parent.child(1)));
    for (const CellId cell : frame.bakes) {
        stream.baked(fake_bake(cell));
    }
    frame = stream.update(selected);
    CHECK(drawn(frame) == std::set{parent.child(0), parent.child(1)});
}

TEST_CASE("a change leaves the ground it doesn't reach be", "[terrain][stream]") {
    PatchStream stream{fine_first};
    const CellId elsewhere = CellId::from_face_ij(4, 1, 1, 6);
    const auto selected = selecting({parent, elsewhere});
    settle(stream, selected);
    stream.changed(elsewhere.child(2));
    const auto frame = stream.update(selected);
    CHECK(frame.bakes == std::vector{elsewhere});
    CHECK(stream.heights(parent));
    CHECK(!stream.heights(elsewhere));
}

TEST_CASE("a change forgets the heights it reaches, and the bounds built on them",
          "[terrain][stream]") {
    PatchStream stream{fine_first};
    for (int child = 0; child < 4; ++child) {
        stream.baked(fake_bake(parent.child(child)));
    }
    REQUIRE(stream.heights(parent));
    stream.changed(parent.child(0).child(0));
    CHECK(!stream.heights(parent));
    for (int child = 0; child < 4; ++child) {
        CHECK(!stream.heights(parent.child(child)));
    }
}

TEST_CASE("a change finds every patch it reaches", "[terrain][stream]") {
    // In from a cube's corner, and out across its faces.
    const CellId corner = CellId::from_face_ij(0, 0, 0, 8);
    for (const CellId change : {corner, corner.child(3).child(1), parent.child(2)}) {
        CAPTURE(change.raw());
        std::set<CellId> cells;
        for (const CellId seed :
             {change.child(0).child(0), hog::planet::edge_neighbors(change)[0].child(1),
              hog::planet::edge_neighbors(hog::planet::edge_neighbors(change)[3])[3]}) {
            for (int level = 0; level <= seed.level(); ++level) {
                const CellId above = seed.parent(level);
                cells.insert(above);
                const auto beside = hog::planet::vertex_neighbors(above);
                for (const CellId next : beside.cells()) {
                    cells.insert(next);
                }
            }
        }
        PatchStream stream{fine_first};
        for (const CellId cell : cells) {
            stream.baked(fake_bake(cell));
        }
        stream.changed(change);
        std::size_t reached = 0;
        for (const CellId cell : cells) {
            CAPTURE(cell.raw());
            CHECK(stream.heights(cell).has_value() != hog::terrain::reaches(cell, change));
            if (hog::terrain::reaches(cell, change)) {
                ++reached;
            }
        }
        CHECK(reached > 0);
        CHECK(reached < cells.size());
    }
}

TEST_CASE("ground of unknown heights is bounded by its nearest known ancestor's, with a margin",
          "[terrain][stream]") {
    PatchStream stream{{.margin = 2.0}};
    stream.baked(fake_bake(parent, {.lowest = 10.0, .highest = 20.0}));
    // Patches at the parent's level span 2048 vertex spacings of a face's quarter turn.
    const double spacing = radius * std::numbers::pi / 2.0 / 2'048.0;
    const CellId inside = parent.child(1).child(2);
    REQUIRE(stream.bounds(inside));
    const HeightRange bounds = stream.bounds(inside).value_or(hog::terrain::GroundBounds{}).heights;
    CHECK(bounds.lowest == Approx(10.0 - (2.0 * spacing)));
    CHECK(bounds.highest == Approx(20.0 + (2.0 * spacing)));
    CHECK(stream.known_ground()(inside) == stream.bounds(inside));
    CHECK(!stream.heights(inside));
    CHECK(stream.bounds(parent).value_or(hog::terrain::GroundBounds{}).heights ==
          HeightRange{.lowest = 10.0, .highest = 20.0});
    CHECK(!stream.bounds(parent.parent()));
    CHECK(!stream.bounds(CellId::from_face_ij(4, 1, 1, 6).child(0)));
}

TEST_CASE("missing ground is baked from coarse to fine, a few levels at a time, each standing in",
          "[terrain][stream]") {
    PatchStream stream{{.coarse_steps = 3}};
    const CellId deep = parent.child(0).child(1).child(2).child(3).child(0);
    REQUIRE(deep.level() == 11);
    const auto selected = selecting({deep});
    auto frame = stream.update(selected);
    for (const int level : {2, 5, 8, 11}) {
        CHECK(frame.bakes == std::vector{deep.parent(level)});
        stream.baked(fake_bake(deep.parent(level)));
        frame = stream.update(selected);
        CHECK(arrived(frame) == std::vector{deep.parent(level)});
        CHECK(drawn(frame) == std::set{deep.parent(level)});
    }
    CHECK(frame.bakes.empty());
}

TEST_CASE("selected ground shares the coarser bakes over it", "[terrain][stream]") {
    PatchStream stream{{.coarse_steps = 3}};
    const auto frame = stream.update(selecting_children(parent.child(2).child(1)));
    CHECK(frame.bakes == std::vector{parent.parent(2)});
}

TEST_CASE("settled, a stream draws the same selection again at once", "[terrain][stream]") {
    PatchStream stream{fine_first};
    const auto selected = selecting_children(parent);
    CHECK_FALSE(stream.settled());
    const StreamFrame first = settle(stream, selected);
    REQUIRE(stream.settled());
    const StreamFrame again = stream.update(selected);
    CHECK(again.drawn == first.drawn);
    CHECK(again.bakes.empty());
    CHECK(again.arrived.empty());
    CHECK(again.evicted.empty());
    CHECK(stream.settled());

    // Another selection is worked out afresh.
    const auto fewer = selecting({parent.child(0), parent.child(1)});
    CHECK(drawn(stream.update(fewer)) == std::set{parent.child(0), parent.child(1)});
}

TEST_CASE("a stream with bakes to do isn't settled", "[terrain][stream]") {
    PatchStream stream{fine_first};
    const auto selected = selecting_children(parent);
    const StreamFrame asked = stream.update(selected);
    REQUIRE_FALSE(asked.bakes.empty());
    CHECK_FALSE(stream.settled());
    // In flight, the bakes keep it unsettled.
    CHECK(stream.update(selected).bakes.empty());
    CHECK_FALSE(stream.settled());
    for (const CellId cell : asked.bakes) {
        stream.baked(fake_bake(cell));
    }
    (void)settle(stream, selected);
    REQUIRE(stream.settled());

    stream.changed(parent.child(2));
    CHECK_FALSE(stream.settled());
    const StreamFrame rebaking = stream.update(selected);
    CHECK(std::ranges::contains(rebaking.bakes, parent.child(2)));
    // Still drawn until baked again.
    CHECK(drawn(rebaking) == children(parent));
    stream.baked(fake_bake(parent.child(2)));
    CHECK_FALSE(stream.settled());
}

TEST_CASE("how far the ground strays from a patch is known from its children once all are baked",
          "[terrain][stream]") {
    PatchStream stream{fine_first};
    const auto error = [&](CellId cell) {
        return stream.bounds(cell)
            .value_or(hog::terrain::GroundBounds{.heights = {}, .error = -1.0})
            .error;
    };
    // Its own bake overstates it, measuring a grid twice as coarse.
    stream.baked(fake_bake(parent, {}, 8.0));
    CHECK(error(parent) == 8.0);
    // Ground not yet baked strays half as far as its nearest known ancestor's, a level down.
    CHECK(error(parent.child(2)) == 4.0);
    CHECK(error(parent.child(2).child(1)) == 2.0);
    for (int child = 0; child < 4; ++child) {
        stream.baked(fake_bake(parent.child(child), {}, 1.0 + child));
    }
    // Each child measures how far the parent strays over it.
    CHECK(error(parent) == 4.0);
    CHECK(error(parent.child(3)) == 4.0);
    // Baked again, it keeps what its children measured.
    stream.baked(fake_bake(parent, {}, 8.0));
    CHECK(error(parent) == 4.0);
    // Bounding its children unbaked, a parent's comes from them too.
    const CellId other = parent.child(1).child(2);
    REQUIRE_FALSE(stream.heights(other));
    for (int child = 0; child < 4; ++child) {
        stream.baked(fake_bake(other.child(child), {}, 0.5));
    }
    CHECK(error(other) == 0.5);
}
