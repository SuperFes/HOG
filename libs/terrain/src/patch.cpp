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

// The woods about a lattice corner on `biome` ground in `habitat`, faring as `fates` have them
// there, `point` being where it lies.
[[nodiscard]] Woods woods_at(Corner at, DVec3 point, planet::Biome biome,
                             const flora::Habitat& habitat, const flora::Fates& fates) {
    const int finer = flora::lattice_level - at.level;
    const Vec2 cell{static_cast<float>(std::ldexp(static_cast<double>(at.i), finer)),
                    static_cast<float>(std::ldexp(static_cast<double>(at.j), finer))};
    const auto face = static_cast<std::uint32_t>(at.face);
    const auto side = static_cast<double>(side_of(at.level));
    const flora::Woodland woodland =
        flora::woodland_at(cell, face, biome,
                           flora::fared(habitat, fates.at(face, static_cast<double>(at.i) / side,
                                                          static_cast<double>(at.j) / side)));
    if (woodland.grown <= 0.0F) {
        return {};
    }
    const double area =
        planet::cell_area(planet::cell_at(point, flora::lattice_level)) * glm::dot(point, point);
    return packed_woods(woodland, area);
}

} // namespace

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
    HOG_ASSERT(corner.i >= -1 && corner.i <= side + 1 && corner.j >= -1 && corner.j <= side + 1,
               "corner more than a step off its face");
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

    std::vector<DVec3> points(std::size_t{bordered} * bordered);
    std::vector<planet::Biome> biomes(std::size_t{patch_side} * patch_side);
    std::vector<Woods> woods(biomes.size());
    HeightRange heights{.lowest = std::numeric_limits<double>::infinity(),
                        .highest = -std::numeric_limits<double>::infinity()};
    for (int j = -1; j <= patch_side; ++j) {
        for (int i = -1; i <= patch_side; ++i) {
            if (!inside(i) && !inside(j)) {
                continue;
            }
            const Corner corner{
                .face = at.face, .i = first_i + i, .j = first_j + j, .level = level};
            const planet::CellState state = world.state(sample_cell(corner));
            const double height = static_cast<double>(state.elevation) / planet::fixed_one;
            points[bordered_index(i, j)] = direction(corner) * (radius + height);
            if (inside(i) && inside(j)) {
                biomes[index(i, j)] = state.biome;
                woods[index(i, j)] = woods_at(canonical(corner), points[bordered_index(i, j)],
                                              state.biome, flora::habitat_of(state), fates);
                heights.lowest = std::min(heights.lowest, height);
                heights.highest = std::max(heights.highest, height);
            }
        }
    }

    Patch patch{.cell = cell,
                .origin = WorldPosition{planet::cell_center(cell) * radius},
                .heights = heights,
                .vertices = {}};
    patch.vertices.reserve(patch_vertices);
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            const DVec3 along_i =
                points[bordered_index(i + 1, j)] - points[bordered_index(i - 1, j)];
            const DVec3 along_j =
                points[bordered_index(i, j + 1)] - points[bordered_index(i, j - 1)];
            patch.vertices.push_back(
                {.position = relative_to(WorldPosition{points[bordered_index(i, j)]}, patch.origin),
                 .normal = Vec3(glm::normalize(glm::cross(along_i, along_j))),
                 .biome = biomes[index(i, j)],
                 .woods = woods[index(i, j)]});
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
