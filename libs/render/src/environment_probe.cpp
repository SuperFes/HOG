#include <hog/planet/tangent_frame.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/environment_probe.hpp>
#include <hog/render/rendering.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <numbers>
#include <utility>

namespace hog::render {

namespace {

struct Face {
    DVec3 forward;
    DVec3 up;
};

// In the cube's own axes, each face looking along its axis with its rows running down the way
// Vulkan samples them.
constexpr std::array<Face, probe_faces> faces{
    {{.forward = {1.0, 0.0, 0.0}, .up = {0.0, 1.0, 0.0}},
     {.forward = {-1.0, 0.0, 0.0}, .up = {0.0, 1.0, 0.0}},
     {.forward = {0.0, 1.0, 0.0}, .up = {0.0, 0.0, -1.0}},
     {.forward = {0.0, -1.0, 0.0}, .up = {0.0, 0.0, 1.0}},
     {.forward = {0.0, 0.0, 1.0}, .up = {0.0, 1.0, 0.0}},
     {.forward = {0.0, 0.0, -1.0}, .up = {0.0, 1.0, 0.0}}}};

constexpr VkClearColorValue black{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}};

// Moves one mip of every face from one use to another.
void at_level(VkCommandBuffer commands, VkImage image, std::uint32_t level, ImageUse from,
              ImageUse to) {
    VkImageMemoryBarrier2 barrier = image_barrier(image, VK_IMAGE_ASPECT_COLOR_BIT, from, to);
    barrier.subresourceRange.baseMipLevel = level;
    barrier.subresourceRange.levelCount = 1;
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .pNext = nullptr,
                                      .dependencyFlags = 0,
                                      .memoryBarrierCount = 0,
                                      .pMemoryBarriers = nullptr,
                                      .bufferMemoryBarrierCount = 0,
                                      .pBufferMemoryBarriers = nullptr,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commands, &dependency);
}
// The face looking down holds nothing, so what samples it falls back to its own.
constexpr VkClearColorValue nothing{.float32 = {0.0F, 0.0F, 0.0F, 0.0F}};

} // namespace

Camera probe_face(std::uint32_t face, WorldPosition eye, DVec3 up, DVec3 east) {
    const DVec3 north = glm::cross(up, east);
    const auto world = [&](DVec3 axes) {
        return (east * axes.x) + (up * axes.y) + (north * axes.z);
    };
    const Face& turned = faces.at(face);
    return {.eye = eye,
            .forward = world(turned.forward),
            .up = world(turned.up),
            .vertical_fov = std::numbers::pi / 2.0};
}

DVec3 probe_direction(DVec3 direction, DVec3 up, DVec3 east) {
    return {glm::dot(direction, east), glm::dot(direction, up),
            glm::dot(direction, glm::cross(up, east))};
}

Result<EnvironmentProbe> EnvironmentProbe::create(const Device& device, VkFormat colour_format,
                                                  VkFormat depth_format, std::uint32_t size) {
    EnvironmentProbe probe;
    probe.device_ = &device;
    probe.size_ = size;
    probe.levels_ = static_cast<std::uint32_t>(std::bit_width(size));
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device.physical(), colour_format, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) !=
        0) {
        probe.shrink_ = VK_FILTER_LINEAR;
    }
    auto colour = make_image(
        device, {.format = colour_format,
                 .extent = {.width = size, .height = size},
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 .layers = probe_faces,
                 .levels = probe.levels_,
                 .cube = true,
                 .layer_views = true});
    if (!colour) {
        return std::unexpected(std::move(colour).error());
    }
    probe.colour_ = std::move(colour).value();
    auto depth = make_image(
        device, {.format = depth_format,
                 .extent = {.width = size, .height = size},
                 .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                 .layers = probe_faces,
                 .layer_views = true});
    if (!depth) {
        return std::unexpected(std::move(depth).error());
    }
    probe.depth_ = std::move(depth).value();
    for (std::uint32_t face = 0; face < probe_faces; ++face) {
        auto volume = make_aerial_volume(device);
        if (!volume) {
            return std::unexpected(std::move(volume).error());
        }
        probe.aerial_.push_back(std::move(volume).value());
    }
    return probe;
}

Result<> EnvironmentProbe::record(VkCommandBuffer commands, std::uint32_t frame,
                                  const Atmosphere& atmosphere, const Sky& sky,
                                  const ProbeView& view) {
    const VkExtent2D extent{.width = size_, .height = size_};
    const bool whole = !std::exchange(whole_, true);
    if (!whole) {
        next_ = (next_ + 1) % probe_faces;
        next_ = next_ == probe_down ? (next_ + 1) % probe_faces : next_;
    }
    // Those cleared this time, and of them those drawn into.
    std::array<bool, probe_faces> cleared{};
    std::array<bool, probe_faces> drawn{};
    for (std::uint32_t face = 0; face < probe_faces; ++face) {
        cleared.at(face) = whole || face == next_;
        drawn.at(face) = cleared.at(face) && face != probe_down;
    }
    const DVec3 up = view.sky.bend.up;
    const DVec3 east = planet::tangent_frame(up).east;
    std::array<SkyView, probe_faces> skies;
    for (std::uint32_t face = 0; face < probe_faces; ++face) {
        skies.at(face) = view.sky;
        skies.at(face).camera = probe_face(face, view.sky.camera.eye, up, east);
    }
    record_air(commands, frame, atmosphere, sky, skies, drawn);

    // The faces not drawn are kept as they were.
    if (whole) {
        transition(commands, colour_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::discard,
                   image_use::colour_target);
    } else {
        at_level(commands, colour_.handle(), 0, image_use::sampled, image_use::colour_target);
    }
    transition(commands, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, image_use::discard,
               image_use::depth_target);
    for (std::uint32_t face = 0; face < probe_faces; ++face) {
        if (!cleared.at(face)) {
            continue;
        }
        if (!drawn.at(face)) {
            begin_rendering(commands, colour_.layer_view(face), extent, nothing,
                            {.view = depth_.layer_view(face)});
            vkCmdEndRendering(commands);
            continue;
        }
        if (Result<> made =
                record_face(commands, frame, face, atmosphere, sky, skies.at(face), view);
            !made) {
            return made;
        }
    }
    shrink(commands);
    return {};
}

Result<> EnvironmentProbe::record_face(VkCommandBuffer commands, std::uint32_t frame,
                                       std::uint32_t face, const Atmosphere& atmosphere,
                                       const Sky& sky, const SkyView& seen, const ProbeView& view) {
    const VkExtent2D extent{.width = size_, .height = size_};
    begin_rendering(commands, colour_.layer_view(face), extent, black,
                    {.view = depth_.layer_view(face)});
    sky.draw(commands, extent, frame, atmosphere, seen, aerial_.at(face).view());
    if (view.around) {
        if (Result<> stood = view.around(commands, seen.camera, extent); !stood) {
            vkCmdEndRendering(commands);
            return stood;
        }
    }
    vkCmdEndRendering(commands);
    if (view.clouds != nullptr) {
        return record_clouds(commands, face, view, seen.camera);
    }
    return {};
}

Result<> EnvironmentProbe::record_clouds(VkCommandBuffer commands, std::uint32_t face,
                                         const ProbeView& view, const Camera& camera) {
    const VkExtent2D extent{.width = size_, .height = size_};
    const VkExtent2D marching = view.clouds->march_extent(extent);
    if (marched_.desc().extent.width != marching.width ||
        marched_.desc().extent.height != marching.height) {
        for (auto [image, format] : {std::pair{&marched_, Clouds::marched_format},
                                     std::pair{&distances_, Clouds::distances_format}}) {
            auto made = make_image(
                *device_, {.format = format,
                           .extent = marching,
                           .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
            if (!made) {
                return std::unexpected(std::move(made).error());
            }
            *image = std::move(made).value();
        }
    }
    CloudsView seen = view.clouds_view;
    seen.camera = camera;
    seen.frame_eye = false;
    seen.air.aerial = aerial_.at(face).view();
    VkImageView depth = depth_.layer_view(face);
    transition(commands, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, image_use::depth_target,
               image_use::sampled);
    for (const Image* image : {&marched_, &distances_}) {
        transition(commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::discard,
                   image_use::storage);
    }
    view.clouds->march_once(commands, extent, depth, marched_.view(), distances_.view(), seen);
    for (const Image* image : {&marched_, &distances_}) {
        transition(commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::storage,
                   image_use::sampled);
    }
    begin_rendering(commands, colour_.layer_view(face), extent, std::nullopt);
    view.clouds->draw_marched(commands, extent, depth, marched_.view(), distances_.view(), seen);
    vkCmdEndRendering(commands);
    transition(commands, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, image_use::sampled,
               image_use::depth_target);
    return {};
}

void EnvironmentProbe::record_air(VkCommandBuffer commands, std::uint32_t frame,
                                  const Atmosphere& atmosphere, const Sky& sky,
                                  const std::array<SkyView, probe_faces>& skies,
                                  const std::array<bool, probe_faces>& drawn) const {
    for (const Image& volume : aerial_) {
        transition(commands, volume.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::discard,
                   image_use::storage);
    }
    for (std::uint32_t face = 0; face < probe_faces; ++face) {
        if (drawn.at(face)) {
            sky.record_aerial(commands, frame, atmosphere, skies.at(face), 1.0,
                              aerial_.at(face).view());
        }
    }
    for (const Image& volume : aerial_) {
        transition(commands, volume.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::storage,
                   image_use::sampled);
    }
}

void EnvironmentProbe::shrink(VkCommandBuffer commands) const {
    // Each mip blitted from the one before, all six faces at once; what each held goes.
    at_level(commands, colour_.handle(), 0, image_use::colour_target, image_use::copy_source);
    for (std::uint32_t level = 1; level < levels_; ++level) {
        const auto from = static_cast<std::int32_t>(std::max(size_ >> (level - 1), 1U));
        const auto to = static_cast<std::int32_t>(std::max(size_ >> level, 1U));
        at_level(commands, colour_.handle(), level, image_use::discard, image_use::copy_target);
        const VkImageBlit blit{.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                  .mipLevel = level - 1,
                                                  .baseArrayLayer = 0,
                                                  .layerCount = probe_faces},
                               .srcOffsets = {VkOffset3D{.x = 0, .y = 0, .z = 0},
                                              VkOffset3D{.x = from, .y = from, .z = 1}},
                               .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                  .mipLevel = level,
                                                  .baseArrayLayer = 0,
                                                  .layerCount = probe_faces},
                               .dstOffsets = {VkOffset3D{.x = 0, .y = 0, .z = 0},
                                              VkOffset3D{.x = to, .y = to, .z = 1}}};
        vkCmdBlitImage(commands, colour_.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colour_.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, shrink_);
        at_level(commands, colour_.handle(), level, image_use::copy_target, image_use::copy_source);
    }
    transition(commands, colour_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::copy_source,
               image_use::sampled);
}

} // namespace hog::render
