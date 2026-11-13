#include <hog/render/blocks.hpp>
#include <hog/render/device.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/shaders/blocks.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 origin; // w: where the tiles' coordinates start, x
    glm::vec4 up_height;
    glm::vec4 sun_radius;
    glm::vec4 faked_night; // zw: where the tiles' coordinates start, y and z
};

// The night's light, under one, and the cutaway's whole metres over it, in the one float the push
// has room for.
float night_and_cutaway(double night, double cutaway) {
    const double metres = std::floor(std::clamp(cutaway, 0.0, block_cutaway_most));
    return static_cast<float>(metres + std::clamp(night, 0.0, 0.999));
}

// Within what every device can push.
static_assert(sizeof(Push) <= 128);

// Far tiles the tiles' coordinates run over before they repeat: near enough to keep their
// precision in floats, far enough that the repetition is never seen.
constexpr double far_tiles_held = 64.0;
constexpr double period = block_far_tile * far_tiles_held;

constexpr std::array descriptors{
    VkDescriptorSetLayoutBinding{.binding = 0,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 .descriptorCount = 1,
                                 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                 .pImmutableSamplers = nullptr},
    VkDescriptorSetLayoutBinding{.binding = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 .descriptorCount = 1,
                                 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                 .pImmutableSamplers = nullptr}};

constexpr std::array vertex_bindings{
    VkVertexInputBindingDescription{.binding = 0,
                                    .stride = sizeof(terrain::BlockVertex),
                                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};

constexpr std::array vertex_attributes{
    VkVertexInputAttributeDescription{.location = 0,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(terrain::BlockVertex, position)},
    VkVertexInputAttributeDescription{.location = 1,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(terrain::BlockVertex, normal)},
    VkVertexInputAttributeDescription{.location = 2,
                                      .binding = 0,
                                      .format = VK_FORMAT_R8G8B8A8_UINT,
                                      .offset = offsetof(terrain::BlockVertex, tiles)}};

static_assert(offsetof(terrain::BlockVertex, corner) == offsetof(terrain::BlockVertex, tiles) + 3,
              "the corner rides in the tiles' fourth byte");

// Each mip of every tile in turn, the tiles one after another.
Result<std::vector<std::byte>> tile_texels(std::span<const terrain::MaterialImage> tiles) {
    if (tiles.empty() || tiles.size() > 256) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("{} tiles; a vertex names one of up to 256", tiles.size()));
    }
    const terrain::MaterialImage& first = tiles.front();
    for (const terrain::MaterialImage& tile : tiles) {
        if (tile.size != first.size || tile.levels.size() != first.levels.size() ||
            first.levels.empty()) {
            return make_error(ErrorCode::InvalidArgument, "tiles of different sizes");
        }
    }
    std::vector<std::byte> texels;
    for (std::size_t level = 0; level < first.levels.size(); ++level) {
        for (const terrain::MaterialImage& tile : tiles) {
            const auto bytes = std::as_bytes(std::span{tile.levels[level]});
            texels.insert(texels.end(), bytes.begin(), bytes.end());
        }
    }
    return texels;
}

// Upload memory for `bytes`, kept if `kept` holds as many already.
Result<> hold(const Device& device, Buffer& kept, std::span<const std::byte> bytes,
              VkBufferUsageFlags usage) {
    if (kept.size() < bytes.size()) {
        auto made = make_buffer(device, bytes.size(), usage, Memory::Upload);
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        kept = std::move(made).value();
    }
    std::memcpy(kept.mapped().data(), bytes.data(), bytes.size());
    return kept.flush();
}

} // namespace

Result<Blocks> Blocks::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                              std::span<const terrain::MaterialImage> tiles, std::uint32_t frames) {
    Blocks blocks;
    blocks.device_ = &device;
    blocks.copies_.resize(frames);
    auto pipeline = make_pipeline(device, {.spirv = shaders::blocks,
                                           .bindings = vertex_bindings,
                                           .attributes = vertex_attributes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    blocks.pipeline_ = std::move(pipeline).value();
    auto texels = tile_texels(tiles);
    if (!texels) {
        return std::unexpected(std::move(texels).error());
    }
    const terrain::MaterialImage& first = tiles.front();
    auto image = make_filled_image(device,
                                   {.format = VK_FORMAT_R8G8B8A8_SRGB,
                                    .extent = {.width = static_cast<std::uint32_t>(first.size),
                                               .height = static_cast<std::uint32_t>(first.size)},
                                    .layers = static_cast<std::uint32_t>(tiles.size()),
                                    .levels = static_cast<std::uint32_t>(first.levels.size())},
                                   *texels);
    if (!image) {
        return std::unexpected(std::move(image).error());
    }
    blocks.tiles_ = std::move(image).value();
    auto sampler = make_tiling_sampler(device);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    blocks.sampler_ = std::move(sampler).value();
    constexpr std::array<std::byte, std::size_t{4} * 6> nothing{};
    auto empty = make_filled_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM, .layers = 6, .cube = true}, nothing);
    if (!empty) {
        return std::unexpected(std::move(empty).error());
    }
    blocks.nothing_ = std::move(empty).value();
    auto mirror_sampler = make_blurring_sampler(device);
    if (!mirror_sampler) {
        return std::unexpected(std::move(mirror_sampler).error());
    }
    blocks.mirror_sampler_ = std::move(mirror_sampler).value();
    return blocks;
}

void Blocks::show(std::shared_ptr<const terrain::BlockMesh> mesh) {
    mesh_ = std::move(mesh);
    ++shown_;
}

Result<> Blocks::copy(Copy& copy) const {
    copy.count = 0;
    copy.shown = shown_;
    if (!mesh_ || mesh_->indices.empty()) {
        return {};
    }
    if (auto held = hold(*device_, copy.vertices, std::as_bytes(std::span{mesh_->vertices}),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        !held) {
        return held;
    }
    if (auto held = hold(*device_, copy.indices, std::as_bytes(std::span{mesh_->indices}),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        !held) {
        return held;
    }
    copy.count = static_cast<std::uint32_t>(mesh_->indices.size());
    copy.origin = mesh_->origin;
    return {};
}

void Blocks::place(std::uint64_t key, std::shared_ptr<const terrain::BlockMesh> mesh) {
    remove(key);
    if (mesh && !mesh->indices.empty()) {
        Placed placed;
        placed.mesh = std::move(mesh);
        placed_.emplace(key, std::move(placed));
    }
}

Result<> Blocks::upload(Placed& placed) const {
    const terrain::BlockMesh& mesh = *placed.mesh;
    auto vertices = make_filled_buffer(*device_, std::as_bytes(std::span{mesh.vertices}),
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vertices) {
        return std::unexpected(std::move(vertices).error());
    }
    auto indices = make_filled_buffer(*device_, std::as_bytes(std::span{mesh.indices}),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!indices) {
        return std::unexpected(std::move(indices).error());
    }
    placed.vertices = std::move(vertices).value();
    placed.indices = std::move(indices).value();
    placed.count = static_cast<std::uint32_t>(mesh.indices.size());
    placed.origin = mesh.origin;
    DVec3 lowest{std::numeric_limits<double>::max()};
    DVec3 highest{std::numeric_limits<double>::lowest()};
    for (const terrain::BlockVertex& vertex : mesh.vertices) {
        lowest = glm::min(lowest, DVec3{vertex.position});
        highest = glm::max(highest, DVec3{vertex.position});
    }
    if (!mesh.vertices.empty()) {
        placed.middle = 0.5 * (lowest + highest);
        placed.radius = 0.5 * glm::length(highest - lowest);
    }
    placed.mesh.reset();
    return {};
}

void Blocks::remove(std::uint64_t key) {
    if (const auto held = placed_.find(key); held != placed_.end()) {
        retire(std::move(held->second));
        placed_.erase(held);
    }
}

void Blocks::retire(Placed placed) {
    retiring_.emplace_back(draws_, std::move(placed));
}

void Blocks::draw_one(VkCommandBuffer commands, const Buffer& vertices, const Buffer& indices,
                      std::uint32_t count, WorldPosition origin, const BlockView& view,
                      double aspect) const {
    // The tiles' coordinates are the world's, wrapped where they repeat, so a mesh placed
    // anywhere wears them as its neighbours do.
    const Vec3 tiled{glm::mod(origin.metres(), DVec3{period})};
    const Push push{
        .clip_from_eye = Mat4{view_projection(view.camera, aspect)},
        .origin = glm::vec4{Vec3{origin - view.camera.eye}, tiled.x},
        .up_height = glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
        .sun_radius = glm::vec4{Vec3{view.sun}, static_cast<float>(view.bend.radius)},
        .faked_night = glm::vec4{static_cast<float>(view.bend.faked),
                                 night_and_cutaway(view.night, view.cutaway), tiled.y, tiled.z}};
    const std::array buffers{vertices.handle()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, buffers.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                       &push);
    vkCmdDrawIndexed(commands, count, 1, 0, 0, 0);
}

Result<> Blocks::draw(VkCommandBuffer commands, VkExtent2D extent, const BlockView& view,
                      std::uint32_t frame) {
    if (Result<> prepared = prepare(frame); !prepared) {
        return prepared;
    }
    draw_prepared(commands, extent, view, frame);
    return {};
}

Result<> Blocks::prepare(std::uint32_t frame) {
    ++draws_;
    // Each frame in flight has been drawn again since: none can still be reading them.
    std::erase_if(retiring_,
                  [&](const auto& retired) { return draws_ - retired.first > copies_.size(); });
    Copy& kept = copies_.at(frame);
    if (kept.shown != shown_) {
        if (auto copied = copy(kept); !copied) {
            return copied;
        }
    }
    for (auto& [key, placed] : placed_) {
        if (placed.mesh) {
            if (auto uploaded = upload(placed); !uploaded) {
                return uploaded;
            }
        }
    }
    return {};
}

void Blocks::draw_prepared(VkCommandBuffer commands, VkExtent2D extent, const BlockView& view,
                           std::uint32_t frame) const {
    const Copy& kept = copies_.at(frame);
    if (extent.width == 0 || extent.height == 0 || (kept.count == 0 && placed_.empty())) {
        return;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_sampled_image(commands, pipeline_, 0, tiles_.view(), sampler_.handle());
    push_sampled_image(commands, pipeline_, 1,
                       view.environment != VK_NULL_HANDLE ? view.environment : nothing_.view(),
                       mirror_sampler_.handle());
    if (kept.count > 0) {
        draw_one(commands, kept.vertices, kept.indices, kept.count, kept.origin, view, aspect);
    }
    for (const auto& [key, placed] : placed_) {
        if (seen(view.camera, aspect, placed)) {
            draw_one(commands, placed.vertices, placed.indices, placed.count, placed.origin, view,
                     aspect);
        }
    }
}

bool Blocks::seen(const Camera& camera, double aspect, const Placed& placed) {
    // The ball about the mesh against the view's four sides and what's behind it; generously, as
    // the bend draws far ground lower than it lies.
    const DVec3 to = (placed.origin.metres() + placed.middle) - camera.eye.metres();
    const double away = glm::length(to);
    const double radius = placed.radius + (0.1 * away);
    const DVec3 forward = glm::normalize(camera.forward);
    const DVec3 right = glm::normalize(glm::cross(forward, camera.up));
    const DVec3 up = glm::cross(right, forward);
    const double half_high = std::tan(camera.vertical_fov / 2.0);
    const double half_wide = half_high * aspect;
    const double ahead = glm::dot(to, forward);
    if (ahead < -radius) {
        return false;
    }
    // Each side's plane through the eye, its normal pointing out of the view.
    const auto outside = [&](DVec3 across, double half) {
        const DVec3 normal = glm::normalize(across - (forward * half));
        return glm::dot(to, normal) > radius;
    };
    return !outside(right, half_wide) && !outside(-right, half_wide) && !outside(up, half_high) &&
           !outside(-up, half_high);
}

} // namespace hog::render
