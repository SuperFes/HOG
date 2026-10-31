#include <hog/globe/hex_cell.hpp>
#include <hog/globe/hexel_mesh.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace hog::globe {
namespace {

// What shows through what: air is clearest, then water, then everything else alike.
int clarity(Material material) {
    if (material == Material::Air) {
        return 0;
    }
    return material == Material::Water ? 1 : 2;
}

// A flat polygon, turned to face away from `inside`.
void add_face(HexelMesh& mesh, std::span<const DVec3> corners, DVec3 inside, const Hexel& hexel) {
    DVec3 middle{0.0};
    for (const DVec3& corner : corners) {
        middle += corner;
    }
    middle /= static_cast<double>(corners.size());
    DVec3 normal = glm::normalize(glm::cross(corners[1] - corners[0], corners[2] - corners[0]));
    const bool reversed = glm::dot(normal, middle - inside) < 0.0;
    if (reversed) {
        normal = -normal;
    }
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t k = 0; k < corners.size(); ++k) {
        const DVec3& corner = corners[reversed ? corners.size() - 1 - k : k];
        mesh.vertices.push_back(
            {.position = Vec3{corner - mesh.origin}, .normal = Vec3{normal}, .hexel = hexel});
    }
    for (std::uint32_t k = 1; k + 1 < corners.size(); ++k) {
        for (const std::uint32_t corner : {0U, k, k + 1}) {
            mesh.indices.push_back(first + corner);
        }
    }
}

// The hexel's top or floor, at `radius`.
void add_cap(HexelMesh& mesh, std::span<const DVec3> outline, double radius, DVec3 inside,
             const Hexel& hexel) {
    std::array<DVec3, 6> polygon{};
    for (std::size_t k = 0; k < outline.size(); ++k) {
        polygon[k] = outline[k] * radius;
    }
    add_face(mesh, std::span{polygon}.first(outline.size()), inside, hexel);
}

void add_hexel(HexelMesh& mesh, const Globe& globe, const HexRing& ring, DVec3 up,
               std::span<const Hexel> column, std::size_t shell) {
    const Hexel& hexel = column[shell];
    const int here = clarity(hexel.material);
    if (here == 0) {
        return;
    }
    const auto at = static_cast<int>(shell);
    const double low = globe.shells().floor(at);
    const double high = globe.shells().ceiling(at);
    const DVec3 inside = up * ((low + high) / 2.0);
    const auto outline = ring.outline();
    const Material above = shell + 1 < column.size() ? column[shell + 1].material : Material::Air;
    if (clarity(above) < here) {
        add_cap(mesh, outline, high, inside, hexel);
    }
    if (shell > 0 && clarity(column[shell - 1].material) < here) {
        add_cap(mesh, outline, low, inside, hexel);
    }
    // Side k, from corner k - 1 to corner k, faces neighbour k.
    for (std::size_t k = 0; k < outline.size(); ++k) {
        if (clarity(globe.at(ring.neighbors[k], at).material) >= here) {
            continue;
        }
        const DVec3 from = outline[(k + outline.size() - 1) % outline.size()];
        const DVec3 to = outline[k];
        const std::array<DVec3, 4> side{from * low, to * low, to * high, from * high};
        add_face(mesh, side, inside, hexel);
    }
}

} // namespace

HexelMesh mesh_hexels(const Globe& globe, DVec3 origin) {
    HexelMesh mesh{.origin = origin, .vertices = {}, .indices = {}};
    for (std::uint64_t index = 0; index < cell_count(globe.level()); ++index) {
        const HexCell cell = cell_at_index(globe.level(), index);
        const auto column = globe.column(cell);
        const HexRing ring = globe::ring(cell);
        const DVec3 up = center(cell);
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            add_hexel(mesh, globe, ring, up, column, shell);
        }
    }
    return mesh;
}

} // namespace hog::globe
