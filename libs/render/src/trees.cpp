#include <hog/core/assert.hpp>
#include <hog/render/blocks.hpp>
#include <hog/render/device.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/render/trees.hpp>
#include <hog/shaders/trees.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <numbers>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    Vec4 foot_pixel;
    Vec4 x_tint;
    Vec4 y_translucency;
    Vec4 z_cutoff;
    Vec4 lo_target;
    Vec4 hi_dims;
    Vec4 uv_calm;
};
static_assert(sizeof(Push) <= 128);

struct Frame {
    Mat4 clip_from_eye;
    Vec4 sun;
};

struct FarPush {
    Vec4 origin_first;
    Vec4 up_height;
    Vec4 radii_reach;
    Vec4 centre_radius;
    Vec4 views_cutoff;
};
static_assert(sizeof(FarPush) <= 128);

struct Instance {
    Vec4 foot_tint;
    Vec4 x_hash;
    Vec4 y;
};

// A tree's own axes, x and y up, unscaled, where it stands, turned and leaning as it does.
std::pair<DVec3, DVec3> axes_of(DVec3 up, const flora::Planted& tree) {
    const flora::Across across = flora::across_of(up);
    const double turn = static_cast<double>(tree.turn) * 2.0 * std::numbers::pi;
    const DVec3 leaning = glm::normalize(up + (across.east * static_cast<double>(tree.lean.x)) +
                                         (across.north * static_cast<double>(tree.lean.y)));
    const DVec3 facing = (across.east * std::cos(turn)) + (across.north * std::sin(turn));
    return {glm::normalize(facing - (leaning * glm::dot(facing, leaning))), leaning};
}

struct Template {
    std::array<Vec4, 6> corners;
    std::array<Vec4, 3> uvs;
    std::array<glm::uvec4, 3> indices;
    Vec4 colour_range;
};

// Leaves thin as their area does.
constexpr float leaf_dims = 2.0F;

constexpr VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type) {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = both,
            .pImmutableSamplers = nullptr};
}

constexpr std::array descriptors{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
    binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
    binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
    binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
    binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
    binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
    binding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
    binding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
};

struct Format {
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::size_t bytes = 0; // a texel
};

Format format_of(flora::TextureFormat format) {
    switch (format) {
    case flora::TextureFormat::Rgba8Srgb:
        return {.format = VK_FORMAT_R8G8B8A8_SRGB, .bytes = 4};
    case flora::TextureFormat::Rgba8:
        return {.format = VK_FORMAT_R8G8B8A8_UNORM, .bytes = 4};
    case flora::TextureFormat::Rg16:
        return {.format = VK_FORMAT_R16G16_UNORM, .bytes = 4};
    case flora::TextureFormat::R8:
        return {.format = VK_FORMAT_R8_UNORM, .bytes = 1};
    case flora::TextureFormat::Rgba16:
        return {.format = VK_FORMAT_R16G16B16A16_UNORM, .bytes = 8};
    }
    return {};
}

float half_at(std::span<const std::byte> bytes, std::size_t at) {
    std::uint16_t bits = 0;
    std::memcpy(&bits, bytes.subspan(at, 2).data(), 2);
    return flora::half_to_float(bits);
}

std::uint32_t u32_at(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.subspan(at, 4).data(), 4);
    return value;
}

template <typename T>
Result<T> or_error(Result<T> made, std::string_view what) {
    if (!made) {
        return make_error(made.error().code, std::format("{}: {}", what, made.error().message));
    }
    return made;
}

Vec4 vec4(DVec3 v, double w) {
    return Vec4{Vec3{v}, static_cast<float>(w)};
}

} // namespace

double pixel_metres(double distance, double vertical_fov, std::uint32_t height) {
    return distance * 2.0 * std::tan(vertical_fov / 2.0) / std::max<double>(height, 1.0);
}

std::uint32_t kept_of(std::span<const float> drops, float metres_per_pixel) {
    const auto end = std::ranges::partition_point(
        drops, [metres_per_pixel](float drop) { return drop > metres_per_pixel; });
    return static_cast<std::uint32_t>(end - drops.begin());
}

Result<Trees> Trees::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                            std::uint32_t frames) {
    Trees trees;
    trees.device_ = &device;
    auto meshes = make_pipeline(device, {.spirv = shaders::trees,
                                         .vertex_entry = "vertex_mesh",
                                         .colour_format = colour_format,
                                         .depth_format = depth_format,
                                         .push_constant_bytes = sizeof(Push),
                                         .descriptors = descriptors});
    if (!meshes) {
        return std::unexpected(std::move(meshes).error());
    }
    trees.meshes_ = std::move(meshes).value();
    auto leaves = make_pipeline(device, {.spirv = shaders::trees,
                                         .vertex_entry = "vertex_leaf",
                                         .colour_format = colour_format,
                                         .depth_format = depth_format,
                                         .push_constant_bytes = sizeof(Push),
                                         .descriptors = descriptors});
    if (!leaves) {
        return std::unexpected(std::move(leaves).error());
    }
    trees.leaves_ = std::move(leaves).value();
    auto impostors = make_pipeline(device, {.spirv = shaders::trees,
                                            .vertex_entry = "vertex_impostor",
                                            .fragment_entry = "fragment_impostor",
                                            .colour_format = colour_format,
                                            .depth_format = depth_format,
                                            .push_constant_bytes = sizeof(FarPush),
                                            .descriptors = descriptors});
    if (!impostors) {
        return std::unexpected(std::move(impostors).error());
    }
    trees.impostors_ = std::move(impostors).value();
    auto atlas_sampler = make_blurring_sampler(device);
    if (!atlas_sampler) {
        return std::unexpected(std::move(atlas_sampler).error());
    }
    trees.atlas_sampler_ = std::move(atlas_sampler).value();
    constexpr std::array<std::byte, 4> white{std::byte{255}, std::byte{255}, std::byte{255},
                                             std::byte{255}};
    auto image = make_filled_image(device, {.format = VK_FORMAT_R8G8B8A8_UNORM}, white);
    if (!image) {
        return std::unexpected(std::move(image).error());
    }
    trees.white_ = std::move(image).value();
    auto sampler = make_tiling_sampler(device);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    trees.sampler_ = std::move(sampler).value();
    for (std::uint32_t i = 0; i < frames; ++i) {
        auto buffer =
            make_buffer(device, sizeof(Frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Memory::Upload);
        if (!buffer) {
            return std::unexpected(std::move(buffer).error());
        }
        trees.frames_.push_back(std::move(buffer).value());
    }
    return trees;
}

Result<> Trees::upload_texture(Species& species, const flora::Species& from, std::int32_t id) {
    if (id < 0 || static_cast<std::size_t>(id) >= from.textures.size()) {
        return {};
    }
    auto& slot = species.textures.at(static_cast<std::size_t>(id));
    if (slot) {
        return {};
    }
    const flora::AssetTexture& texture = from.textures.at(static_cast<std::size_t>(id));
    const Format format = format_of(texture.format);
    // Only the mips laid out as the GPU halves them.
    std::vector<std::byte> texels;
    std::uint32_t levels = 0;
    for (const auto& level : texture.levels) {
        const std::size_t width = std::max(texture.width >> levels, 1U);
        const std::size_t height = std::max(texture.height >> levels, 1U);
        if (level.size() != width * height * format.bytes) {
            break;
        }
        texels.insert(texels.end(), level.begin(), level.end());
        ++levels;
    }
    if (format.format == VK_FORMAT_UNDEFINED || levels == 0) {
        return make_error(ErrorCode::InvalidArgument, std::format("texture {} unreadable", id));
    }
    auto image = make_filled_image(*device_,
                                   {.format = format.format,
                                    .extent = {.width = texture.width, .height = texture.height},
                                    .levels = levels},
                                   texels);
    if (!image) {
        return std::unexpected(std::move(image).error());
    }
    slot = std::move(image).value();
    return {};
}

VkImageView Trees::texture(const Species& species, std::int32_t id) const {
    if (id >= 0 && static_cast<std::size_t>(id) < species.textures.size()) {
        if (const auto& image = species.textures[static_cast<std::size_t>(id)]) {
            return image->view();
        }
    }
    return white_.view();
}

Result<Trees::Variant> Trees::upload_variant(Species& species, const flora::Species& from,
                                             const flora::Variant& variant) {
    Variant out{.lo = variant.lo,
                .hi = variant.hi,
                .meshes = {},
                .leaves = std::nullopt,
                .impostor = std::nullopt};
    for (const flora::MeshPart& mesh : variant.meshes) {
        if (mesh.role == flora::MeshRole::Card || mesh.index_count == 0) {
            continue;
        }
        Part part{.role = mesh.role,
                  .dims = mesh.dims,
                  .target = mesh.target,
                  .translucency = mesh.translucency,
                  .cutoff = mesh.cutoff,
                  .bark = mesh.relief > 0.0F,
                  .uv_scale = mesh.uv_scale,
                  .albedo = mesh.albedo,
                  .records = {},
                  .pieces = {},
                  .indices = {},
                  .drops = {},
                  .index_ends = {}};
        for (std::uint32_t i = 0; i < mesh.piece_count; ++i) {
            part.drops.push_back(half_at(mesh.pieces, (std::size_t{i} * 32) + 10));
            part.index_ends.push_back(u32_at(mesh.pieces, (std::size_t{i} * 32) + 24));
        }
        auto records =
            make_filled_buffer(*device_, mesh.vertices, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto pieces = make_filled_buffer(*device_, mesh.pieces, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto indices = make_filled_buffer(*device_, mesh.indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        if (!records || !pieces || !indices) {
            return make_error(ErrorCode::Internal, "tree mesh buffers");
        }
        part.records = std::move(records).value();
        part.pieces = std::move(pieces).value();
        part.indices = std::move(indices).value();
        if (auto uploaded = upload_texture(species, from, mesh.albedo); !uploaded) {
            return std::unexpected(std::move(uploaded).error());
        }
        out.meshes.push_back(std::move(part));
    }
    if (variant.leaves && variant.leaves->count > 0) {
        const flora::LeafPart& leaves = *variant.leaves;
        Leaves part{.target = leaves.target,
                    .translucency = leaves.translucency,
                    .cutoff = leaves.cutoff,
                    .albedo = leaves.albedo,
                    .records = {},
                    .drops = {}};
        for (std::uint32_t i = 0; i < leaves.count; ++i) {
            part.drops.push_back(half_at(leaves.leaves, (std::size_t{i} * 40) + 12));
        }
        auto records =
            make_filled_buffer(*device_, leaves.leaves, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!records) {
            return make_error(ErrorCode::Internal, "tree leaf buffer");
        }
        part.records = std::move(records).value();
        if (auto uploaded = upload_texture(species, from, leaves.albedo); !uploaded) {
            return std::unexpected(std::move(uploaded).error());
        }
        out.leaves = std::move(part);
    }
    if (variant.impostor) {
        const flora::ImpostorPart& impostor = *variant.impostor;
        for (const std::int32_t id : impostor.textures) {
            if (auto uploaded = upload_texture(species, from, id); !uploaded) {
                return std::unexpected(std::move(uploaded).error());
            }
        }
        out.impostor = Impostor{.views = impostor.views,
                                .centre = impostor.centre,
                                .radius = impostor.radius,
                                .textures = impostor.textures};
    }
    return out;
}

Result<> Trees::add(const flora::Species& from, std::uint32_t most_variants) {
    Species species;
    species.colour_range = from.colour_range;
    species.textures.resize(from.textures.size());
    Template shape{};
    shape.colour_range = Vec4{from.colour_range};
    for (std::size_t i = 0; i < 6; ++i) {
        shape.corners.at(i) = Vec4{from.leaf.offsets.at(i), 0.0F};
    }
    for (std::size_t i = 0; i < 3; ++i) {
        shape.uvs.at(i) = Vec4{from.leaf.uvs.at(i * 2), from.leaf.uvs.at((i * 2) + 1)};
        shape.indices.at(i) =
            glm::uvec4{from.leaf.indices.at(i * 4), from.leaf.indices.at((i * 4) + 1),
                       from.leaf.indices.at((i * 4) + 2), from.leaf.indices.at((i * 4) + 3)};
    }
    auto leaf_shape = or_error(make_filled_buffer(*device_, std::as_bytes(std::span{&shape, 1}),
                                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
                               "leaf template");
    if (!leaf_shape) {
        return std::unexpected(std::move(leaf_shape).error());
    }
    species.leaf_shape = std::move(leaf_shape).value();
    const std::size_t count = std::min<std::size_t>(most_variants, from.variants.size());
    for (const flora::Variant& variant : std::span{from.variants}.first(count)) {
        auto uploaded = upload_variant(species, from, variant);
        if (!uploaded) {
            return std::unexpected(std::move(uploaded).error());
        }
        species.variants.push_back(std::move(uploaded).value());
    }
    species_.push_back(std::move(species));
    return {};
}

void Trees::push_air(VkCommandBuffer commands, const Pipeline& pipeline, const TreeView& view,
                     std::uint32_t frame) const {
    push_uniform_buffer(commands, pipeline, 0, view.air.air);
    push_uniform_buffer(commands, pipeline, 1, view.air.lighting);
    push_sampled_image(commands, pipeline, 2, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, pipeline, 3, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, pipeline, 4, view.air.cloud_shadow, view.air.sampler);
    push_uniform_buffer(commands, pipeline, 5, frames_.at(frame).handle());
}

void Trees::place(VkExtent2D extent, const TreeView& view) {
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const Camera& camera = view.camera;
    const BlockView culling{.camera = camera, .sun = view.sun, .bend = view.bend};
    placed_.clear();
    for (const TreeDraw& tree : shown_) {
        if (tree.tree.species >= species_.size()) {
            continue;
        }
        const Species& species = species_[tree.tree.species];
        if (species.variants.empty()) {
            continue;
        }
        const Variant& variant = species.variants[tree.tree.variant % species.variants.size()];
        const DVec3 from_eye = tree.foot - camera.eye;
        const double distance = glm::length(from_eye);
        if (distance > near_) {
            continue;
        }
        const DVec3 up = glm::normalize(tree.foot.metres());
        const auto scale = static_cast<double>(tree.tree.scale);
        const double tall = static_cast<double>(variant.hi.y) * scale;
        const auto wide = static_cast<double>(
            std::max({-variant.lo.x, variant.hi.x, -variant.lo.z, variant.hi.z}));
        if (!in_view(culling, aspect, tree.foot.metres() + (up * (tall / 2.0)),
                     std::hypot(tall / 2.0, wide * scale))) {
            continue;
        }
        // Bent where its foot is, and rigid from there, too small to bend itself.
        const DVec3 foot = terrain::bent(view.bend, from_eye);
        const DVec3 bent_up =
            glm::normalize(terrain::bent(view.bend, from_eye + (up * 10.0)) - foot);
        const auto [x, leaning] = axes_of(bent_up, tree.tree);
        const DVec3 z = glm::cross(x, leaning);
        placed_.push_back(
            {.variant = &variant,
             .species = &species,
             .axes = {vec4(foot, pixel_metres(distance, camera.vertical_fov, extent.height)),
                      vec4(x * scale, static_cast<double>(tree.tree.tint)),
                      vec4(leaning * scale, 0.0), vec4(z * scale, 0.0)}});
    }
}

void Trees::draw_meshes(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame) {
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, meshes_.handle());
    push_air(commands, meshes_, view, frame);
    for (const Placed& placed : placed_) {
        const Species& species = *placed.species;
        const float px = placed.axes[0].w;
        for (const Part& part : placed.variant->meshes) {
            const std::uint32_t kept = part.dims == 0
                                           ? static_cast<std::uint32_t>(part.drops.size())
                                           : kept_of(part.drops, px);
            const std::uint32_t count = kept > 0 ? part.index_ends[kept - 1] : 0;
            if (count == 0) {
                continue;
            }
            const Push push{.foot_pixel = placed.axes[0],
                            .x_tint = placed.axes[1],
                            .y_translucency = Vec4{Vec3{placed.axes[2]}, part.translucency},
                            .z_cutoff = Vec4{Vec3{placed.axes[3]}, part.cutoff},
                            .lo_target = Vec4{placed.variant->lo, part.target},
                            .hi_dims = Vec4{placed.variant->hi, static_cast<float>(part.dims)},
                            .uv_calm = Vec4{part.uv_scale,
                                            part.role == flora::MeshRole::Wood ? 0.0F : 1.0F,
                                            part.bark ? 1.0F : 0.0F}};
            push_storage_buffer(commands, meshes_, 6, part.records.handle());
            push_storage_buffer(commands, meshes_, 7, part.pieces.handle());
            push_sampled_image(commands, meshes_, 8, texture(species, part.albedo),
                               sampler_.handle());
            push_uniform_buffer(commands, meshes_, 9, species.leaf_shape.handle());
            vkCmdBindIndexBuffer(commands, part.indices.handle(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdPushConstants(commands, meshes_.layout(), both, 0, sizeof(Push), &push);
            vkCmdDrawIndexed(commands, count, 1, 0, 0, 0);
            drawn_.triangles += count / 3;
        }
    }
}

void Trees::draw_leaves(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame) {
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, leaves_.handle());
    push_air(commands, leaves_, view, frame);
    for (const Placed& placed : placed_) {
        const Species& species = *placed.species;
        if (!placed.variant->leaves.has_value()) {
            continue;
        }
        const Leaves& leaves = placed.variant->leaves.value();
        const std::uint32_t kept = kept_of(leaves.drops, placed.axes[0].w);
        if (kept == 0) {
            continue;
        }
        const Push push{.foot_pixel = placed.axes[0],
                        .x_tint = placed.axes[1],
                        .y_translucency = Vec4{Vec3{placed.axes[2]}, leaves.translucency},
                        .z_cutoff = Vec4{Vec3{placed.axes[3]}, leaves.cutoff},
                        .lo_target = Vec4{placed.variant->lo, leaves.target},
                        .hi_dims = Vec4{placed.variant->hi, leaf_dims},
                        .uv_calm = Vec4{1.0F, 1.0F, 1.0F, 0.0F}};
        push_storage_buffer(commands, leaves_, 6, leaves.records.handle());
        push_storage_buffer(commands, leaves_, 7, leaves.records.handle());
        push_sampled_image(commands, leaves_, 8, texture(species, leaves.albedo),
                           sampler_.handle());
        push_uniform_buffer(commands, leaves_, 9, species.leaf_shape.handle());
        vkCmdPushConstants(commands, leaves_.layout(), both, 0, sizeof(Push), &push);
        vkCmdDraw(commands, 12, kept, 0, 0);
        drawn_.leaves += kept;
    }
}

void Trees::show(std::vector<TreeDraw> trees) {
    shown_ = std::move(trees);
    shown_changed_ = true;
}

Result<> Trees::build_instances() {
    shown_changed_ = false;
    runs_.clear();
    if (instances_.size() > 0) {
        retiring_.emplace_back(draws_, std::move(instances_));
        instances_ = {};
    }
    if (shown_.empty()) {
        return {};
    }
    origin_ = shown_.front().foot;
    // By species and impostor, each run drawn at once.
    std::vector<std::pair<std::pair<std::uint32_t, std::uint32_t>, Instance>> keyed;
    keyed.reserve(shown_.size());
    for (const TreeDraw& tree : shown_) {
        if (tree.tree.species >= species_.size()) {
            continue;
        }
        const Species& species = species_[tree.tree.species];
        std::uint32_t impostors = 0;
        while (impostors < species.variants.size() && species.variants[impostors].impostor) {
            ++impostors;
        }
        if (impostors == 0) {
            continue;
        }
        const auto [x, y] = axes_of(glm::normalize(tree.foot.metres()), tree.tree);
        const auto scale = static_cast<double>(tree.tree.scale);
        keyed.push_back(
            {{tree.tree.species, tree.tree.variant % impostors},
             {.foot_tint = vec4(tree.foot - origin_, static_cast<double>(tree.tree.tint)),
              .x_hash = vec4(x * scale, static_cast<double>(tree.tree.season) * 0x1p-32),
              .y = vec4(y * scale, 0.0)}});
    }
    std::ranges::stable_sort(keyed, {}, [](const auto& entry) { return entry.first; });
    std::vector<Instance> instances;
    instances.reserve(keyed.size());
    for (const auto& [key, instance] : keyed) {
        if (runs_.empty() || runs_.back().species != key.first ||
            runs_.back().variant != key.second) {
            runs_.push_back({.species = key.first,
                             .variant = key.second,
                             .first = static_cast<std::uint32_t>(instances.size()),
                             .count = 0});
        }
        ++runs_.back().count;
        instances.push_back(instance);
    }
    if (instances.empty()) {
        return {};
    }
    auto buffer = make_filled_buffer(*device_, std::as_bytes(std::span{instances}),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!buffer) {
        return std::unexpected(std::move(buffer).error());
    }
    instances_ = std::move(buffer).value();
    return {};
}

void Trees::draw_impostors(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame) {
    if (runs_.empty()) {
        return;
    }
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, impostors_.handle());
    push_air(commands, impostors_, view, frame);
    push_storage_buffer(commands, impostors_, 10, instances_.handle());
    const DVec3 origin = origin_ - view.camera.eye;
    for (const Run& run : runs_) {
        const Species& species = species_.at(run.species);
        const Impostor& impostor = species.variants.at(run.variant).impostor.value();
        for (std::uint32_t i = 0; i < 5; ++i) {
            push_sampled_image(commands, impostors_, 11 + i,
                               texture(species, impostor.textures.at(i)), atlas_sampler_.handle());
        }
        const FarPush push{.origin_first = vec4(origin, static_cast<double>(run.first)),
                           .up_height = vec4(view.bend.up, view.bend.eye_height),
                           .radii_reach = Vec4{static_cast<float>(view.bend.radius),
                                               static_cast<float>(view.bend.faked),
                                               static_cast<float>(near_), static_cast<float>(far_)},
                           .centre_radius = Vec4{impostor.centre, impostor.radius},
                           .views_cutoff =
                               Vec4{static_cast<float>(impostor.views), 0.5F, 0.0F, 0.0F}};
        vkCmdPushConstants(commands, impostors_.layout(), both, 0, sizeof(FarPush), &push);
        vkCmdDraw(commands, 6, run.count, 0, 0);
        drawn_.impostors += run.count;
    }
}

Result<> Trees::draw(VkCommandBuffer commands, VkExtent2D extent, const TreeView& view,
                     std::uint32_t frame) {
    ++draws_;
    std::erase_if(retiring_,
                  [&](const auto& retired) { return draws_ - retired.first > frames_.size(); });
    drawn_ = {};
    placed_.clear();
    if (shown_changed_) {
        if (Result<> built = build_instances(); !built) {
            return built;
        }
    }
    if (extent.width == 0 || extent.height == 0 || shown_.empty() || species_.empty()) {
        return {};
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing trees without the air");
    place(extent, view);
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const Frame constants{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
                          .sun = Vec4{Vec3{view.sun}, 0.0F}};
    std::memcpy(frames_.at(frame).mapped().data(), &constants, sizeof(Frame));
    if (Result<> flushed = frames_.at(frame).flush(); !flushed) {
        return flushed;
    }
    draw_meshes(commands, view, frame);
    draw_leaves(commands, view, frame);
    draw_impostors(commands, view, frame);
    drawn_.trees = placed_.size();
    return {};
}

} // namespace hog::render
