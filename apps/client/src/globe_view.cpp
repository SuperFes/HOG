#include <hog/client/globe_view.hpp>
#include <hog/globe/hexel_mesh.hpp>

#include <algorithm>

namespace hog::client {

Vec3 hexel_colour(const globe::Hexel& hexel) {
    using globe::Material;
    Vec3 colour{1.0F, 0.0F, 1.0F};
    switch (hexel.material) {
    case Material::Air:
        colour = {0.8F, 0.9F, 1.0F};
        break;
    case Material::Water:
        colour = {0.05F, 0.2F, 0.45F};
        break;
    case Material::Snow:
        colour = {0.9F, 0.92F, 0.95F};
        break;
    case Material::Soil:
        colour = {0.3F, 0.22F, 0.12F};
        break;
    case Material::Sand:
        colour = {0.76F, 0.68F, 0.5F};
        break;
    case Material::Sediment:
        colour = {0.6F, 0.52F, 0.36F};
        break;
    case Material::Sandstone:
        colour = {0.7F, 0.5F, 0.3F};
        break;
    case Material::Shale:
        colour = {0.4F, 0.38F, 0.35F};
        break;
    case Material::Limestone:
        colour = {0.75F, 0.73F, 0.65F};
        break;
    case Material::Granite:
        colour = {0.55F, 0.5F, 0.48F};
        break;
    case Material::Basalt:
        colour = {0.18F, 0.18F, 0.2F};
        break;
    case Material::Mantle:
        colour = {0.3F, 0.36F, 0.22F};
        break;
    case Material::Iron:
        colour = {0.42F, 0.4F, 0.38F};
        break;
    }
    const double melts = globe::properties(hexel.material).melts;
    if (melts <= 0.0 || hexel.material == Material::Water || hexel.temperature < melts) {
        return colour;
    }
    // Dull red at melting, to yellow-white some hundreds of kelvin past it.
    const auto past = static_cast<float>(std::clamp((hexel.temperature - melts) / 600.0, 0.0, 1.0));
    return glm::mix(Vec3{0.75F, 0.12F, 0.02F}, Vec3{1.0F, 0.85F, 0.45F}, past);
}

terrain::BlockMesh globe_blocks(const globe::Globe& globe) {
    const globe::HexelMesh hexels = globe::mesh_hexels(globe, DVec3{0.0});
    terrain::BlockMesh blocks{
        .origin = WorldPosition{hexels.origin}, .vertices = {}, .indices = {}};
    blocks.vertices.reserve(hexels.vertices.size());
    for (const globe::HexelVertex& vertex : hexels.vertices) {
        blocks.vertices.push_back({.position = vertex.position,
                                   .normal = vertex.normal,
                                   .colour = hexel_colour(vertex.hexel)});
    }
    blocks.indices = hexels.indices;
    return blocks;
}

} // namespace hog::client
