#include <hog/planet/tangent_frame.hpp>
#include <hog/render/device.hpp>
#include <hog/render/ocean.hpp>
#include <hog/shaders/ocean.hpp>
#include <hog/terrain/sea.hpp>

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace hog::render {

namespace {

using terrain::SeaVertex;

static_assert(std::is_trivially_copyable_v<SeaVertex> && std::is_standard_layout_v<SeaVertex>);

// Triangles about as long as they're wide, a few pixels across at 1080p.
constexpr int rings = 96;
constexpr int spokes = 128;

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 up_height;
    glm::vec4 east_faked;
    glm::vec4 sun_nearest;
    glm::vec4 sky_furthest;
};

// Within what every device can push.
static_assert(sizeof(Push) <= 128);

constexpr std::array vertex_bindings{VkVertexInputBindingDescription{
    .binding = 0, .stride = sizeof(SeaVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
constexpr std::array vertex_attributes{
    VkVertexInputAttributeDescription{.location = 0,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32_SFLOAT,
                                      .offset = offsetof(SeaVertex, out)},
    VkVertexInputAttributeDescription{.location = 1,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32_SFLOAT,
                                      .offset = offsetof(SeaVertex, towards)}};

} // namespace

Result<Ocean> Ocean::create(const Device& device, VkFormat colour_format, VkFormat depth_format) {
    Ocean ocean;
    const terrain::SeaDisc disc = terrain::sea_disc(rings, spokes);
    auto vertices = make_filled_buffer(device, std::as_bytes(std::span{disc.vertices}),
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vertices) {
        return std::unexpected(std::move(vertices).error());
    }
    ocean.vertices_ = std::move(vertices).value();
    auto indices = make_filled_buffer(device, std::as_bytes(std::span{disc.indices}),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!indices) {
        return std::unexpected(std::move(indices).error());
    }
    ocean.indices_ = std::move(indices).value();
    ocean.index_count_ = static_cast<std::uint32_t>(disc.indices.size());

    // Seen from below too, from under the sea.
    auto pipeline = make_pipeline(device, {.spirv = shaders::ocean,
                                           .bindings = vertex_bindings,
                                           .attributes = vertex_attributes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .blend = Blend::Premultiplied,
                                           .push_constant_bytes = sizeof(Push)});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    ocean.pipeline_ = std::move(pipeline).value();
    return ocean;
}

void Ocean::draw(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view) const {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const terrain::SeaReach reach = terrain::sea_reach(view.bend);
    const Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
                    .up_height =
                        glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
                    .east_faked = glm::vec4{Vec3{planet::tangent_frame(view.bend.up).east},
                                            static_cast<float>(view.bend.faked)},
                    .sun_nearest = glm::vec4{Vec3{view.sun}, static_cast<float>(reach.nearest)},
                    .sky_furthest = glm::vec4{view.sky, static_cast<float>(reach.furthest)}};

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    const std::array vertices{vertices_.handle()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, vertices.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices_.handle(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                       &push);
    vkCmdDrawIndexed(commands, index_count_, 1, 0, 0, 0);
}

} // namespace hog::render
