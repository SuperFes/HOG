#include <hog/core/assert.hpp>
#include <hog/flora/mesh.hpp>
#include <hog/flora/skeleton.hpp>
#include <hog/flora/species.hpp>
#include <hog/render/device.hpp>
#include <hog/render/forest.hpp>
#include <hog/shaders/forest.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

// The shaders read the baked trees by these layouts.
static_assert(sizeof(flora::WoodVertex) == 24);
static_assert(sizeof(flora::LeafVertex) == 36);
static_assert(offsetof(flora::LeafVertex, card) == 24);
static_assert(offsetof(flora::LeafVertex, seed) == 32);

constexpr std::uint32_t variant_count = 8;

// The ages each variant is baked at, sapling to old, the prime among them; a tree takes the stage
// nearest its own age, scaled to its own height.
constexpr std::array stage_ages{0.15F, 0.42F, flora::prime_age, 0.93F};
constexpr std::size_t stage_count = stage_ages.size();
constexpr std::size_t build_count = std::size_t{forest_details} * stage_count * variant_count;

constexpr VkDeviceSize tile_bytes = VkDeviceSize{tile_trees} * sizeof(TreeAnchor);
// A whole tile is aligned for any device's storage binding.
static_assert(tile_bytes % 256 == 0);

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 bend_up;
    glm::vec4 bend_radii;
    glm::vec4 wind;
    glm::vec4 origin;
};

static_assert(sizeof(Push) <= 128);

struct Build {
    glm::uvec4 wood;
    glm::uvec4 leaf;
};

struct Settings {
    glm::vec4 bands;
    glm::vec4 stages; // the ages between one baked stage and the next
    std::array<Build, build_count> builds;
};

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type,
                                               VkShaderStageFlags stages) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

constexpr std::array descriptors{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT),
    binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT),
    binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)};

// How much each baked detail keeps, finest first. Only the trunk is ever wood: each twig's solid
// frond runs its own spine, so a tube under it would never be seen. Dropped fronds are covered by
// widening the kept ones a little, not doubling them: each tree sheds detail by its own distance.
constexpr std::array<flora::Detail, forest_details> details{
    flora::Detail{
        .ring_stride = 1, .sides = 1.0F, .frond_stride = 1, .frond_scale = 1.0F, .wood_levels = 1},
    flora::Detail{
        .ring_stride = 2, .sides = 0.75F, .frond_stride = 2, .frond_scale = 1.4F, .wood_levels = 1},
    flora::Detail{
        .ring_stride = 3, .sides = 0.5F, .frond_stride = 3, .frond_scale = 1.9F, .wood_levels = 1}};

void append(std::vector<std::byte>& blob, const void* bytes, std::size_t count) {
    const std::span<const std::byte> source{static_cast<const std::byte*>(bytes), count};
    blob.insert(blob.end(), source.begin(), source.end());
}

// Every variant at every stage and detail, in one buffer's bytes, with the builds table.
struct Carpentry {
    std::vector<std::byte> blob;
    std::array<Build, build_count> builds{};
    std::array<std::uint32_t, forest_details> most_wood{};
    std::array<std::uint32_t, forest_details> most_leaves{};
};

// Bakes one grown tree at every detail, as the builds table's `grown_as` entry at each.
void bake_variant(Carpentry& carpentry, const flora::Skeleton& skeleton, std::size_t grown_as) {
    float height = 0.0F;
    for (const flora::Bough& bough : skeleton.boughs) {
        for (const flora::Ring& ring : bough.rings) {
            height = std::max(height, ring.centre.z);
        }
    }
    for (int detail = 0; detail < forest_details; ++detail) {
        const flora::TreeMesh grown =
            flora::mesh(skeleton, details[static_cast<std::size_t>(detail)]);
        Build& build = carpentry.builds.at(
            (static_cast<std::size_t>(detail) * stage_count * variant_count) + grown_as);
        build.wood.x = static_cast<std::uint32_t>(carpentry.blob.size());
        append(carpentry.blob, grown.wood.data(), grown.wood.size() * sizeof(flora::WoodVertex));
        build.wood.y = static_cast<std::uint32_t>(carpentry.blob.size());
        append(carpentry.blob, grown.wood_indices.data(),
               grown.wood_indices.size() * sizeof(std::uint32_t));
        build.wood.z = static_cast<std::uint32_t>(grown.wood_indices.size());
        build.wood.w = std::bit_cast<std::uint32_t>(height);
        build.leaf.x = static_cast<std::uint32_t>(carpentry.blob.size());
        append(carpentry.blob, grown.leaves.data(),
               grown.leaves.size() * sizeof(flora::LeafVertex));
        build.leaf.y = static_cast<std::uint32_t>(carpentry.blob.size());
        append(carpentry.blob, grown.leaf_indices.data(),
               grown.leaf_indices.size() * sizeof(std::uint32_t));
        build.leaf.z = static_cast<std::uint32_t>(grown.leaf_indices.size());
        auto& wood_most = carpentry.most_wood[static_cast<std::size_t>(detail)];
        wood_most = std::max(wood_most, build.wood.z);
        auto& leaf_most = carpentry.most_leaves[static_cast<std::size_t>(detail)];
        leaf_most = std::max(leaf_most, build.leaf.z);
    }
}

// Bakes every variant at every stage and detail.
Carpentry bake() {
    Carpentry carpentry;
    const flora::Species species = flora::pine();
    for (std::size_t stage = 0; stage < stage_count; ++stage) {
        for (std::uint32_t variant = 0; variant < variant_count; ++variant) {
            bake_variant(carpentry,
                         flora::grow(flora::aged(species, stage_ages.at(stage)), 101U + variant),
                         (stage * variant_count) + variant);
        }
    }
    return carpentry;
}

} // namespace

Result<Forest> Forest::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                              double full, double mid) {
    Forest forest;
    const Carpentry carpentry = bake();
    forest.most_wood_ = carpentry.most_wood;
    forest.most_leaves_ = carpentry.most_leaves;
    Settings settings{};
    settings.bands = {static_cast<float>(full), static_cast<float>(mid),
                      static_cast<float>(forest_far), 0.0F};
    for (std::size_t stage = 0; stage + 1 < stage_count; ++stage) {
        settings.stages[static_cast<glm::length_t>(stage)] =
            0.5F * (stage_ages.at(stage) + stage_ages.at(stage + 1));
    }
    settings.builds = carpentry.builds;
    auto settings_buffer = make_filled_buffer(device, std::as_bytes(std::span{&settings, 1}),
                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!settings_buffer) {
        return std::unexpected(std::move(settings_buffer).error());
    }
    forest.settings_ = std::move(settings_buffer).value();
    auto carpentry_buffer =
        make_filled_buffer(device, std::span{carpentry.blob}, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!carpentry_buffer) {
        return std::unexpected(std::move(carpentry_buffer).error());
    }
    forest.carpentry_ = std::move(carpentry_buffer).value();

    auto wood = make_pipeline(device, {.spirv = shaders::forest,
                                       .vertex_entry = "wood_vertex",
                                       .fragment_entry = "wood_fragment",
                                       .colour_format = colour_format,
                                       .depth_format = depth_format,
                                       .cull = VK_CULL_MODE_NONE,
                                       .push_constant_bytes = sizeof(Push),
                                       .descriptors = descriptors});
    if (!wood) {
        return std::unexpected(std::move(wood).error());
    }
    forest.wood_ = std::move(wood).value();
    auto leaves = make_pipeline(device, {.spirv = shaders::forest,
                                         .vertex_entry = "leaf_vertex",
                                         .fragment_entry = "leaf_fragment",
                                         .colour_format = colour_format,
                                         .depth_format = depth_format,
                                         .cull = VK_CULL_MODE_NONE,
                                         .push_constant_bytes = sizeof(Push),
                                         .descriptors = descriptors});
    if (!leaves) {
        return std::unexpected(std::move(leaves).error());
    }
    forest.leaves_ = std::move(leaves).value();
    return forest;
}

void Forest::draw(VkCommandBuffer commands, VkExtent2D extent, const AnchorPool& pool,
                  std::span<const StandDraw> draws, const GrassView& view) const {
    if (extent.width == 0 || extent.height == 0 || draws.empty()) {
        return;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
              .bend_up = glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
              .bend_radii =
                  glm::vec4{static_cast<float>(view.bend.radius),
                            static_cast<float>(view.bend.faked), static_cast<float>(view.seconds),
                            static_cast<float>(view.camera.vertical_fov / extent.height)},
              .wind = glm::vec4{Vec3{view.wind}, 0.0F},
              .origin = {}};
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    const auto plant_all = [&](const Pipeline& pipeline,
                               const std::array<std::uint32_t, forest_details>& most) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        push_uniform_buffer(commands, pipeline, 0, settings_.handle());
        HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE,
                   "growing a forest lit by the air without it");
        push_uniform_buffer(commands, pipeline, 2, view.air.air);
        push_uniform_buffer(commands, pipeline, 3, view.air.lighting);
        push_sampled_image(commands, pipeline, 4, view.air.transmittance, view.air.sampler);
        push_sampled_image(commands, pipeline, 5, view.air.aerial, view.air.sampler);
        push_sampled_image(commands, pipeline, 6, view.air.cloud_shadow, view.air.sampler);
        push_storage_buffer(commands, pipeline, 7, carpentry_.handle());
        vkCmdPushConstants(commands, pipeline.layout(), stages, 0, offsetof(Push, wind), &push);
        for (const StandDraw& draw : draws) {
            HOG_ASSERT(draw.slot < pool.capacity(), "growing a forest on a slot beyond the pool");
            HOG_ASSERT(draw.detail >= 0 && draw.detail < forest_details,
                       "a detail beyond those baked");
            if (draw.count == 0) {
                continue;
            }
            push_storage_buffer(commands, pipeline, 1, pool.anchors(),
                                VkDeviceSize{draw.slot} * tile_bytes, tile_bytes);
            HOG_ASSERT(draw.first + draw.count <= tile_trees, "drawing past the tile's anchors");
            const std::uint32_t chosen =
                (draw.first << 2U) | static_cast<std::uint32_t>(draw.detail);
            push.origin =
                glm::vec4{relative_to(draw.origin, view.camera.eye), std::bit_cast<float>(chosen)};
            vkCmdPushConstants(commands, pipeline.layout(), stages, offsetof(Push, wind),
                               sizeof(Push) - offsetof(Push, wind), &push.wind);
            vkCmdDraw(commands, most[static_cast<std::size_t>(draw.detail)], draw.count, 0, 0);
        }
    };
    plant_all(wood_, most_wood_);
    plant_all(leaves_, most_leaves_);
}

} // namespace hog::render
