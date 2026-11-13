#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/blocks.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/terrain/block_mesh.hpp>
#include <hog/terrain/material.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

// The tiles vertices may name: plain orange, white, red, green and blue, then a grey one grained.
enum class Tile : std::uint8_t { Orange, White, Red, Green, Blue, Grained };

const std::vector<hog::terrain::MaterialImage>& tiles() {
    static const std::vector<hog::terrain::MaterialImage> made = [] {
        std::vector<hog::terrain::MaterialImage> images;
        for (const hog::DVec3 colour :
             {hog::DVec3{1.0, 0.5, 0.0}, hog::DVec3{1.0}, hog::DVec3{1.0, 0.0, 0.0},
              hog::DVec3{0.0, 1.0, 0.0}, hog::DVec3{0.0, 0.0, 1.0}}) {
            images.push_back(hog::terrain::material_image({.colour = colour}, 64));
        }
        images.push_back(
            hog::terrain::material_image({.colour = hog::DVec3{0.5}, .grain = 0.6, .seed = 3}, 64));
        return images;
    }();
    return made;
}

// A square two metres across facing +z, `away` metres down -z from the eye, wearing `tile`; or
// `left` on its left and `right` on its right, blending across its middle.
void add_square(BlockMesh& mesh, double away, Tile left, Tile right) {
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const hog::Vec2 corner : {hog::Vec2{-1.0F, -1.0F}, hog::Vec2{1.0F, -1.0F},
                                   hog::Vec2{1.0F, 1.0F}, hog::Vec2{-1.0F, 1.0F}}) {
        const auto tile = static_cast<std::uint8_t>(corner.x < 0.0F ? left : right);
        mesh.vertices.push_back({.position = hog::Vec3{corner, static_cast<float>(-away)},
                                 .normal = hog::Vec3{0.0F, 0.0F, 1.0F},
                                 .tiles = {tile, tile, tile},
                                 .corner = 0});
    }
    for (const std::uint32_t corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
        mesh.indices.push_back(first + corner);
    }
    hog::terrain::mix_corners(mesh);
}

void add_square(BlockMesh& mesh, double away, Tile tile) {
    add_square(mesh, away, tile, tile);
}

// Draws the blocks over a black background, looking down -z from `eye` on a planet a thousand
// kilometres across whose ground is `up` there, and reads it back.
std::vector<std::uint8_t> render_view(hog::render::Device& device, hog::render::Blocks& blocks,
                                      const BlockView& view);

std::vector<std::uint8_t> render(hog::render::Device& device, hog::render::Blocks& blocks,
                                 DVec3 sun, WorldPosition eye = {}, DVec3 up = {0.0, 0.0, 1.0},
                                 double cutaway = 0.0) {
    return render_view(
        device, blocks,
        {.camera = Camera{.eye = eye, .forward = {0.0, 0.0, -1.0}, .up = {0.0, 1.0, 0.0}},
         .sun = sun,
         .bend = {.up = up, .eye_height = 0.0, .radius = 1.0e6, .faked = 1.0e6},
         .cutaway = cutaway});
}

// Draws the blocks as `view` sees them over a black background, and reads it back.
std::vector<std::uint8_t> render_view(hog::render::Device& device, hog::render::Blocks& blocks,
                                      const BlockView& view) {
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
                               WorldPosition eye = {}, DVec3 up = {0.0, 0.0, 1.0},
                               double cutaway = 0.0) {
    auto blocks = hog::render::Blocks::create(device, VK_FORMAT_R8G8B8A8_UNORM,
                                              VK_FORMAT_D32_SFLOAT, tiles());
    REQUIRE(blocks);
    blocks->show(std::make_shared<const BlockMesh>(std::move(mesh)));
    return render(device, *blocks, sun, eye, up, cutaway);
}

Pixel at(const std::vector<std::uint8_t>& pixels, std::size_t x, std::size_t y) {
    const std::size_t first = 4 * ((y * extent.width) + x);
    return {pixels.at(first), pixels.at(first + 1), pixels.at(first + 2), pixels.at(first + 3)};
}

Pixel middle(const std::vector<std::uint8_t>& pixels) {
    return at(pixels, extent.width / 2, extent.height / 2);
}

// The columns along the middle row where something is drawn over the black background.
std::pair<std::size_t, std::size_t> drawn_columns(const std::vector<std::uint8_t>& pixels) {
    std::size_t first = extent.width;
    std::size_t last = 0;
    for (std::size_t x = 0; x < extent.width; ++x) {
        const Pixel pixel = at(pixels, x, extent.height / 2);
        if (pixel[0] > 0 || pixel[1] > 0 || pixel[2] > 0) {
            first = std::min(first, x);
            last = std::max(last, x);
        }
    }
    REQUIRE(first + 16 < last);
    return {first, last};
}

} // namespace

TEST_CASE("a side facing the sun shows in its colour", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
    add_square(mesh, 4.0, Tile::Orange);
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
    add_square(mesh, 4.0, Tile::Orange);
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
    add_square(mesh, 4.0, Tile::White);
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
    for (const bool near_first : {true, false}) {
        BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
        add_square(mesh, near_first ? 3.0 : 6.0, near_first ? Tile::Red : Tile::Green);
        add_square(mesh, near_first ? 6.0 : 3.0, near_first ? Tile::Green : Tile::Red);
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
    add_square(mesh, 4.0, Tile::White);
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
    auto blocks = hog::render::Blocks::create(*device, VK_FORMAT_R8G8B8A8_UNORM,
                                              VK_FORMAT_D32_SFLOAT, tiles());
    REQUIRE(blocks);
    // Two squares a planet's radius apart, each where its own origin is.
    const WorldPosition here{6.4e6, 0.0, 0.0};
    const WorldPosition there{0.0, 6.4e6, 0.0};
    for (const auto& [key, origin, tile] :
         {std::tuple{1U, here, Tile::Red}, std::tuple{2U, there, Tile::Green}}) {
        BlockMesh mesh{.origin = origin, .vertices = {}, .indices = {}};
        add_square(mesh, 4.0, tile);
        blocks->place(key, std::make_shared<const BlockMesh>(std::move(mesh)));
    }
    CHECK(blocks->placed() == 2);
    CHECK(middle(render(*device, *blocks, DVec3{0.0, 0.0, 1.0}, here))[0] > 240);
    CHECK(middle(render(*device, *blocks, DVec3{0.0, 0.0, 1.0}, there))[1] > 240);
    // Placed again, it's replaced; taken away, it's gone.
    BlockMesh blue{.origin = here, .vertices = {}, .indices = {}};
    add_square(blue, 4.0, Tile::Blue);
    blocks->place(1, std::make_shared<const BlockMesh>(std::move(blue)));
    CHECK(blocks->placed() == 2);
    const Pixel replaced = middle(render(*device, *blocks, DVec3{0.0, 0.0, 1.0}, here));
    CHECK(replaced[0] < 10);
    CHECK(replaced[2] > 240);
    blocks->remove(1);
    CHECK(blocks->placed() == 1);
    CHECK(middle(render(*device, *blocks, DVec3{0.0, 0.0, 1.0}, here))[2] == 0);
}

TEST_CASE("a triangle wearing two tiles blends from one to the other", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
    add_square(mesh, 4.0, Tile::Red, Tile::Green);
    const std::vector<std::uint8_t> pixels = draw(*device, mesh, DVec3{0.0, 0.0, 1.0});
    const std::size_t row = extent.height / 2;
    const auto [first, last] = drawn_columns(pixels);
    // Tile::Red at the left edge, green at the right, and both part way across.
    const Pixel left = at(pixels, first + 1, row);
    const Pixel mixed = middle(pixels);
    const Pixel right = at(pixels, last - 1, row);
    CHECK(left[0] > 200);
    CHECK(left[1] < 60);
    CHECK(mixed[0] > 60);
    CHECK(mixed[0] < 200);
    CHECK(mixed[1] > 60);
    CHECK(mixed[1] < 200);
    CHECK(right[0] < 60);
    CHECK(right[1] > 200);
}

TEST_CASE("wet ground is drawn darker than dry, in its own hue", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const auto shade = [&](double wet) {
        BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
        add_square(mesh, 2.5, Tile::Orange);
        for (hog::terrain::BlockVertex& vertex : mesh.vertices) {
            vertex.corner =
                hog::terrain::packed_corner(hog::terrain::corner_of(vertex.corner), wet);
        }
        return middle(draw(*device, mesh, DVec3{0.0, 0.0, 1.0}));
    };
    const Pixel dry = shade(0.0);
    const Pixel wet = shade(1.0);
    INFO("dry " << int{dry[0]} << ", " << int{dry[1]} << ", " << int{dry[2]} << "; wet "
                << int{wet[0]} << ", " << int{wet[1]} << ", " << int{wet[2]});
    REQUIRE(dry[0] > 100);
    CHECK(wet[0] < 0.65 * dry[0]);
    CHECK(wet[0] > 0.4 * dry[0]);
    const double darker = static_cast<double>(wet[0]) / dry[0];
    CHECK(wet[1] > (darker - 0.1) * dry[1]);
    CHECK(wet[1] < (darker + 0.1) * dry[1]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a grained tile shows its grain, a plain one none", "[render][gpu]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const auto spread = [&](Tile tile) {
        BlockMesh mesh{.origin = WorldPosition{}, .vertices = {}, .indices = {}};
        add_square(mesh, 2.5, tile);
        const std::vector<std::uint8_t> pixels = draw(*device, mesh, DVec3{0.0, 0.0, 1.0});
        const auto [first, last] = drawn_columns(pixels);
        std::uint8_t least = 255;
        std::uint8_t most = 0;
        for (std::size_t x = first + 1; x < last; ++x) {
            const std::uint8_t red = at(pixels, x, extent.height / 2)[0];
            least = std::min(least, red);
            most = std::max(most, red);
        }
        return most - least;
    };
    CHECK(spread(Tile::White) <= 2);
    CHECK(spread(Tile::Grained) > 15);
}

TEST_CASE("ground inside the cutaway isn't drawn, and a lamp lights what's left",
          "[render][blocks]") {
    auto device = test_device();
    if (!device) {
        SKIP("no device to draw with");
    }
    const auto square = [] {
        BlockMesh mesh;
        add_square(mesh, 5.0, Tile::White);
        return mesh;
    };
    // The sun on the far side of the square, so only the sky and the lamp light it.
    const DVec3 sun{0.0, 0.0, -1.0};
    const Pixel plain = middle(draw(*device, square(), sun));
    CHECK(plain[0] > 0);
    // Past the cutaway it's drawn, and brighter for the lamp.
    const Pixel lit = middle(draw(*device, square(), sun, {}, {0.0, 0.0, 1.0}, 3.0));
    CHECK(lit[0] > plain[0]);
    // Within most of it, gone: the background shows.
    const Pixel gone = middle(draw(*device, square(), sun, {}, {0.0, 0.0, 1.0}, 7.0));
    CHECK(gone == Pixel{0, 0, 0, 255});
}

TEST_CASE("water standing on level ground mirrors what the probe took round the eye",
          "[render][blocks][gpu]") {
    const auto device = test_device();
    // A wide flat floor a metre under the eye, under a film of water, looked across low.
    BlockMesh floor;
    for (const hog::Vec2 corner : {hog::Vec2{-40.0F, 1.0F}, hog::Vec2{40.0F, 1.0F},
                                   hog::Vec2{40.0F, 80.0F}, hog::Vec2{-40.0F, 80.0F}}) {
        floor.vertices.push_back({.position = hog::Vec3{corner, -1.0F},
                                  .normal = hog::Vec3{0.0F, 0.0F, 1.0F},
                                  .tiles = {3, 3, 3},
                                  .corner = hog::terrain::packed_corner(0, 1.0, 0.0, true)});
    }
    floor.indices = {0, 1, 2, 0, 2, 3};
    hog::terrain::mix_corners(floor);
    auto blocks = hog::render::Blocks::create(*device, VK_FORMAT_R8G8B8A8_UNORM,
                                              VK_FORMAT_D32_SFLOAT, tiles());
    REQUIRE(blocks);
    blocks->show(std::make_shared<const BlockMesh>(std::move(floor)));
    // All round the eye, red.
    std::array<std::byte, std::size_t{4} * 6> red{};
    for (std::size_t face = 0; face < 6; ++face) {
        red.at(4 * face) = std::byte{255};
        red.at((4 * face) + 3) = std::byte{255};
    }
    auto around = hog::render::make_filled_image(
        *device, {.format = VK_FORMAT_R8G8B8A8_UNORM, .layers = 6, .cube = true}, red);
    REQUIRE(around);
    BlockView view{
        .camera = Camera{.eye = {},
                         .forward = glm::normalize(DVec3{0.0, 1.0, -0.12}),
                         .up = {0.0, 0.0, 1.0}},
        .sun = glm::normalize(DVec3{0.0, -1.0, 1.0}),
        .bend = {.up = {0.0, 0.0, 1.0}, .eye_height = 1.0, .radius = 1.0e6, .faked = 1.0e6}};
    const Pixel plain = middle(render_view(*device, *blocks, view));
    view.environment = around->view();
    const Pixel mirrored = middle(render_view(*device, *blocks, view));
    CAPTURE(plain, mirrored);
    // Without a probe it mirrors a plain sky; with one, what it holds.
    CHECK(plain[2] > plain[0]);
    CHECK(mirrored[0] > mirrored[2]);
    CHECK(mirrored[0] > plain[0] + 40);
    CHECK(device->validation_errors() == 0);
}
