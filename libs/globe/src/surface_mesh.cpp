#include <hog/core/assert.hpp>
#include <hog/globe/surface_mesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace hog::globe {

Cell chunk_under(DVec3 direction, const Layers& layers) {
    return cell_under(direction, layers.level - chunk_levels);
}

std::vector<Cell> chunks_near(const World& world, DVec3 around, double reach) {
    const Layers& layers = world.generator().layers();
    const double radius = world.generator().planet().radius;
    const Cell first = chunk_under(around, layers);
    // A chunk's middle is never farther than this from its corners.
    const double slack = 2.0 * glm::length(corners(first)[0] - center(first)) * radius;
    const auto apart = [&](Cell chunk) { return glm::length(center(chunk) - around) * radius; };
    std::set<Cell> seen{first};
    std::vector<Cell> open{first};
    std::vector<Cell> near;
    while (!open.empty()) {
        const Cell chunk = open.back();
        open.pop_back();
        if (apart(chunk) <= reach) {
            near.push_back(chunk);
        }
        const CellNeighbors beside = sides(chunk);
        for (const Cell next : beside.all()) {
            if (apart(next) <= reach + slack && seen.insert(next).second) {
                open.push_back(next);
            }
        }
    }
    std::ranges::sort(near, {}, apart);
    return near;
}

namespace {

// Two of a place's three axes: i, j and up its column.
constexpr std::array<std::array<int, 2>, 3> others{{{1, 2}, {2, 0}, {0, 1}}};

// The voxels a chunk's surfaces are drawn from, in a band of layers: its columns and one more all
// round, so each cube of eight voxels at its edges is whole.
class Box {
public:
    static constexpr int across = chunk_side + 2;

    // `column_of` gives each place's index in `cells`, or -1 past the cube's corner.
    Box(const World& world, std::span<const Cell> cells, std::span<const Generator::Column> columns,
        std::span<const int> column_of, int bottom, int up, DVec3 origin)
        : up_{up}, face_{cells[static_cast<std::size_t>(column_of[across + 1])].face},
          column_of_{column_of.begin(), column_of.end()},
          voxels_{world.fill(cells, columns, bottom, up)}, origin_{origin} {
        const Layers& layers = world.generator().layers();
        directions_.reserve(cells.size());
        faces_.reserve(cells.size());
        for (const Cell cell : cells) {
            directions_.push_back(center(cell));
            faces_.push_back(cell.face);
        }
        radii_.reserve(static_cast<std::size_t>(up));
        for (int layer = bottom; layer < bottom + up; ++layer) {
            radii_.push_back(layers.middle(layer));
        }
    }

    [[nodiscard]] int up() const noexcept { return up_; }
    // The chunk's own face.
    [[nodiscard]] std::uint8_t face() const noexcept { return face_; }

    // The voxel at a place in the box, none past the cube's corner.
    [[nodiscard]] const Voxel* at(std::array<int, 3> place) const {
        const int column = column_at(place);
        if (column < 0) {
            return nullptr;
        }
        return &voxels_[(static_cast<std::size_t>(column) * static_cast<std::size_t>(up_)) +
                        static_cast<std::size_t>(place[2])];
    }

    [[nodiscard]] DVec3 point(std::array<int, 3> place) const {
        const auto column = static_cast<std::size_t>(column_at(place));
        return (directions_[column] * radii_[static_cast<std::size_t>(place[2])]) - origin_;
    }

    [[nodiscard]] std::uint8_t face(std::array<int, 3> place) const {
        return faces_[static_cast<std::size_t>(column_at(place))];
    }

private:
    [[nodiscard]] int column_at(std::array<int, 3> place) const {
        return column_of_[static_cast<std::size_t>(place[0]) +
                          (static_cast<std::size_t>(place[1]) * across)];
    }

    int up_;
    std::uint8_t face_;
    std::vector<int> column_of_;
    std::vector<Voxel> voxels_;
    DVec3 origin_;
    std::vector<DVec3> directions_;
    std::vector<std::uint8_t> faces_;
    std::vector<double> radii_;
};

// Where shaped matter meets open space: the distance's zero, increasing outward. What it's made
// of says which side a voxel is on, as a distance under half a millimetre rounds to none.
struct Ground {
    static bool inside(const Voxel& voxel) { return voxel.solid != Material::Air; }
    static bool crosses(const Voxel& a, const Voxel& b) { return inside(a) != inside(b); }
    static double value(const Voxel& voxel) { return metres(voxel.distance); }
    static bool may_cross(std::span<const Voxel* const> corners) {
        return std::ranges::any_of(corners, [](const Voxel* voxel) { return inside(*voxel); }) &&
               std::ranges::any_of(corners, [](const Voxel* voxel) { return !inside(*voxel); });
    }
    // The solid nearest the surface.
    static Material material(std::span<const Voxel* const> corners) {
        const Voxel* nearest = nullptr;
        for (const Voxel* voxel : corners) {
            if (inside(*voxel) && (nearest == nullptr || voxel->distance > nearest->distance)) {
                nearest = voxel;
            }
        }
        return nearest != nullptr ? nearest->solid : Material::Air;
    }
};

// Where liquid filling more than half its voxel meets air. Shaped matter counts as full, so the
// liquid's surface runs into the ground rather than round it.
struct Liquid {
    static bool wet(const Voxel& voxel) {
        return !Ground::inside(voxel) && voxel.liquid_amount >= Voxel::full / 2;
    }
    static bool crosses(const Voxel& a, const Voxel& b) {
        return Ground::inside(a) == Ground::inside(b) && wet(a) != wet(b);
    }
    static bool may_cross(std::span<const Voxel* const> corners) {
        return std::ranges::any_of(corners, [](const Voxel* voxel) { return wet(*voxel); }) &&
               std::ranges::any_of(corners, [](const Voxel* voxel) {
                   return !wet(*voxel) && !Ground::inside(*voxel);
               });
    }
    static double value(const Voxel& voxel) {
        const double share =
            Ground::inside(voxel) ? 1.0 : voxel.liquid_amount / double{Voxel::full};
        return 0.5 - std::min(share, 1.0);
    }
    static Material material(std::span<const Voxel* const> corners) {
        for (const Voxel* voxel : corners) {
            if (wet(*voxel)) {
                return voxel->liquid;
            }
        }
        return Material::Air;
    }
};

constexpr std::uint32_t no_vertex = std::numeric_limits<std::uint32_t>::max();

template <typename Surface>
class Net {
public:
    Net(const Box& box, SurfaceMesh& mesh)
        : box_{&box}, mesh_{&mesh},
          cubes_(std::size_t{cubes_across} * cubes_across * static_cast<std::size_t>(box.up() - 1),
                 no_vertex) {}

    void add() {
        for (int c = 0; c + 1 < box_->up(); ++c) {
            for (int b = 0; b < cubes_across; ++b) {
                for (int a = 0; a < cubes_across; ++a) {
                    vertex({a, b, c});
                }
            }
        }
        for (int c = 0; c < box_->up(); ++c) {
            for (int b = 0; b < Box::across; ++b) {
                for (int a = 0; a < Box::across; ++a) {
                    for (int axis = 0; axis < 3; ++axis) {
                        quad({a, b, c}, axis);
                    }
                }
            }
        }
    }

private:
    static constexpr int cubes_across = Box::across - 1;

    [[nodiscard]] std::uint32_t& cube(std::array<int, 3> at) {
        const auto row = static_cast<std::size_t>(at[1]) +
                         (std::size_t{cubes_across} * static_cast<std::size_t>(at[2]));
        return cubes_[static_cast<std::size_t>(at[0]) + (std::size_t{cubes_across} * row)];
    }

    // The cube's vertex, if the surface passes through it and it's whole.
    void vertex(std::array<int, 3> at) {
        std::array<const Voxel*, 8> corners{};
        for (std::size_t k = 0; k < 8; ++k) {
            corners.at(k) = box_->at(corner_of(at, k));
            if (corners.at(k) == nullptr) {
                return;
            }
        }
        if (!Surface::may_cross(corners)) {
            return;
        }
        DVec3 sum{0.0};
        int crossings = 0;
        std::array<DVec3, 3> along{};
        DVec3 rise{0.0};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::size_t step = std::size_t{1} << axis;
            for (std::size_t k = 0; k < 8; ++k) {
                if ((k & step) != 0) {
                    continue;
                }
                const Voxel& a = *corners.at(k);
                const Voxel& b = *corners.at(k | step);
                const DVec3 from = box_->point(corner_of(at, k));
                const DVec3 to = box_->point(corner_of(at, k | step));
                const double va = Surface::value(a);
                const double vb = Surface::value(b);
                along.at(axis) += to - from;
                rise[static_cast<glm::length_t>(axis)] += vb - va;
                if (Surface::crosses(a, b)) {
                    sum += glm::mix(from, to, std::clamp(va / (va - vb), 0.0, 1.0));
                    ++crossings;
                }
            }
        }
        if (crossings == 0) {
            return;
        }
        const DVec3 position = sum / static_cast<double>(crossings);
        // The field's gradient, from its rise along each of the cube's three edge directions.
        const glm::dmat3 edges = glm::transpose(glm::dmat3{along[0], along[1], along[2]});
        DVec3 normal =
            std::abs(glm::determinant(edges)) > 0.0 ? glm::inverse(edges) * rise : DVec3{0.0};
        if (!(glm::length(normal) > 0.0)) {
            normal = position + mesh_->origin;
        }
        cube(at) = static_cast<std::uint32_t>(mesh_->vertices.size());
        mesh_->vertices.push_back({.position = Vec3{position},
                                   .normal = Vec3{glm::normalize(normal)},
                                   .material = Surface::material(corners)});
    }

    [[nodiscard]] static std::array<int, 3> corner_of(std::array<int, 3> at, std::size_t k) {
        return {at[0] + static_cast<int>(k & 1U), at[1] + static_cast<int>((k >> 1U) & 1U),
                at[2] + static_cast<int>((k >> 2U) & 1U)};
    }

    // Whether the chunk draws the edge from `at` along `axis`: its lower end is in the chunk's
    // own columns, or it crosses from the chunk's face onto a higher one.
    [[nodiscard]] bool owns(std::array<int, 3> at, std::array<int, 3> to, int axis) const {
        const auto own = [](int place) { return place >= 1 && place <= chunk_side; };
        if (axis == 2) {
            return own(at[0]) && own(at[1]);
        }
        if (!own(at[static_cast<std::size_t>(1 - axis)])) {
            return false;
        }
        const std::uint8_t from = box_->face(at);
        const std::uint8_t into = box_->face(to);
        if (from != into) {
            return box_->face() < (from == box_->face() ? into : from);
        }
        return own(at[static_cast<std::size_t>(axis)]);
    }

    void quad(std::array<int, 3> at, int axis) {
        std::array<int, 3> to = at;
        ++to.at(static_cast<std::size_t>(axis));
        const auto [p, q] = others.at(static_cast<std::size_t>(axis));
        if (to[0] >= Box::across || to[1] >= Box::across || to[2] >= box_->up() ||
            at.at(static_cast<std::size_t>(p)) == 0 || at.at(static_cast<std::size_t>(q)) == 0) {
            return;
        }
        const Voxel* a = box_->at(at);
        const Voxel* b = box_->at(to);
        if (a == nullptr || b == nullptr || !Surface::crosses(*a, *b) || !owns(at, to, axis)) {
            return;
        }
        // The four cubes round the edge, anticlockwise about it.
        std::array<std::uint32_t, 4> corners{};
        for (std::size_t k = 0; k < 4; ++k) {
            std::array<int, 3> cube_at = at;
            cube_at.at(static_cast<std::size_t>(p)) -= (k == 0 || k == 3) ? 1 : 0;
            cube_at.at(static_cast<std::size_t>(q)) -= (k == 0 || k == 1) ? 1 : 0;
            if (cube_at[2] + 1 >= box_->up()) {
                return;
            }
            corners.at(k) = cube(cube_at);
            if (corners.at(k) == no_vertex) {
                return;
            }
        }
        // So far it faces along the axis; the outside is where the value is higher.
        if (Surface::value(*a) > Surface::value(*b)) {
            std::swap(corners[1], corners[3]);
        }
        const auto apart = [&](std::uint32_t from, std::uint32_t into) {
            return glm::length(mesh_->vertices[from].position - mesh_->vertices[into].position);
        };
        // Split along the shorter diagonal.
        if (apart(corners[0], corners[2]) <= apart(corners[1], corners[3])) {
            mesh_->indices.insert(mesh_->indices.end(), {corners[0], corners[1], corners[2],
                                                         corners[0], corners[2], corners[3]});
        } else {
            mesh_->indices.insert(mesh_->indices.end(), {corners[0], corners[1], corners[3],
                                                         corners[1], corners[2], corners[3]});
        }
    }

    const Box* box_;
    SurfaceMesh* mesh_;
    std::vector<std::uint32_t> cubes_;
};

// The chunk's columns and one more all round, row by row; none past the cube's corners.
std::vector<std::optional<Cell>> places_of(Cell chunk, const Layers& layers) {
    const Cell corner{.level = static_cast<std::uint8_t>(layers.level),
                      .face = chunk.face,
                      .i = chunk.i * chunk_side,
                      .j = chunk.j * chunk_side};
    std::vector<std::optional<Cell>> places;
    places.reserve(std::size_t{Box::across} * Box::across);
    for (int b = 0; b < Box::across; ++b) {
        for (int a = 0; a < Box::across; ++a) {
            const int i = std::clamp(a - 1, 0, chunk_side - 1);
            const int j = std::clamp(b - 1, 0, chunk_side - 1);
            const Cell inside{.level = corner.level,
                              .face = corner.face,
                              .i = corner.i + static_cast<std::uint32_t>(i),
                              .j = corner.j + static_cast<std::uint32_t>(j)};
            places.push_back(next_to(inside, (a - 1) - i, (b - 1) - j));
        }
    }
    return places;
}

// Layer bands the surfaces lie in, lowest first, apart from each other.
std::vector<std::pair<int, int>> bands_of(const World& world, Cell chunk,
                                          std::span<const Generator::Column> columns) {
    const Generator& generator = world.generator();
    const Layers& layers = generator.layers();
    double lowest = std::numeric_limits<double>::max();
    double highest = std::numeric_limits<double>::lowest();
    for (const Generator::Column& column : columns) {
        lowest = std::min(lowest, column.ground());
        highest = std::max(highest, column.ground());
    }
    const double band = distance_band(layers) + layers.thickness;
    std::vector<std::pair<int, int>> bands{
        {layers.layer_at(lowest - surface_depth), layers.layer_at(highest + band) + 1}};
    if (const auto sea = generator.sea_level(); sea && *sea > lowest) {
        bands.emplace_back(layers.layer_at(*sea - band), layers.layer_at(*sea + band) + 1);
    }
    // Changes held next to a band widen it, a brick at a time.
    const VoxelStore& store = world.store();
    constexpr auto bricks_across = static_cast<std::uint32_t>(chunk_side / brick_side);
    const auto held_in = [&](int brick_layer) {
        for (std::uint32_t j = 0; j < bricks_across; ++j) {
            for (std::uint32_t i = 0; i < bricks_across; ++i) {
                const BrickSite site{.face = chunk.face,
                                     .i = (chunk.i * bricks_across) + i,
                                     .j = (chunk.j * bricks_across) + j,
                                     .layer = static_cast<std::uint32_t>(brick_layer)};
                if (store.brick(site) != nullptr) {
                    return true;
                }
            }
        }
        return false;
    };
    for (auto& [bottom, top] : bands) {
        bottom = std::clamp(bottom, 0, layers.count);
        top = std::clamp(top, 0, layers.count);
        while (bottom > 0 && held_in((bottom - 1) / brick_side)) {
            bottom = ((bottom - 1) / brick_side) * brick_side;
        }
        while (top < layers.count && held_in(top / brick_side)) {
            top = std::min(layers.count, ((top / brick_side) + 1) * brick_side);
        }
    }
    std::ranges::sort(bands);
    std::vector<std::pair<int, int>> merged;
    for (const auto& [bottom, top] : bands) {
        if (!merged.empty() && bottom <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, top);
        } else if (top - bottom >= 2) {
            merged.emplace_back(bottom, top);
        }
    }
    return merged;
}

} // namespace

void add_chunk(SurfaceMesh& mesh, const World& world, Cell chunk) {
    const Layers& layers = world.generator().layers();
    HOG_ASSERT(chunk.is_valid() && chunk.level == layers.level - chunk_levels,
               "a chunk is a cell chunk_levels coarser than the voxels'");
    const std::vector<std::optional<Cell>> places = places_of(chunk, layers);
    std::vector<Cell> cells;
    std::vector<int> column_of;
    column_of.reserve(places.size());
    for (const std::optional<Cell>& place : places) {
        column_of.push_back(place ? static_cast<int>(cells.size()) : -1);
        if (place) {
            cells.push_back(*place);
        }
    }
    const std::vector<Generator::Column> columns =
        world.generator().columns(cells, world.generator().planet().voxel);
    for (const auto& [bottom, top] : bands_of(world, chunk, columns)) {
        const Box box{world, cells, columns, column_of, bottom, top - bottom, mesh.origin};
        Net<Ground>{box, mesh}.add();
        Net<Liquid>{box, mesh}.add();
    }
}

} // namespace hog::globe
