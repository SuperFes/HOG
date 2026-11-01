#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/blocks.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/terrain/blocks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::render::BlockView;
using hog::render::Camera;
using hog::render::testing::test_device;
using hog::terrain::BlockMesh;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr VkExtent2D extent{.width = 64, .height = 64};

// A square two metres across facing +z, `away` metres down -z from the eye, in `colour`.
void add_square(BlockMesh& mesh, double away, hog::Vec3 colour) {
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const hog::Vec2 corner : {hog::Vec2{-1.0F, -1.0F}, hog::Vec2{1.0F, -1.0F},
                                   hog::Vec2{1.0F, 1.0F}, hog::Vec2{-1.0F, 1.0F}}) {
        mesh.vertices.push_back({.position = hog::Vec3{corner, static_cast<float>(-away)},
                                 .normal = hog::Vec3{0.0F, 0.0F, 1.0F},
                                 .colour = colour});
    }
    for (const std::uint32_t corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
        mesh.indices.push_back(first + corner);
    }
}

// Draws the blocks over a black background, looking down -z from `eye` on a planet a thousand
// kilometres across whose ground is `up` there, and reads it back.
std::vector<std::uint8_t> render(hog::render::Device& device, hog::render::Blocks& blocks,
                                 DVec3 sun, WorldPosition eye = {}, DVec3 up = {0.0, 0.0, 1.0}) {
    auto target = hog::render::make_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(target);
    auto depth =
        hog::render::make_image(device, {.format = VK_FORMAT_D32_SFLOAT,
                                         .extent = extent,
                                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depth);
    const BlockView view{.camera =
                             Camera{.eye = eye, .forward = {0.0, 0.0, -1.0}, .up = {0.0, 1.0, 0.0}},
                         .sun = sun,
                         .bend = {.up = up, .eye_height = 0.0, .radius = 1.0e6, .faked = 1.0e6}};
    bool drawn = false;
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        drawn = blocks.draw(commands, extent, view, 0).has_value();
        vkCmdEndRendering(commands);
    }));
    REQUIRE(drawn);
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

std::vector<std::uint8_t> draw(hog::render::Device& device, BlockMesh mesh, DVec3 sun,
                               WorldPosition eye = {}, DVec3 up = {0.0, 0.0, 1.0}) {
    auto blocks =
        hog::render::Blocks::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(blocks);
    blocks->show(std::make_shared<const BlockMesh>(std::move(mesh)));
    return render(device, *blocks, sun, eye, up);
}

Pixel middle(const std::vector<std::uint8_t>& pixels) {
    const std::size_t first =
        4 * (((std::size_t{extent.height} / 2) * extent.width) + (extent.width / 2));
    return {pixels.at(first), pixels.at(first + 1), pixels.at(first + 2), pixels.at(first + 3)};
}

} // namespace

TEST_CASE("a side facing the sun shows in its colour", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
    add_square(mesh, 4.0, hog::Vec3{1.0F, 0.5F, 0.0F});
    const Pixel lit = middle(draw(*device, mesh, DVec3{0.0, 0.0, 1.0}));
    CHECK(lit[0] > 240);
    CHECK(lit[1] > 110);
    CHECK(lit[1] < 145);
    CHECK(lit[2] < 10);

    // Away from the sun, only as bright as the night leaves it.
    const Pixel dark = middle(draw(*device, mesh, DVec3{0.0, 0.0, -1.0}));
    CHECK(dark[0] > 50);
    CHECK(dark[0] < 80);
}

TEST_CASE("a side facing the sun once it has set is lit by the sky alone", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
    add_square(mesh, 4.0, hog::Vec3{1.0F, 0.5F, 0.0F});
    // Standing with the square's side facing the sun, a little over or under the horizon.
    const DVec3 up{0.0, 1.0, 0.0};
    const Pixel risen =
        middle(draw(*device, mesh, glm::normalize(DVec3{0.0, 0.1, 1.0}), WorldPosition{}, up));
    const Pixel set =
        middle(draw(*device, mesh, glm::normalize(DVec3{0.0, -0.1, 1.0}), WorldPosition{}, up));
    // Side on to the sky, it takes less of the sky's light than looking up, sun or none.
    CHECK(risen[0] > 220);
    // Side on to the sky, it sees less of it than looking up.
    CHECK(set[0] > 30);
    CHECK(set[0] < 45);
}

TEST_CASE("with the sun down, sides show by how much of the sky they see", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
    add_square(mesh, 4.0, hog::Vec3{1.0F, 1.0F, 1.0F});
    // The sun straight under foot, wherever up is.
    const auto seen_with_up = [&](DVec3 up) {
        return middle(draw(*device, mesh, -up, WorldPosition{}, up))[0];
    };
    const std::uint8_t looking_up = seen_with_up({0.0, 0.0, 1.0});
    const std::uint8_t tilted = seen_with_up(glm::normalize(DVec3{0.0, 1.0, 1.0}));
    const std::uint8_t side_on = seen_with_up({0.0, 1.0, 0.0});
    const std::uint8_t looking_down = seen_with_up({0.0, 0.0, -1.0});
    CHECK(looking_up > tilted);
    CHECK(tilted > side_on);
    CHECK(side_on > looking_down);
    CHECK(looking_down > 5);
}

TEST_CASE("nearer blocks hide farther ones, whichever comes first", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const hog::Vec3 red{1.0F, 0.0F, 0.0F};
    const hog::Vec3 green{0.0F, 1.0F, 0.0F};
    for (const bool near_first : {true, false}) {
        BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
        add_square(mesh, near_first ? 3.0 : 6.0, near_first ? red : green);
        add_square(mesh, near_first ? 6.0 : 3.0, near_first ? green : red);
        const Pixel seen = middle(draw(*device, mesh, DVec3{0.0, 0.0, 1.0}));
        CHECK(seen[0] > 240);
        CHECK(seen[1] < 10);
    }
}

TEST_CASE("blocks are drawn from their origin, wherever the eye is", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // As far from the planet's centre as its surface, where floats alone would blur the square
    // away.
    const WorldPosition far{6.4e6, 0.0, 0.0};
    BlockMesh mesh{.origin = far, .vertices = {}, .indices = {}};
    add_square(mesh, 4.0, hog::Vec3{1.0F});
    CHECK(middle(draw(*device, mesh, DVec3{0.0, 0.0, 1.0}, far))[0] > 240);
    // Seen from elsewhere, it's not there.
    CHECK(middle(draw(*device, mesh, DVec3{0.0, 0.0, 1.0}))[0] == 0);
}

TEST_CASE("meshes placed apart are drawn each from its own origin, until taken away",
          "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    auto blocks =
        hog::render::Blocks::create(*device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(blocks);
    // Two squares a planet's radius apart, each where its own origin is.
    const WorldPosition here{6.4e6, 0.0, 0.0};
    const WorldPosition there{0.0, 6.4e6, 0.0};
    for (const auto& [key, at, colour] : {std::tuple{1U, here, hog::Vec3{1.0F, 0.0F, 0.0F}},
                                          std::tuple{2U, there, hog::Vec3{0.0F, 1.0F, 0.0F}}}) {
        BlockMesh mesh{.origin = at, .vertices = {}, .indices = {}};
        add_square(mesh, 4.0, colour);
        blocks->place(key, std::make_shared<const BlockMesh>(std::move(mesh)));
    }
    CHECK(blocks->placed() == 2);
    CHECK(middle(render(*device, *blocks, DVec3{0.0, 0.0, 1.0}, here))[0] > 240);
    CHECK(middle(render(*device, *blocks, DVec3{0.0, 0.0, 1.0}, there))[1] > 240);
    // Placed again, it's replaced; taken away, it's gone.
    BlockMesh blue{.origin = here, .vertices = {}, .indices = {}};
    add_square(blue, 4.0, hog::Vec3{0.0F, 0.0F, 1.0F});
    blocks->place(1, std::make_shared<const BlockMesh>(std::move(blue)));
    CHECK(blocks->placed() == 2);
    const Pixel replaced = middle(render(*device, *blocks, DVec3{0.0, 0.0, 1.0}, here));
    CHECK(replaced[0] < 10);
    CHECK(replaced[2] > 240);
    blocks->remove(1);
    CHECK(blocks->placed() == 1);
    CHECK(middle(render(*device, *blocks, DVec3{0.0, 0.0, 1.0}, here))[2] == 0);
}
