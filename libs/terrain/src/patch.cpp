#include <hog/core/assert.hpp>
#include <hog/planet/cube_face.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/patch.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <span>
#include <utility>

namespace hog::terrain {

namespace {

[[nodiscard]] std::int64_t side_of(int level) {
    HOG_ASSERT(level >= 0 && level <= planet::max_level, "corner level out of range");
    return std::int64_t{1} << level;
}

// Skirts hang this many vertex spacings below the patch's lowest ground: enough to meet a
// neighbour a few levels coarser, whose edge runs straight over what this one's follows.
constexpr double skirt_spacings = patch_cells / 4.0;

[[nodiscard]] bool on_face(std::int64_t at, std::int64_t side) noexcept {
    return at >= 0 && at <= side;
}

// A byte from 0 to 1, rounded.
[[nodiscard]] std::uint8_t byte_of(double value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

// How `fates` have the woods fare at a lattice corner.
[[nodiscard]] flora::Fate fate_at(Corner at, const flora::Fates& fates) {
    const auto side = static_cast<double>(side_of(at.level));
    return fates.at(static_cast<std::uint32_t>(at.face), static_cast<double>(at.i) / side,
                    static_cast<double>(at.j) / side);
}

// The woods about a lattice corner on `biome` ground in `habitat`, `point` being where it lies.
[[nodiscard]] Woods woods_at(Corner at, DVec3 point, planet::Biome biome,
                             const flora::Habitat& habitat) {
    const int finer = flora::lattice_level - at.level;
    const Vec2 cell{static_cast<float>(std::ldexp(static_cast<double>(at.i), finer)),
                    static_cast<float>(std::ldexp(static_cast<double>(at.j), finer))};
    const flora::Woodland woodland =
        flora::woodland_at(cell, static_cast<std::uint32_t>(at.face), biome, habitat);
    if (woodland.grown <= 0.0F) {
        return {};
    }
    const double area =
        planet::cell_area(planet::cell_at(point, flora::lattice_level)) * glm::dot(point, point);
    return packed_woods(woodland, area);
}

// A nibble from 0 to 1, rounded.
[[nodiscard]] std::uint8_t nibble_of(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 15.0F));
}

// A corner of `face` that may lie off it; off both ways, where only a cube's corner is, it's
// brought back onto the face along the way it lies least off.
[[nodiscard]] Corner on_one_face(int face, std::int64_t i, std::int64_t j, int level) {
    const std::int64_t side = side_of(level);
    const auto off = [&](std::int64_t at) {
        return at < 0 ? -at : std::max(at - side, std::int64_t{0});
    };
    if (off(i) > 0 && off(j) > 0) {
        std::int64_t& nearer = off(i) <= off(j) ? i : j;
        nearer = std::clamp<std::int64_t>(nearer, 0, side);
    }
    return {.face = face, .i = i, .j = j, .level = level};
}

// Whether a grid point is a patch's or in the border about it the normals need, its corners
// aside.
[[nodiscard]] constexpr bool bordering(int i, int j) noexcept {
    const auto inside = [](int along) { return along >= 0 && along < patch_side; };
    const auto beside = [](int along) { return along >= -1 && along <= patch_side; };
    return beside(i) && beside(j) && (inside(i) || inside(j));
}

struct Blend {
    planet::Biome biome;
    std::uint8_t share;
};

// Of the biomes within `steps` of (i, j) on a grid `width` wide, the most common other than the
// one there, and its share of the two.
[[nodiscard]] Blend blend_at(std::span<const planet::Biome> biomes, int width, int steps, int i,
                             int j) {
    const auto at = [&](int x, int y) {
        return biomes[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                      static_cast<std::size_t>(x)];
    };
    std::array<int, planet::biome_count> counts{};
    for (int y = j - steps; y <= j + steps; ++y) {
        for (int x = i - steps; x <= i + steps; ++x) {
            ++counts.at(std::to_underlying(at(x, y)));
        }
    }
    const planet::Biome own = at(i, j);
    Blend blend{.biome = own, .share = 0};
    int most = 0;
    for (int biome = 0; biome < planet::biome_count; ++biome) {
        const int count = counts.at(static_cast<std::size_t>(biome));
        if (biome != static_cast<int>(own) && count > most) {
            most = count;
            blend.biome = static_cast<planet::Biome>(biome);
        }
    }
    const int mine = counts.at(std::to_underlying(own));
    blend.share =
        byte_of(static_cast<double>(most) / static_cast<double>(std::max(mine + most, 1)));
    return blend;
}

// How far heights on a patch's grid stray from the grid at every other vertex. Each vertex left
// out lies midway between two kept, along a row, a column or the diagonal the cells split along.
[[nodiscard]] double coarser_error(std::span<const double> heights) {
    const auto at = [&](int i, int j) {
        return heights[(static_cast<std::size_t>(j) * patch_side) + static_cast<std::size_t>(i)];
    };
    double error = 0.0;
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            const int across = i % 2;
            const int up = j % 2;
            if (across == 0 && up == 0) {
                continue;
            }
            const double between = (at(i - across, j - up) + at(i + across, j + up)) / 2.0;
            error = std::max(error, std::abs(at(i, j) - between));
        }
    }
    return error;
}

} // namespace

int blend_steps(int level, double radius) {
    const double spacing = radius * std::numbers::pi / 2.0 / static_cast<double>(side_of(level));
    return static_cast<int>(
        std::min(std::floor(biome_blend_reach / spacing), static_cast<double>(most_blend_steps)));
}

Cover packed_cover(const flora::Fate& fate, bool wooded) noexcept {
    const std::uint8_t scar = byte_of(static_cast<double>(fate.scar));
    if (!wooded) {
        return {scar, 0};
    }
    const float gone = 1.0F - fate.living;
    const float fallen = gone - fate.dead;
    return {scar, static_cast<std::uint8_t>((nibble_of(gone) << 4U) | nibble_of(fallen))};
}

std::uint8_t packed_bare(std::int64_t bare) noexcept {
    return byte_of(planet::fixed_to_double(bare));
}

Woods packed_woods(const flora::Woodland& woodland, double cell_area) noexcept {
    const double crowding = 100.0 * static_cast<double>(woodland.grown) / cell_area;
    return {byte_of(crowding / static_cast<double>(woods_crowding)),
            byte_of(static_cast<double>(woodland.height / woods_tallest)),
            byte_of(static_cast<double>(woodland.broadleaf)),
            byte_of(static_cast<double>(woodland.maple)),
            byte_of(static_cast<double>(woodland.palm))};
}

Corner canonical(Corner corner) {
    const std::int64_t side = side_of(corner.level);
    HOG_ASSERT(corner.i >= -side && corner.i <= 2 * side && corner.j >= -side &&
                   corner.j <= 2 * side,
               "corner more than a face off its face");
    HOG_ASSERT(on_face(corner.i, side) || on_face(corner.j, side),
               "corner off its face in both directions");
    // The corners of every face's cells make one lattice over the cube, from 0 to side on each
    // axis, since s and t follow the same warp along every axis.
    const planet::FaceFrame frame = planet::face_frame(corner.face);
    std::array<std::int64_t, 3> lattice{};
    const auto on = [&](planet::Axis axis) -> std::int64_t& {
        return lattice.at(static_cast<std::size_t>(axis.index));
    };
    on(frame.normal) = frame.normal.sign > 0 ? side : 0;
    const auto place = [&](planet::Axis axis, std::int64_t at) {
        const std::int64_t inside = std::clamp<std::int64_t>(at, 0, side);
        on(axis) = inside;
        // Past the edge, the step carries on down the next face.
        on(frame.normal) -= frame.normal.sign * std::abs(at - inside);
    };
    place(frame.u, corner.i);
    place(frame.v, corner.j);
    for (int face = 0; face < planet::face_count; ++face) {
        const planet::FaceFrame candidate = planet::face_frame(face);
        if (on(candidate.normal) == (candidate.normal.sign > 0 ? side : 0)) {
            return {
                .face = face, .i = on(candidate.u), .j = on(candidate.v), .level = corner.level};
        }
    }
    assertion_failed("on a face", "a lattice point off the cube");
}

DVec3 direction(Corner corner) {
    const Corner at = canonical(corner);
    const auto side = static_cast<double>(side_of(at.level));
    return glm::normalize(
        planet::face_uv_to_xyz(at.face, planet::st_to_uv(static_cast<double>(at.i) / side),
                               planet::st_to_uv(static_cast<double>(at.j) / side)));
}

planet::CellId sample_cell(Corner corner) {
    const Corner at = canonical(corner);
    const int shift = planet::max_level - at.level;
    constexpr std::int64_t last = (std::int64_t{1} << planet::max_level) - 1;
    const auto leaf = [&](std::int64_t along) {
        return static_cast<std::uint32_t>(std::min(along << shift, last));
    };
    return planet::CellId::from_face_ij(at.face, leaf(at.i), leaf(at.j), planet::max_level);
}

Patch bake_patch(const planet::World& world, planet::CellId cell, const flora::Fates& fates) {
    HOG_ASSERT(cell.is_valid() && cell.level() <= max_patch_level, "patch level out of range");
    const planet::FaceIJ at = cell.to_face_ij();
    const int level = at.level + patch_depth;
    const std::int64_t first_i = std::int64_t{at.i} << patch_depth;
    const std::int64_t first_j = std::int64_t{at.j} << patch_depth;
    const double radius = world.geology().planet_radius();

    // A border one corner wide around the patch, for the normals; its own corners go unused.
    constexpr int bordered = patch_side + 2;
    const auto inside = [](int along) { return along >= 0 && along < patch_side; };
    const auto bordered_index = [](int i, int j) {
        return (static_cast<std::size_t>(j + 1) * bordered) + static_cast<std::size_t>(i + 1);
    };
    const auto index = [](int i, int j) {
        return (static_cast<std::size_t>(j) * patch_side) + static_cast<std::size_t>(i);
    };
    // A border `steps` corners wide, at least the normals', for the biomes each vertex blends.
    const int steps = blend_steps(level, radius);
    const int reach = std::max(steps, 1);
    const int blended = patch_side + (2 * reach);
    const auto blended_index = [&](int i, int j) {
        return (static_cast<std::size_t>(j + reach) * static_cast<std::size_t>(blended)) +
               static_cast<std::size_t>(i + reach);
    };

    std::vector<DVec3> points(std::size_t{bordered} * bordered);
    std::vector<planet::Biome> biomes(static_cast<std::size_t>(blended) *
                                      static_cast<std::size_t>(blended));
    std::vector<Woods> woods(std::size_t{patch_side} * patch_side);
    std::vector<Cover> covers(woods.size());
    std::vector<planet::Exposure> exposures(woods.size());
    std::vector<double> grid_heights(woods.size());
    HeightRange heights{.lowest = std::numeric_limits<double>::infinity(),
                        .highest = -std::numeric_limits<double>::infinity()};
    const planet::Rivers& rivers = world.geology().terrain().rivers();
    for (int j = -reach; j < patch_side + reach; ++j) {
        for (int i = -reach; i < patch_side + reach; ++i) {
            const Corner corner = on_one_face(at.face, first_i + i, first_j + j, level);
            const planet::CellId sampled = sample_cell(corner);
            const planet::CellState state = world.state(sampled);
            biomes[blended_index(i, j)] = state.biome;
            if (!bordering(i, j)) {
                continue;
            }
            const double height = static_cast<double>(state.elevation) / planet::fixed_one;
            points[bordered_index(i, j)] = direction(corner) * (radius + height);
            if (inside(i) && inside(j)) {
                const flora::Fate fate = fate_at(canonical(corner), fates);
                exposures[index(i, j)] = world.exposure(sampled, state);
                // Nothing grows in a river's channel, though past its mouth it's the bed of the
                // sea or lake it runs into.
                if (rivers.in_channel(sampled) && !rivers.standing(sampled, state.elevation)) {
                    exposures[index(i, j)].bare = planet::fixed_one;
                }
                woods[index(i, j)] = woods_at(
                    canonical(corner), points[bordered_index(i, j)], state.biome,
                    flora::fared(flora::habitat_of(state, exposures[index(i, j)].bare), fate));
                covers[index(i, j)] =
                    packed_cover(fate, flora::trees_of(state.biome).height > 0.0F);
                grid_heights[index(i, j)] = height;
                heights.lowest = std::min(heights.lowest, height);
                heights.highest = std::max(heights.highest, height);
            }
        }
    }

    Patch patch{.cell = cell,
                .origin = WorldPosition{planet::cell_center(cell) * radius},
                .heights = heights,
                .coarser_error = coarser_error(grid_heights),
                .vertices = {}};
    patch.vertices.reserve(patch_vertices);
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            const DVec3 along_i =
                points[bordered_index(i + 1, j)] - points[bordered_index(i - 1, j)];
            const DVec3 along_j =
                points[bordered_index(i, j + 1)] - points[bordered_index(i, j - 1)];
            const Blend blend = blend_at(biomes, blended, steps, i + reach, j + reach);
            patch.vertices.push_back(
                {.position = relative_to(WorldPosition{points[bordered_index(i, j)]}, patch.origin),
                 .normal = Vec3(glm::normalize(glm::cross(along_i, along_j))),
                 .biome = biomes[blended_index(i, j)],
                 .woods = woods[index(i, j)],
                 .cover = covers[index(i, j)],
                 .blend_biome = blend.biome,
                 .blend = blend.share,
                 .rock = exposures[index(i, j)].rock,
                 .bare = packed_bare(exposures[index(i, j)].bare),
                 .layer =
                     static_cast<float>(planet::fixed_to_double(exposures[index(i, j)].layer))});
        }
    }

    const double spacing = radius * std::numbers::pi / 2.0 / static_cast<double>(side_of(level));
    const double depth = (heights.highest - heights.lowest) + (skirt_spacings * spacing);
    for (int edge = 0; edge < 4; ++edge) {
        for (int along = 0; along < patch_side; ++along) {
            const GridPoint above = on_edge(edge, along);
            const DVec3 point = points[bordered_index(above.i, above.j)];
            PatchVertex skirt = patch.vertices[index(above.i, above.j)];
            skirt.position = relative_to(
                WorldPosition{point * (1.0 - (depth / glm::length(point)))}, patch.origin);
            patch.vertices.push_back(skirt);
        }
    }
    return patch;
}

bool reaches(planet::CellId patch, planet::CellId change) {
    const auto overlaps = [&](planet::CellId cell) {
        return cell.contains(change) || change.contains(cell);
    };
    return overlaps(patch) ||
           std::ranges::any_of(planet::vertex_neighbors(patch).cells(), overlaps);
}

} // namespace hog::terrain
