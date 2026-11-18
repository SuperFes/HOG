#include <hog/client/shape_mesh.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::client::ShapeDrawn;
using hog::client::ShapeMesh;
using hog::client::ShapesDrawn;
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

namespace {

// Metres from the planet's centre out of the solid: a ball of air in rock about `centre`.
Field cave_at(DVec3 centre, double radius) {
    return [=](DVec3 at) { return radius - glm::length(at - centre); };
}

// The page's shape tree `levels` deep, each finest node holding the plane through it.
Tree<Shape> shaped_page(const hog::grid::Grid& grid, Key page, std::uint8_t levels,
                        const Field& out) {
    const DVec3 low = grid.low(page);
    const double side = grid.size(static_cast<std::uint8_t>(page.depth + levels));
    return Tree<Shape>::build(levels, [&](int x, int y, int z) {
        const DVec3 middle = low + (DVec3{x + 0.5, y + 0.5, z + 0.5} * side);
        return Surface::plane(slope_of(out, middle), out(middle) / side, rock);
    });
}

// Pages, their meshes and seams, as one surface in metres from the planet's centre, its
// vertices welded where they meet.
struct Welded {
    std::vector<DVec3> points;
    std::vector<std::size_t> meshes; // which each point was first met in
    std::size_t welded = 0;          // meshes welded in
    std::vector<std::uint32_t> indices;
};

using Cells = std::map<std::array<long long, 3>, std::vector<std::uint32_t>>;

void weld(Welded& into, const ShapeMesh& mesh, Cells& cells) {
    // Within a tenth of a millimetre: well under any gap, well over float rounding.
    constexpr double near = 1e-4;
    std::vector<std::uint32_t> local(mesh.vertices.size());
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        const DVec3 point = mesh.origin + DVec3{mesh.vertices[v].position};
        const std::array<long long, 3> cell{std::llround(point.x / near),
                                            std::llround(point.y / near),
                                            std::llround(point.z / near)};
        std::optional<std::uint32_t> found;
        for (int step = 0; step < 27 && !found; ++step) {
            const auto at = cells.find({cell[0] + (step % 3) - 1, cell[1] + ((step / 3) % 3) - 1,
                                        cell[2] + (step / 9) - 1});
            if (at == cells.end()) {
                continue;
            }
            for (const std::uint32_t id : at->second) {
                // A mesh's vertices are its leaves', each its own, though two may meet at a
                // corner of their leaves.
                if (into.meshes[id] != into.welded && glm::length(into.points[id] - point) < near) {
                    found = id;
                }
            }
        }
        if (!found) {
            found = static_cast<std::uint32_t>(into.points.size());
            into.points.push_back(point);
            into.meshes.push_back(into.welded);
            cells[cell].push_back(*found);
        }
        local[v] = *found;
    }
    for (const std::uint32_t index : mesh.indices) {
        into.indices.push_back(local[index]);
    }
    ++into.welded;
}

// Each page meshed, then each seam, all welded together.
Welded drawn_whole(const ShapesDrawn& base, const std::vector<ShapeDrawn>& pages) {
    std::vector<ShapeMesh> meshes;
    meshes.reserve(pages.size());
    for (const ShapeDrawn& page : pages) {
        meshes.push_back(hog::client::mesh_shape(base.grid(), page));
    }
    ShapesDrawn drawn{base.grid()};
    for (std::size_t p = 0; p < pages.size(); ++p) {
        ShapeDrawn page = pages[p];
        page.mesh = &meshes[p];
        drawn.add(page);
    }
    Welded welded;
    Cells ids;
    for (std::size_t p = 0; p < pages.size(); ++p) {
        weld(welded, meshes[p], ids);
        weld(welded, hog::client::mesh_seam(drawn, pages[p].page), ids);
    }
    return welded;
}

ShapeMesh as_mesh(const Welded& welded) {
    ShapeMesh mesh;
    for (const DVec3 point : welded.points) {
        mesh.vertices.push_back(
            {.position = hog::Vec3{point}, .normal = {}, .materials = {}, .share = 1.0F});
    }
    mesh.indices = welded.indices;
    return mesh;
}

// A closed surface with no holes through it, each edge run once each way: Euler's two.
void check_closed_ball(const Welded& welded) {
    const ShapeMesh mesh = as_mesh(welded);
    REQUIRE(!mesh.indices.empty());
    const Edges edges = edges_of(mesh, 0.0);
    CHECK(edges.oriented);
    CHECK(edges.unpaired == 0);
    CHECK(static_cast<long>(mesh.vertices.size()) - static_cast<long>(edges.edges) +
              static_cast<long>(mesh.indices.size() / 3) ==
          2);
}

} // namespace

TEST_CASE("seams close a cave across pages drawn at different depths", "[client][shape_mesh]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    // The eight pages round the point 0, 0, 0, a cave centred off it.
    const Field out = cave_at(DVec3{0.3, -0.2, 0.4}, 21.3);
    using Levels = std::array<std::uint8_t, 8>;
    for (const Levels& levels : {Levels{6, 5, 4, 6, 3, 6, 5, 4}, Levels{6, 6, 6, 6, 6, 6, 6, 6},
                                 Levels{1, 6, 2, 6, 1, 6, 2, 6}, Levels{0, 6, 0, 6, 0, 6, 0, 6}}) {
        CAPTURE(levels);
        std::vector<Tree<Shape>> trees;
        trees.reserve(8);
        std::vector<ShapeDrawn> pages;
        for (std::uint32_t k = 0; k < 8; ++k) {
            const Key page{
                .depth = 5, .x = 15 + (k & 1U), .y = 15 + ((k >> 1U) & 1U), .z = 15 + (k >> 2U)};
            trees.push_back(shaped_page(grid, page, levels.at(k), out));
            pages.push_back({.page = page,
                             .tree = &trees.back(),
                             .levels = levels.at(k),
                             .under = {},
                             .mesh = nullptr});
        }
        const Welded welded = drawn_whole(ShapesDrawn{grid}, pages);
        check_closed_ball(welded);
        if (std::ranges::min(levels) >= 3) {
            double farthest = 0.0;
            for (const DVec3 point : welded.points) {
                farthest = std::max(farthest, std::abs(out(point)));
            }
            // The coarsest drawn nodes are 8 m.
            CHECK(farthest < near_enough(8.0));
        }
        // Without seams, gaps.
        Welded apart;
        Cells cells;
        for (const ShapeDrawn& page : pages) {
            weld(apart, hog::client::mesh_shape(grid, page), cells);
        }
        CHECK(edges_of(as_mesh(apart), 0.0).unpaired > 0);
    }
}

TEST_CASE("seams close a cave round finer pages drawn in a page", "[client][shape_mesh]") {
    // 1/64 m nodes: pages 64 m rooted 5 deep, and 1 m rooted 11 deep.
    const hog::grid::Grid grid{1000.0, 1.0 / 64.0};
    REQUIRE(grid.levels_under(5) == 6);
    REQUIRE(grid.levels_under(11) == 6);
    const Key outer{.depth = 5, .x = 16, .y = 16, .z = 16};
    const Field out = cave_at(DVec3{31.7, 32.2, 30.9}, 6.3);
    const Tree<Shape> outer_tree = shaped_page(grid, outer, 6, out);
    // Pages under the cave's top half where its surface runs, drawn 2 and 3 deep in turn.
    std::vector<Key> under;
    for (std::uint32_t x = 24; x < 40; ++x) {
        for (std::uint32_t y = 24; y < 40; ++y) {
            for (std::uint32_t z = 31; z < 40; ++z) {
                const Key node{
                    .depth = 11, .x = (16U << 6U) + x, .y = (16U << 6U) + y, .z = (16U << 6U) + z};
                const DVec3 middle = grid.centre(node);
                if (std::abs(out(middle)) < 1.0) {
                    under.push_back(node);
                }
            }
        }
    }
    REQUIRE(under.size() > 20);
    std::ranges::sort(under);
    for (const bool mixed : {false, true}) {
        CAPTURE(mixed);
        std::vector<Tree<Shape>> trees;
        trees.reserve(under.size());
        std::vector<ShapeDrawn> pages{
            {.page = outer, .tree = &outer_tree, .levels = 6, .under = under, .mesh = nullptr}};
        for (std::size_t k = 0; k < under.size(); ++k) {
            const auto levels = static_cast<std::uint8_t>(mixed ? 2 + (k % 2) : 3);
            trees.push_back(shaped_page(grid, under[k], levels, out));
            pages.push_back({.page = under[k],
                             .tree = &trees.back(),
                             .levels = levels,
                             .under = {},
                             .mesh = nullptr});
        }
        // Leaves holding a page drawn under them split round it, and no surface is drawn twice.
        const Welded welded = drawn_whole(ShapesDrawn{grid}, pages);
        if (!mixed) {
            check_closed_ball(welded);
            continue;
        }
        // Where pages drawn at three depths meet at a corner, a leaf may join two sheets at its
        // vertex, but leaves no gap.
        const Edges edges = edges_of(as_mesh(welded), 0.0);
        CHECK(edges.oriented);
        CHECK(edges.unpaired == 0);
    }
}

TEST_CASE("a page touches those beside it and those drawn in it", "[client][shape_mesh]") {
    const hog::grid::Grid grid{1000.0, 1.0 / 64.0};
    const Tree<Shape> tree{6, Surface::solid(), false};
    const Key outer{.depth = 5, .x = 16, .y = 16, .z = 16};
    const Key beside{.depth = 5, .x = 17, .y = 16, .z = 16};
    const Key far{.depth = 5, .x = 18, .y = 16, .z = 16};
    // One on the outer page's side by the one beside it, one well within it.
    const Key at_side{
        .depth = 11, .x = (17U << 6U) - 1, .y = (16U << 6U) + 3, .z = (16U << 6U) + 3};
    const Key within{
        .depth = 11, .x = (16U << 6U) + 30, .y = (16U << 6U) + 30, .z = (16U << 6U) + 30};
    const std::vector<Key> under{within, at_side};
    ShapesDrawn drawn{grid};
    drawn.add({.page = outer, .tree = &tree, .levels = 6, .under = under, .mesh = nullptr});
    drawn.add({.page = beside, .tree = &tree, .levels = 6, .under = {}, .mesh = nullptr});
    drawn.add({.page = far, .tree = &tree, .levels = 6, .under = {}, .mesh = nullptr});
    drawn.add({.page = at_side, .tree = &tree, .levels = 6, .under = {}, .mesh = nullptr});
    drawn.add({.page = within, .tree = &tree, .levels = 6, .under = {}, .mesh = nullptr});
    CHECK(drawn
              .over(Key{
                  .depth = 17, .x = (16U << 12U) + 5, .y = (16U << 12U) + 5, .z = (16U << 12U) + 5})
              ->page == outer);
    CHECK(drawn.over(within.child(3))->page == within);
    CHECK(drawn.over(Key{.depth = 3, .x = 1, .y = 1, .z = 1}) == nullptr);
    auto touching = drawn.touching(beside);
    std::ranges::sort(touching);
    CHECK(touching == std::vector<Key>{outer, far, at_side});
    touching = drawn.touching(outer);
    std::ranges::sort(touching);
    CHECK(touching == std::vector<Key>{beside, within, at_side});
}
