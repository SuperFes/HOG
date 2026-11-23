#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hog::flora {

// A species baked by the tree lab (docs/flora-assets.md): its textures and variants, with the
// meshes, leaves and impostor maps as byte views into the file, laid out as the GPU reads them.

enum class TextureFormat : std::uint8_t {
    Rgba8Srgb = 1, // alpha linear
    Rgba8 = 2,
    Rg16 = 3,
    R8 = 4,
    Rgba16 = 5,
};

struct AssetTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::Rgba8;
    std::vector<std::span<const std::byte>> levels; // largest first
};

enum class MeshRole : std::uint8_t {
    Wood = 1,
    Foliage = 2,
    Card = 3,
};

// Vertices (32 bytes), pieces (32 bytes) in the order they drop out with distance, and indices
// sorted to follow them, so every level of detail is a prefix of the indices.
struct MeshPart {
    MeshRole role = MeshRole::Wood;
    std::uint32_t dims = 0; // how a piece's area grows with its size; 0: never thins
    float target = 0.0F;    // pixels a piece is kept at, at least
    std::int32_t albedo = -1;
    std::int32_t second = -1; // cards: their normal map
    float translucency = 0.0F;
    float cutoff = 0.0F;
    float relief = 0.0F;
    Vec2 uv_scale{1.0F};
    std::uint32_t vertex_count = 0;
    std::uint32_t piece_count = 0;
    std::uint32_t index_count = 0;
    std::span<const std::byte> vertices;
    std::span<const std::byte> pieces;
    std::span<const std::byte> indices;
};

// Leaves as instances of the species' template (40 bytes each), in drop order.
struct LeafPart {
    std::uint32_t count = 0;
    float target = 0.0F;
    std::int32_t albedo = -1;
    std::int32_t turn_mask = -1;
    float translucency = 0.0F;
    float cutoff = 0.0F;
    std::span<const std::byte> leaves;
};

struct ImpostorPart {
    std::uint32_t views = 0;
    std::uint32_t cell = 0; // texels a view spans
    Vec3 centre{0.0F};
    float radius = 0.0F;
    // The crown as the far forest traces it: an ellipsoid of leaves and their density per metre.
    Vec3 crown_centre{0.0F};
    Vec2 crown_radii{0.0F};
    float crown_density = 0.0F;
    // Albedo, normal and leafiness, depth and moment yz, sun visibility, moments.
    std::array<std::int32_t, 5> textures{};
};

struct Variant {
    std::uint32_t seed = 0;
    std::uint32_t age = 0;
    Vec3 lo{0.0F}; // bounds positions are quantized over
    Vec3 hi{0.0F};
    std::vector<MeshPart> meshes;
    std::optional<LeafPart> leaves;
    std::optional<ImpostorPart> impostor;
};

struct LeafTemplate {
    std::uint32_t corners = 0;     // 6, or 0 for species without broad leaves
    std::array<Vec3, 6> offsets{}; // along across, way and face, per metre of the leaf's length
    std::array<Vec2, 6> uvs{};
    std::array<std::uint16_t, 12> indices{};
};

struct Species {
    std::string name;
    float colour_range = 1.0F; // stored colours are linear colour / this, sRGB encoded
    LeafTemplate leaf;
    std::vector<AssetTexture> textures; // by id
    std::vector<Variant> variants;
};

// Views into `bytes`, which must outlive the result.
[[nodiscard]] Result<Species> parse_species(std::span<const std::byte> bytes);

// A species with the file's bytes it views.
struct SpeciesFile {
    std::vector<std::byte> bytes;
    Species species;
};
[[nodiscard]] Result<SpeciesFile> load_species(const std::filesystem::path& path);

// One leaf as stored, decoded.
struct Leaf {
    Vec3 base{0.0F};
    float length = 0.0F;
    Quat rotation{1.0F, 0.0F, 0.0F, 0.0F}; // columns: across, way, face
    float drop = 0.0F;                     // metres a pixel spans when it drops out
    bool fallen = false;
};
[[nodiscard]] Leaf leaf_at(const Variant& variant, const LeafPart& part, std::size_t index);
[[nodiscard]] std::array<Vec3, 6> leaf_corners(const LeafTemplate& shape, const Leaf& leaf);

// How many pieces (or leaves) stay at `metres_per_pixel`, and the indices drawing them.
[[nodiscard]] std::uint32_t kept_pieces(const MeshPart& part, float metres_per_pixel);
[[nodiscard]] std::uint32_t kept_index_count(const MeshPart& part, std::uint32_t pieces);
[[nodiscard]] std::uint32_t kept_leaves(const LeafPart& part, float metres_per_pixel);

[[nodiscard]] float half_to_float(std::uint16_t bits) noexcept;

} // namespace hog::flora
