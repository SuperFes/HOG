#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/grid/tree.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using Catch::Approx;
using hog::DVec3;
using hog::grid::Blend;
using hog::grid::Blending;
using hog::grid::Shape;
using hog::grid::Surface;
using hog::grid::Tree;

namespace {

constexpr std::uint8_t levels = 6;
constexpr double across = 1 << levels;

DVec3 middle(int x, int y, int z) {
    return DVec3{x + 0.5, y + 0.5, z + 0.5};
}

// The surface of a ball `radius` finest nodes round, centred at `centre`, through each finest
// node: its tangent plane there.
Tree<Shape> ball(DVec3 centre, double radius) {
    return Tree<Shape>::build(levels, [&](int x, int y, int z) {
        const DVec3 out = middle(x, y, z) - centre;
        return Surface::plane(glm::normalize(out), glm::length(out) - radius);
    });
}

// How far the finest node at (x, y, z) lies from the surface as the tree has it, in finest
// nodes: from the plane of the leaf over it.
double held_distance(const Tree<Shape>& tree, int x, int y, int z) {
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    const auto uz = static_cast<std::uint32_t>(z);
    const auto leaf = tree.at(levels, ux, uy, uz);
    const auto shift = static_cast<unsigned>(levels - leaf.depth);
    const double side = std::ldexp(1.0, static_cast<int>(shift));
    const DVec3 leaf_middle =
        (DVec3{static_cast<double>(ux >> shift), static_cast<double>(uy >> shift),
               static_cast<double>(uz >> shift)} +
         0.5) *
        side;
    if (!leaf.value.crosses()) {
        return leaf.value.is_open() ? side : -side;
    }
    return (leaf.value.distance() * side) +
           glm::dot(leaf.value.normal(), middle(x, y, z) - leaf_middle);
}

} // namespace

TEST_CASE("a surface keeps its plane to within its storage", "[grid]") {
    for (std::uint64_t n = 0; n < 500; ++n) {
        const DVec3 raw{static_cast<double>(hog::hash_values(1, n) % 2001) - 1000.0,
                        static_cast<double>(hog::hash_values(2, n) % 2001) - 1000.0,
                        static_cast<double>(hog::hash_values(3, n) % 2001) - 1000.0};
        if (glm::length(raw) < 1.0) {
            continue;
        }
        const DVec3 normal = glm::normalize(raw);
        const double distance = (static_cast<double>(hog::hash_values(4, n) % 1001) / 1000.0) - 0.5;
        const Surface surface = Surface::plane(normal, distance * 0.5);
        REQUIRE(surface.crosses());
        CHECK(glm::length(surface.normal() - normal) < 1e-4);
        CHECK(surface.distance() == Approx(distance * 0.5).margin(1e-7));
    }
}

TEST_CASE("a plane missing its node leaves it all solid or all open", "[grid]") {
    const DVec3 up{0.0, 0.0, 1.0};
    CHECK(Surface::plane(up, 0.6).is_open());
    CHECK(Surface::plane(up, -0.6).is_solid());
    CHECK(Surface::plane(up, 0.4).crosses());
    const DVec3 corner = glm::normalize(DVec3{1.0});
    CHECK(Surface::plane(corner, 0.8).crosses());
    CHECK(Surface::plane(corner, 0.9).is_open());
    CHECK(Surface{}.is_open());
    CHECK(Surface::plane(up, -0.6, 7) == Surface::solid());
    CHECK_FALSE(Surface::from_raw(Surface::solid().raw_distance(), 1, 0, 0));
    CHECK_FALSE(Surface::from_raw(Surface::solid().raw_distance(), 0, 0, 7));
    CHECK(Surface::from_raw(Surface::solid().raw_distance(), 0, 0, 0) == Surface::solid());
}

TEST_CASE("a blend holds two entries and a share, and stands alone where it can", "[grid]") {
    const Blend sand_soil{.first = 4, .second = 3, .share = 0.75};
    const Blend back = Blend::of(sand_soil.entry());
    CHECK(sand_soil.entry() >= Blend::blendable);
    CHECK(back.first == 4);
    CHECK(back.second == 3);
    CHECK(back.share == 0.75);
    // The share counts from whichever shows more.
    CHECK(Blend{.first = 3, .second = 4, .share = 0.25}.entry() == sand_soil.entry());
    CHECK(Blend{.first = 4, .second = 3, .share = 0.99}.entry() == 4);
    CHECK(Blend{.first = 4, .second = 4, .share = 0.5}.entry() == 4);
    CHECK(Blend{.first = 4, .second = Blend::blendable, .share = 0.5}.entry() == 4);
    CHECK(Blend::of(9).first == 9);
    CHECK(Blend::of(9).share == 1.0);
    CHECK(Blend::of(60'000).first == 60'000);

    // Blends tally by their shares: three quarters sand and soil, with soil alone, is soil over
    // sand five to three.
    Blending blending;
    blending.add(sand_soil.entry());
    blending.add(3);
    CHECK(blending.entry() == Blend{.first = 3, .second = 4, .share = 0.625}.entry());
    blending.add(0);
    blending.add(7, 0.1);
    CHECK(Blend::of(blending.entry()).share == 0.625);
    CHECK(Blending{}.entry() == 0);
    Blending tied;
    tied.add(4);
    tied.add(3);
    CHECK(tied.entry() == Blend{.first = 3, .second = 4, .share = 0.5}.entry());
}

TEST_CASE("a surface keeps its roughness to a step, none where it isn't known", "[grid]") {
    const DVec3 up{0.0, 0.0, 1.0};
    const Surface plane = Surface::plane(up, 0.1);
    CHECK_FALSE(plane.roughness());
    const double step = std::exp2(1.0 / (2.0 * Surface::roughness_steps));
    const double roughest = Surface::smoothest * std::exp2(254.0 / Surface::roughness_steps);
    for (int n = 0; Surface::smoothest * std::pow(1.37, n) <= roughest; ++n) {
        const double rough = Surface::smoothest * std::pow(1.37, n);
        CAPTURE(rough);
        const auto held = plane.with_roughness(rough).roughness();
        REQUIRE(held);
        CHECK(held.value() <= rough * step * (1.0 + 1e-12));
        CHECK(held.value() >= rough / step / (1.0 + 1e-12));
    }
    CHECK(plane.with_roughness(0.0).roughness() == Surface::smoothest);
    CHECK(plane.with_roughness(1e-9).roughness() == Surface::smoothest);
    CHECK(plane.with_roughness(1e9).roughness() == Approx(roughest));
    CHECK_FALSE(plane.with_roughness(0.3).with_roughness(std::nullopt).roughness());
    // The plane itself is kept.
    CHECK(plane.with_roughness(0.3).with_roughness(std::nullopt) == plane);
    // A node all solid or open has none.
    CHECK(Surface::solid().with_roughness(0.3) == Surface::solid());
    CHECK(Surface::open().with_roughness(0.3) == Surface::open());
    CHECK_FALSE(Surface::from_raw(Surface::solid().raw_distance(), 0, 0, 0, 5));
}

TEST_CASE("a coarse plane is as rough as the most its children stray from it", "[grid]") {
    const DVec3 up{0.0, 0.0, 1.0};
    const Surface open = Surface::open();
    const auto at = [&](double distance, double rough) {
        return Surface::plane(up, distance).with_roughness(rough);
    };
    // Flat through the lower four, each 0.2 of its side rough: half that of the parent's.
    const Surface flat = Shape::summary(
        {at(0.4, 0.2), at(0.4, 0.2), at(0.4, 0.2), at(0.4, 0.2), open, open, open, open});
    REQUIRE(flat.roughness());
    CHECK(flat.roughness().value() == Approx(0.1).epsilon(0.05));
    // Two a tenth of their side higher than the other two: the parent's plane between them.
    const Surface stepped = Shape::summary(
        {at(0.4, 0.2), at(0.3, 0.2), at(0.4, 0.2), at(0.3, 0.2), open, open, open, open});
    REQUIRE(stepped.roughness());
    CHECK(stepped.roughness().value() == Approx((0.05 + 0.2) / 2.0).epsilon(0.05));
    // Not known where any crossing child's isn't; those it doesn't cross don't count.
    CHECK_FALSE(Shape::summary({at(0.4, 0.2), Surface::plane(up, 0.4), at(0.4, 0.2), at(0.4, 0.2),
                                open, open, open, open})
                    .roughness());
    const auto merged = Shape::merged(
        {at(0.4, 0.2), at(0.4, 0.2), at(0.4, 0.2), at(0.4, 0.2), open, open, open, open}, 1.0);
    REQUIRE(merged);
    CHECK(merged.value().roughness() == flat.roughness());

    const Tree<Shape> tree = Tree<Shape>::build(levels, [&](int x, int y, int z) {
        return Surface::plane(up, middle(x, y, z).z - 20.3).with_roughness(0.01 * (1 + (x % 3)));
    });
    const auto back = Tree<Shape>::decode(tree.encode());
    REQUIRE(back);
    CHECK(back->same_as(tree));
    CHECK(tree.root().value.roughness());
}

TEST_CASE("a shape tree from before roughness reads with none", "[grid]") {
    const Surface plane = Surface::plane(DVec3{0.0, 0.0, 1.0}, 0.1, 3);
    hog::ByteWriter out;
    out.write(std::uint8_t{1}); // the format
    out.write(std::uint8_t{4}); // levels
    out.write_varint(1);
    out.write(std::uint8_t{0}); // a closed leaf
    out.write(std::bit_cast<std::uint32_t>(plane.raw_distance()));
    out.write(plane.raw_u());
    out.write(plane.raw_v());
    out.write(plane.material());
    std::vector<std::byte> bytes = std::move(out).take();
    const auto old = Tree<Shape>::decode(bytes);
    REQUIRE(old);
    CHECK(old->root().value == plane);
    // As this format, it's cut short.
    bytes[0] = std::byte{Shape::format};
    CHECK_FALSE(Tree<Shape>::decode(bytes));
    bytes[0] = std::byte{Shape::format + 1};
    CHECK_FALSE(Tree<Shape>::decode(bytes));
}

TEST_CASE("a coarse plane shows the blend of what the planes under it show", "[grid]") {
    const DVec3 up{0.0, 0.0, 1.0};
    const Surface solid = Surface::solid();
    const Surface open = Surface::open();
    // Flat ground low through the lower four, showing sand three times, soil once.
    const auto showing = [&](std::uint16_t material) { return Surface::plane(up, 0.4, material); };
    const Surface summed =
        Shape::summary({showing(4), showing(3), showing(4), showing(4), open, open, open, open});
    const std::uint16_t blend = Blend{.first = 4, .second = 3, .share = 0.75}.entry();
    REQUIRE(summed.crosses());
    CHECK(summed.material() == blend);
    // Unknown where none crossed shows anything.
    CHECK(Shape::summary({solid, solid, solid, solid, open, open, open, open}).material() == 0);
    // What the ground shows never keeps it from merging: the material tree holds it finer.
    const auto merged = Shape::merged(
        {showing(4), showing(3), showing(4), showing(4), open, open, open, open}, 1.0);
    REQUIRE(merged);
    CHECK(merged.value().material() == blend);

    const Tree<Shape> tree = Tree<Shape>::build(levels, [&](int x, int y, int z) {
        return Surface::plane(up, middle(x, y, z).z - 20.3,
                              static_cast<std::uint16_t>(1 + (x % 3)));
    });
    const auto back = Tree<Shape>::decode(tree.encode());
    REQUIRE(back);
    CHECK(back->same_as(tree));
    CHECK(tree.root().value.material() != 0);
}

TEST_CASE("eight solid and open with no plane between sum up to one between them", "[grid]") {
    const Surface lower = Surface::solid();
    const Surface upper = Surface::open();
    const Surface summed = Shape::summary({lower, lower, lower, lower, upper, upper, upper, upper});
    REQUIRE(summed.crosses());
    CHECK(summed.normal().z == Approx(1.0).margin(1e-4));
    CHECK(summed.distance() == Approx(0.0).margin(1e-6));
    CHECK(Shape::summary({lower, lower, lower, lower, lower, lower, lower, lower}).is_solid());
}

TEST_CASE("flat ground however tilted is one leaf", "[grid]") {
    const DVec3 normal = glm::normalize(DVec3{1.0, 2.0, 3.0});
    const double through = glm::dot(normal, DVec3{across / 2});
    const auto flat = Tree<Shape>::build(levels, [&](int x, int y, int z) {
        return Surface::plane(normal, glm::dot(normal, middle(x, y, z)) - through);
    });
    CHECK(flat.nodes() == 1);
    const Surface root = flat.root().value;
    REQUIRE(root.crosses());
    CHECK(glm::length(root.normal() - normal) < 1e-4);
    CHECK(root.distance() == Approx(0.0).margin(1e-6));
}

TEST_CASE("curved ground splits more the tighter it curves", "[grid]") {
    const auto tight = ball(DVec3{32.0, 32.0, -8.0}, 40.0);
    const auto gentle = ball(DVec3{32.0, 32.0, -1960.0}, 1992.0);
    CHECK(tight.nodes() > 1);
    CHECK(gentle.nodes() < tight.nodes());
    // Fewer leaves than the finest nodes the surface crosses.
    CHECK(tight.leaves() < static_cast<std::size_t>(across * across));
}

TEST_CASE("merged ground stays near the surface it was built from", "[grid]") {
    const DVec3 centre{32.0, 32.0, -8.0};
    const double radius = 40.0;
    const auto tree = ball(centre, radius);
    double worst = 0.0;
    for (int z = 0; z < 64; ++z) {
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const double truth = glm::length(middle(x, y, z) - centre) - radius;
                const auto leaf =
                    tree.at(levels, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                            static_cast<std::uint32_t>(z));
                if (leaf.value.crosses()) {
                    if (std::abs(truth) < 1.0) {
                        worst = std::max(worst, std::abs(held_distance(tree, x, y, z) - truth));
                    }
                } else if (std::abs(truth) > 1.0) {
                    CHECK(leaf.value.is_open() == (truth > 0.0));
                }
            }
        }
    }
    CHECK(worst < 2 * Shape::tolerance);
}

TEST_CASE("a crease is kept as fine as it is", "[grid]") {
    // A ridge along y: two planes meeting between nodes' sides.
    const DVec3 left = glm::normalize(DVec3{-1.0, 0.0, 1.0});
    const DVec3 right = glm::normalize(DVec3{1.0, 0.0, 1.0});
    const DVec3 top{31.3, 0.0, 32.0};
    const auto ridge = Tree<Shape>::build(levels, [&](int x, int y, int z) {
        const DVec3 at = middle(x, y, z);
        const DVec3 normal = at.x < top.x ? left : right;
        return Surface::plane(normal, glm::dot(normal, at - top));
    });
    CHECK(ridge.at(levels, 31, 10, 31).depth == levels);
    CHECK(ridge.at(levels, 8, 10, 8).depth < levels);
    const auto back = Tree<Shape>::decode(ridge.encode());
    REQUIRE(back);
    CHECK(back->same_as(ridge));
}

TEST_CASE("a stand holds its cover and shares in parts summing to the whole", "[grid]") {
    using hog::grid::Stand;
    const Stand third = Stand::of(0.5, {1.0, 1.0, 1.0, 0.0, 0.0, 0.0});
    CHECK(third.ground);
    CHECK(third.cover == 128);
    CHECK(third.shares[0] + third.shares[1] + third.shares[2] == Stand::whole);
    CHECK(third.shares[0] == 85);
    CHECK(Stand::of(2.0, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0}).cover == Stand::whole);
    CHECK(Stand::of(0.0, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0}).shares == std::array<std::uint8_t, 6>{});
    // Never more than the whole however they round.
    const Stand spread = Stand::of(1.0, {0.1, 0.2, 0.3, 0.15, 0.15, 0.1});
    int total = 0;
    for (const std::uint8_t share : spread.shares) {
        total += share;
    }
    CHECK(total == Stand::whole);

    hog::ByteWriter out;
    hog::grid::Woods::write(out, spread);
    hog::grid::Woods::write(out, Stand{});
    const std::vector<std::byte> bytes = std::move(out).take();
    CHECK(bytes.size() == 9);
    hog::ByteReader in{bytes};
    CHECK(hog::grid::Woods::read(in).value() == spread);
    CHECK(hog::grid::Woods::read(in).value() == Stand{});
    std::vector<std::byte> broken = bytes;
    broken[2] = std::byte{0};
    hog::ByteReader short_of{broken};
    CHECK_FALSE(hog::grid::Woods::read(short_of));

    // Only the kinds standing there are written.
    hog::ByteWriter pines;
    hog::grid::Woods::write(pines, Stand::of(0.7, {0.0, 0.0, 0.0, 0.0, 1.0, 0.0}));
    const std::vector<std::byte> few = std::move(pines).take();
    CHECK(few.size() == 3);
    hog::ByteReader few_in{few};
    CHECK(hog::grid::Woods::read(few_in).value() == Stand::of(0.7, {0.0, 0.0, 0.0, 0.0, 1.0, 0.0}));
}

TEST_CASE("woods sum up over the ground under them, and even woods stay coarse", "[grid]") {
    using hog::grid::Stand;
    using hog::grid::Woods;
    const Stand pines = Stand::of(1.0, {1.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    const Stand oaks = Stand::of(0.5, {0.0, 1.0, 0.0, 0.0, 0.0, 0.0});
    // Air beside the ground counts for nothing.
    const Stand half = Woods::summary({pines, oaks, pines, oaks, {}, {}, {}, {}});
    CHECK(half.ground);
    CHECK(half.cover == 192);
    // Pines cover twice what oaks do.
    CHECK(half.shares[0] == 170);
    CHECK(half.shares[1] == 85);
    CHECK(Woods::summary({}) == Stand{});
    CHECK_FALSE(Woods::merged({pines, oaks, pines, oaks, {}, {}, {}, {}}, 1.0));
    const Stand near = Stand::of(0.99, {0.98, 0.02, 0.0, 0.0, 0.0, 0.0});
    CHECK(Woods::merged({pines, near, pines, near, {}, {}, {}, {}}, 1.0));
    CHECK(Woods::merged({}, 1.0) == Stand{});

    // A slope of cover across the nodes merges only where it's gentle.
    const auto sloped = [](double over) {
        return Tree<Woods>::build(levels, [&](int x, int /*y*/, int z) {
            return z != 20 ? Stand{}
                           : Stand::of(std::fmod(x / over, 1.0), {1.0, 1.0, 0.0, 0.0, 0.0, 0.0});
        });
    };
    CHECK(sloped(4096.0).leaves() < sloped(64.0).leaves());
    const Tree<Woods> gentle = sloped(4096.0);
    for (int x = 0; x < 64; x += 7) {
        const auto found = gentle.at(levels, static_cast<std::uint32_t>(x), 3, 20);
        CHECK(found.value.ground);
        CHECK(std::abs(found.value.cover - Stand::of(x / 4096.0, {}).cover) <=
              3 * Woods::tolerance);
    }
}
