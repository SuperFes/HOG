#include <hog/client/shape_mesh.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::client::ShapeMesh;
using hog::grid::Key;
using hog::grid::Shape;
using hog::grid::Surface;
using hog::grid::Tree;

namespace {

// 1 m nodes 11 deep: pages of 6 levels, 64 m a side, rooted 5 deep.
constexpr Key root{.depth = 5, .x = 16, .y = 16, .z = 16};
constexpr double page_side = 64.0;
constexpr std::uint16_t rock = 7;

// Metres out of the solid at a point, metres from the page's low corner.
using Field = std::function<double(DVec3)>;

DVec3 slope_of(const Field& out, DVec3 at) {
    constexpr double step = 1e-4;
    return glm::normalize(DVec3{out(at + DVec3{step, 0.0, 0.0}) - out(at - DVec3{step, 0.0, 0.0}),
                                out(at + DVec3{0.0, step, 0.0}) - out(at - DVec3{0.0, step, 0.0}),
                                out(at + DVec3{0.0, 0.0, step}) - out(at - DVec3{0.0, 0.0, step})});
}

// The page's shape tree, each finest node holding the plane through it.
Tree<Shape> shaped(const Field& out) {
    return Tree<Shape>::build(6, [&](int x, int y, int z) {
        const DVec3 middle{x + 0.5, y + 0.5, z + 0.5};
        return Surface::plane(slope_of(out, middle), out(middle), rock);
    });
}

double flat(DVec3 at) {
    return at.z - 20.3;
}
double slope(DVec3 at) {
    return glm::dot(at - DVec3{32.0, 32.0, 30.0}, glm::normalize(DVec3{0.3, 0.5, 1.0}));
}
// A ball sunk in flat ground, its underside overhanging.
double ground(DVec3 at) {
    return at.z - 16.2;
}
double ball(DVec3 at) {
    return glm::length(at - DVec3{32.0, 32.0, 28.0}) - 13.7;
}
double overhang(DVec3 at) {
    return std::min(ground(at), ball(at));
}
// Where the ball and ground are both within a node, air between them is thinner than a node, and
// how the surface faces there is down to how the leaves fall.
bool wedge(DVec3 at) {
    return std::abs(ground(at)) < 1.0 && std::abs(ball(at)) < 1.0;
}
// A ball of air in rock.
double cave(DVec3 at) {
    return 12.3 - glm::length(at - DVec3{31.7, 32.2, 30.9});
}

DVec3 at(const ShapeMesh& mesh, std::uint32_t vertex) {
    return DVec3{mesh.vertices.at(vertex).position};
}

struct Edges {
    std::size_t edges = 0;
    std::size_t unpaired = 0;
    bool oriented = true; // no edge run the same way twice
    bool sides = true;    // edges met once all near the page's sides
};

// Edges a triangle each way are inside the surface; met once, at its rim.
Edges edges_of(const ShapeMesh& mesh, double rim) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<int, int>> runs;
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t from = mesh.indices[t + k];
            const std::uint32_t to = mesh.indices[t + ((k + 1) % 3)];
            auto& run = runs[{std::min(from, to), std::max(from, to)}];
            ++(from < to ? run.first : run.second);
        }
    }
    const auto near_side = [&](std::uint32_t vertex) {
        const DVec3 point = at(mesh, vertex);
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            if (point[axis] < rim || point[axis] > page_side - rim) {
                return true;
            }
        }
        return false;
    };
    Edges edges;
    for (const auto& [ends, run] : runs) {
        ++edges.edges;
        edges.oriented = edges.oriented && run.first <= 1 && run.second <= 1;
        if (run.first + run.second == 1) {
            ++edges.unpaired;
            edges.sides = edges.sides && near_side(ends.first) && near_side(ends.second);
        }
    }
    return edges;
}

double farthest_off(const ShapeMesh& mesh, const Field& out) {
    double farthest = 0.0;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        farthest = std::max(farthest, std::abs(out(at(mesh, v))));
    }
    return farthest;
}

// Triangles facing against the field's rise, into the solid, but in `unclear`, and those facing
// down.
struct Facing {
    std::size_t in = 0;
    std::size_t down = 0;
};

Facing facing(
    const ShapeMesh& mesh, const Field& out,
    const std::function<bool(DVec3)>& unclear = [](DVec3) { return false; }) {
    Facing facing;
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        const DVec3 a = at(mesh, mesh.indices[t]);
        const DVec3 b = at(mesh, mesh.indices[t + 1]);
        const DVec3 c = at(mesh, mesh.indices[t + 2]);
        const DVec3 middle = (a + b + c) / 3.0;
        const DVec3 normal = glm::normalize(glm::cross(b - a, c - a));
        facing.in += glm::dot(normal, slope_of(out, middle)) <= 0.0 && !unclear(middle) ? 1U : 0U;
        facing.down += normal.z < -0.5 ? 1U : 0U;
    }
    return facing;
}

// Merged leaves' planes run within Shape::tolerance of their finest nodes', and those within a
// little of a curved surface.
double near_enough(double node) {
    return (Shape::tolerance * node) + 0.1;
}

} // namespace

TEST_CASE("the mesh lies on the surface, closed but at the page's sides", "[client][shape_mesh]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    REQUIRE(grid.levels_under(root.depth) == 6);
    struct Region {
        const char* name;
        Field out;
        std::function<bool(DVec3)> unclear = [](DVec3) { return false; };
    };
    const std::vector<Region> regions{{.name = "flat", .out = flat},
                                      {.name = "slope", .out = slope},
                                      {.name = "overhang", .out = overhang, .unclear = wedge},
                                      {.name = "cave", .out = cave}};
    for (const auto& [name, out, unclear] : regions) {
        CAPTURE(name);
        const ShapeMesh mesh = hog::client::mesh_shape(grid, root, shaped(out), 6);
        REQUIRE(!mesh.indices.empty());
        CHECK(mesh.origin == grid.low(root));
        CHECK(farthest_off(mesh, out) < near_enough(1.0));
        // Leaves are drawn at most four nodes wide.
        const Edges edges = edges_of(mesh, 4.0);
        CHECK(edges.oriented);
        CHECK(edges.sides);
        CHECK(facing(mesh, out, unclear).in == 0);
        for (const auto& vertex : mesh.vertices) {
            CHECK(vertex.materials[0] == rock);
            CHECK(std::abs(glm::length(vertex.normal) - 1.0F) < 1e-3F);
        }
    }
}

TEST_CASE("a cave in a page is a closed ball", "[client][shape_mesh]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    for (const std::uint8_t levels : {std::uint8_t{6}, std::uint8_t{5}, std::uint8_t{4}}) {
        CAPTURE(levels);
        const ShapeMesh mesh = hog::client::mesh_shape(grid, root, shaped(cave), levels);
        const double node = grid.size(static_cast<std::uint8_t>(root.depth + levels));
        const Edges edges = edges_of(mesh, 0.0);
        CHECK(edges.oriented);
        CHECK(edges.unpaired == 0);
        // Euler's: a ball's surface has no holes through it.
        CHECK(static_cast<long>(mesh.vertices.size()) - static_cast<long>(edges.edges) +
                  static_cast<long>(mesh.indices.size() / 3) ==
              2);
        CHECK(farthest_off(mesh, cave) < near_enough(node));
        CHECK(facing(mesh, cave).in == 0);
    }
}

TEST_CASE("an overhang faces down under it", "[client][shape_mesh]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const ShapeMesh mesh = hog::client::mesh_shape(grid, root, shaped(overhang), 6);
    CHECK(facing(mesh, overhang).down > 0);
    CHECK(facing(hog::client::mesh_shape(grid, root, shaped(slope), 6), slope).down == 0);
}

TEST_CASE("flat ground is drawn by wide leaves", "[client][shape_mesh]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const ShapeMesh mesh = hog::client::mesh_shape(grid, root, shaped(flat), 6);
    // Four nodes a side: no more than two triangles for each of 16 by 16.
    CHECK(mesh.indices.size() / 3 <= 2 * 16 * 16);
    for (const auto& vertex : mesh.vertices) {
        CHECK(vertex.normal.z > 0.999F);
    }
}

TEST_CASE("a page all solid or all open draws nothing", "[client][shape_mesh]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    CHECK(hog::client::mesh_shape(grid, root, Tree<Shape>{6, Surface::solid(), false}, 6)
              .indices.empty());
    CHECK(hog::client::mesh_shape(grid, root, shaped(flat), 0).indices.empty());
}
