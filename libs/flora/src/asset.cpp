#include <hog/core/bytes.hpp>
#include <hog/flora/asset.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace hog::flora {

namespace {

constexpr std::array<char, 8> magic{'H', 'O', 'G', 'F', 'L', 'O', 'R', 'A'};
constexpr std::uint32_t version = 1;
constexpr std::size_t chunk_header = 16;
constexpr std::size_t vertex_size = 32;
constexpr std::size_t piece_size = 32;
constexpr std::size_t leaf_size = 40;
constexpr std::size_t mesh_head = 64;
constexpr std::size_t leaf_head = 32;

std::uint16_t u16_at(std::span<const std::byte> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[at]) |
                                      (std::to_integer<unsigned>(bytes[at + 1]) << 8U));
}

std::uint32_t u32_at(std::span<const std::byte> bytes, std::size_t at) {
    return u16_at(bytes, at) | (static_cast<std::uint32_t>(u16_at(bytes, at + 2)) << 16U);
}

float snorm8_at(std::span<const std::byte> bytes, std::size_t at) {
    const auto value = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(bytes[at]));
    return std::max(static_cast<float>(value) / 127.0F, -1.0F);
}

std::string tag_of(std::span<const std::byte> bytes, std::size_t at) {
    std::string tag(4, '\0');
    std::ranges::transform(bytes.subspan(at, 4), tag.begin(),
                           [](std::byte b) { return static_cast<char>(b); });
    return tag;
}

Vec3 vec3(ValueReader& in) {
    const float x = in.single();
    const float y = in.single();
    return {x, y, in.single()};
}

Result<std::span<const std::byte>> section(std::span<const std::byte> payload, std::size_t at,
                                           std::size_t size) {
    if (at > payload.size() || size > payload.size() - at) {
        return make_error(ErrorCode::Parse, "flora chunk shorter than its contents");
    }
    return payload.subspan(at, size);
}

std::size_t padded(std::size_t size) {
    return (size + 15U) & ~std::size_t{15};
}

Result<AssetTexture> parse_texture(std::span<const std::byte> payload, std::uint32_t& id) {
    ValueReader in{payload};
    id = in.integer<std::uint32_t>();
    AssetTexture texture;
    texture.width = in.integer<std::uint32_t>();
    texture.height = in.integer<std::uint32_t>();
    const auto format = in.integer<std::uint32_t>();
    const auto levels = in.integer<std::uint32_t>();
    if (!in.ok() || format < 1 || format > 5 || levels > 16) {
        return make_error(ErrorCode::Parse, "bad flora texture");
    }
    texture.format = static_cast<TextureFormat>(static_cast<std::uint8_t>(format));
    constexpr std::array<std::size_t, 6> texel_bytes{0, 4, 4, 4, 1, 8};
    std::size_t at = 32;
    std::uint32_t w = texture.width;
    std::uint32_t h = texture.height;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::size_t size = std::size_t{w} * h * texel_bytes.at(format);
        auto bytes = section(payload, at, size);
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        texture.levels.push_back(*bytes);
        at += padded(size);
        w = std::max(w / 2, 1U);
        h = std::max(h / 2, 1U);
    }
    return texture;
}

Result<MeshPart> parse_mesh(std::span<const std::byte> payload) {
    ValueReader in{payload};
    MeshPart mesh;
    const auto role = in.integer<std::uint32_t>();
    mesh.dims = in.integer<std::uint32_t>();
    (void)in.integer<std::uint32_t>();
    mesh.target = in.single();
    mesh.albedo = static_cast<std::int32_t>(in.integer<std::uint32_t>());
    mesh.second = static_cast<std::int32_t>(in.integer<std::uint32_t>());
    mesh.translucency = in.single();
    mesh.cutoff = in.single();
    mesh.relief = in.single();
    const float us = in.single();
    mesh.uv_scale = {us, in.single()};
    mesh.vertex_count = in.integer<std::uint32_t>();
    mesh.piece_count = in.integer<std::uint32_t>();
    mesh.index_count = in.integer<std::uint32_t>();
    if (!in.ok() || role < 1 || role > 3) {
        return make_error(ErrorCode::Parse, "bad flora mesh");
    }
    mesh.role = static_cast<MeshRole>(static_cast<std::uint8_t>(role));
    std::size_t at = mesh_head;
    auto vertices = section(payload, at, std::size_t{mesh.vertex_count} * vertex_size);
    at += std::size_t{mesh.vertex_count} * vertex_size;
    auto pieces = section(payload, at, std::size_t{mesh.piece_count} * piece_size);
    at += std::size_t{mesh.piece_count} * piece_size;
    auto indices = section(payload, at, std::size_t{mesh.index_count} * 4);
    if (!vertices || !pieces || !indices) {
        return make_error(ErrorCode::Parse, "flora mesh shorter than its contents");
    }
    mesh.vertices = *vertices;
    mesh.pieces = *pieces;
    mesh.indices = *indices;
    return mesh;
}

Result<LeafPart> parse_leaves(std::span<const std::byte> payload) {
    ValueReader in{payload};
    LeafPart part;
    part.count = in.integer<std::uint32_t>();
    (void)in.integer<std::uint32_t>();
    part.target = in.single();
    part.albedo = static_cast<std::int32_t>(in.integer<std::uint32_t>());
    part.turn_mask = static_cast<std::int32_t>(in.integer<std::uint32_t>());
    part.translucency = in.single();
    part.cutoff = in.single();
    auto leaves = section(payload, leaf_head, std::size_t{part.count} * leaf_size);
    if (!in.ok() || !leaves) {
        return make_error(ErrorCode::Parse, "bad flora leaves");
    }
    part.leaves = *leaves;
    return part;
}

Result<ImpostorPart> parse_impostor(std::span<const std::byte> payload) {
    ValueReader in{payload};
    ImpostorPart imp;
    imp.views = in.integer<std::uint32_t>();
    imp.cell = in.integer<std::uint32_t>();
    imp.centre = vec3(in);
    imp.radius = in.single();
    imp.crown_centre = vec3(in);
    const float rh = in.single();
    imp.crown_radii = {rh, in.single()};
    imp.crown_density = in.single();
    for (auto& texture : imp.textures) {
        texture = static_cast<std::int32_t>(in.integer<std::uint32_t>());
    }
    if (!in.ok()) {
        return make_error(ErrorCode::Parse, "bad flora impostor");
    }
    return imp;
}

void parse_head(Species& species, std::uint32_t& variants, ValueReader& in) {
    const std::string name = in.text(32);
    species.name = name.substr(0, name.find('\0'));
    variants = in.integer<std::uint32_t>();
    (void)in.integer<std::uint32_t>();
    species.colour_range = in.single();
    species.leaf.corners = in.integer<std::uint32_t>();
    (void)in.text(12);
    for (auto& offset : species.leaf.offsets) {
        offset = vec3(in);
    }
    for (auto& uv : species.leaf.uvs) {
        const float u = in.single();
        uv = {u, in.single()};
    }
    for (auto& index : species.leaf.indices) {
        index = in.integer<std::uint16_t>();
    }
}

} // namespace

float half_to_float(std::uint16_t bits) noexcept {
    const std::uint32_t sign = (bits & 0x8000U) << 16U;
    const std::uint32_t exponent = (bits >> 10U) & 0x1FU;
    const std::uint32_t mantissa = bits & 0x3FFU;
    if (exponent == 0) {
        const float value = std::ldexp(static_cast<float>(mantissa), -24);
        return sign != 0 ? -value : value;
    }
    if (exponent == 31) {
        return std::bit_cast<float>(sign | 0x7F800000U | (mantissa << 13U));
    }
    return std::bit_cast<float>(sign | ((exponent + 112U) << 23U) | (mantissa << 13U));
}

namespace {

// A part of the last variant read.
Result<void> parse_part(std::string_view tag, std::span<const std::byte> payload,
                        Variant& variant) {
    if (tag == "MESH") {
        auto mesh = parse_mesh(payload);
        if (!mesh) {
            return std::unexpected(std::move(mesh.error()));
        }
        variant.meshes.push_back(*mesh);
    } else if (tag == "LEAF") {
        auto leaves = parse_leaves(payload);
        if (!leaves) {
            return std::unexpected(std::move(leaves.error()));
        }
        variant.leaves = *leaves;
    } else if (tag == "IMPO") {
        auto imp = parse_impostor(payload);
        if (!imp) {
            return std::unexpected(std::move(imp.error()));
        }
        variant.impostor = *imp;
    }
    // Unknown chunks are skipped, so newer files still load.
    return {};
}

Result<void> parse_chunk(std::string_view tag, std::span<const std::byte> payload,
                         Species& species) {
    if (tag == "TEXR") {
        std::uint32_t id = 0;
        auto texture = parse_texture(payload, id);
        if (!texture) {
            return std::unexpected(std::move(texture.error()));
        }
        if (id != species.textures.size()) {
            return make_error(ErrorCode::Parse, "flora textures out of order");
        }
        species.textures.push_back(std::move(*texture));
        return {};
    }
    if (tag == "VARI") {
        ValueReader in{payload};
        Variant variant;
        variant.seed = in.integer<std::uint32_t>();
        variant.age = in.integer<std::uint32_t>();
        variant.lo = vec3(in);
        variant.hi = vec3(in);
        if (!in.ok()) {
            return make_error(ErrorCode::Parse, "bad flora variant");
        }
        species.variants.push_back(std::move(variant));
        return {};
    }
    if (species.variants.empty()) {
        return make_error(ErrorCode::Parse, "flora part before any variant");
    }
    return parse_part(tag, payload, species.variants.back());
}

Result<void> check_textures(const Species& species) {
    const auto count = static_cast<std::int32_t>(species.textures.size());
    const auto bad = [count](std::int32_t id) { return id >= count || id < -1; };
    for (const Variant& variant : species.variants) {
        const bool meshes = std::ranges::any_of(variant.meshes, [&](const MeshPart& mesh) {
            return bad(mesh.albedo) || bad(mesh.second);
        });
        const bool leaves =
            variant.leaves && (bad(variant.leaves->albedo) || bad(variant.leaves->turn_mask));
        const bool imp = variant.impostor && std::ranges::any_of(variant.impostor->textures, bad);
        if (meshes || leaves || imp) {
            return make_error(ErrorCode::Parse, "flora part names a missing texture");
        }
    }
    return {};
}

} // namespace

Result<Species> parse_species(std::span<const std::byte> bytes) {
    if (bytes.size() < 16 || !std::ranges::equal(magic, bytes.first(8), {}, {}, [](std::byte b) {
            return static_cast<char>(b);
        })) {
        return make_error(ErrorCode::Parse, "not a flora file");
    }
    if (u32_at(bytes, 8) != version) {
        return make_error(ErrorCode::Unsupported,
                          std::format("flora file version {}", u32_at(bytes, 8)));
    }
    Species species;
    std::uint32_t declared = 0;
    bool headed = false;
    std::size_t at = 16;
    while (at < bytes.size()) {
        if (bytes.size() - at < chunk_header) {
            return make_error(ErrorCode::Parse, "flora chunk cut short");
        }
        const std::string tag = tag_of(bytes, at);
        auto payload = section(bytes, at + chunk_header, u32_at(bytes, at + 4));
        if (!payload) {
            return std::unexpected(std::move(payload.error()));
        }
        at += chunk_header + payload->size();
        if (tag == "SPEC") {
            ValueReader in{*payload};
            parse_head(species, declared, in);
            if (!in.ok()) {
                return make_error(ErrorCode::Parse, "bad flora species head");
            }
            headed = true;
            continue;
        }
        if (!headed) {
            return make_error(ErrorCode::Parse, "flora file without a species head first");
        }
        if (auto done = parse_chunk(tag, *payload, species); !done) {
            return std::unexpected(std::move(done.error()));
        }
    }
    if (!headed || species.variants.size() != declared) {
        return make_error(ErrorCode::Parse, "flora file missing variants");
    }
    if (auto checked = check_textures(species); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    return species;
}

Result<SpeciesFile> load_species(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        return make_error(exists ? ErrorCode::Io : ErrorCode::NotFound,
                          std::format("cannot open {}", path.string()));
    }
    SpeciesFile out;
    std::vector<char> raw{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad()) {
        return make_error(ErrorCode::Io, std::format("cannot read {}", path.string()));
    }
    out.bytes.resize(raw.size());
    std::ranges::transform(raw, out.bytes.begin(),
                           [](char c) { return static_cast<std::byte>(c); });
    auto species = parse_species(out.bytes);
    if (!species) {
        return make_error(species.error().code,
                          std::format("{}: {}", path.string(), species.error().message));
    }
    out.species = std::move(*species);
    return out;
}

std::size_t first_card_mesh(const Variant& variant) noexcept {
    std::size_t first = variant.meshes.size();
    bool any_card = false;
    while (first > 0) {
        const MeshPart& mesh = variant.meshes[first - 1];
        const bool card = mesh.role == MeshRole::Card;
        const bool limb = mesh.role == MeshRole::Wood && mesh.dims == 0;
        if (!card && !limb) {
            break;
        }
        any_card = any_card || card;
        --first;
    }
    return any_card ? first : variant.meshes.size();
}

Leaf leaf_at(const Variant& variant, const LeafPart& part, std::size_t index) {
    const auto bytes = part.leaves.subspan(index * leaf_size, leaf_size);
    const Vec3 unit{static_cast<float>(u16_at(bytes, 0)), static_cast<float>(u16_at(bytes, 2)),
                    static_cast<float>(u16_at(bytes, 4))};
    Leaf leaf;
    leaf.base = variant.lo + unit / 65535.0F * (variant.hi - variant.lo);
    leaf.length = half_to_float(u16_at(bytes, 6));
    // GLM's quaternion constructor takes w first.
    leaf.rotation = glm::normalize(
        Quat{snorm8_at(bytes, 11), snorm8_at(bytes, 8), snorm8_at(bytes, 9), snorm8_at(bytes, 10)});
    leaf.drop = half_to_float(u16_at(bytes, 12));
    leaf.fallen = (std::to_integer<unsigned>(bytes[19]) & 1U) != 0;
    return leaf;
}

std::array<Vec3, 6> leaf_corners(const LeafTemplate& shape, const Leaf& leaf) {
    const Mat3 frame = glm::mat3_cast(leaf.rotation);
    std::array<Vec3, 6> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        Vec3 offset = shape.offsets.at(i);
        if (leaf.fallen) {
            offset.z = 0.0F;
        }
        out.at(i) = leaf.base + frame * offset * leaf.length;
    }
    return out;
}

std::uint32_t kept_pieces(const MeshPart& part, float metres_per_pixel) {
    // Drops fall along the pieces: the first that drops at a finer pixel ends the prefix.
    std::uint32_t lo = 0;
    std::uint32_t hi = part.piece_count;
    while (lo < hi) {
        const std::uint32_t mid = lo + ((hi - lo) / 2);
        if (half_to_float(u16_at(part.pieces, (std::size_t{mid} * piece_size) + 10)) >
            metres_per_pixel) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

std::uint32_t kept_index_count(const MeshPart& part, std::uint32_t pieces) {
    if (pieces == 0) {
        return 0;
    }
    return u32_at(part.pieces, (std::size_t{pieces - 1} * piece_size) + 24);
}

std::uint32_t kept_leaves(const LeafPart& part, float metres_per_pixel) {
    std::uint32_t lo = 0;
    std::uint32_t hi = part.count;
    while (lo < hi) {
        const std::uint32_t mid = lo + ((hi - lo) / 2);
        if (half_to_float(u16_at(part.leaves, (std::size_t{mid} * leaf_size) + 12)) >
            metres_per_pixel) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

} // namespace hog::flora
