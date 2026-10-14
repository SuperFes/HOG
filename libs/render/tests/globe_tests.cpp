#include "test_device.hpp"

#include <hog/planet/projection.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/globe.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/orbit.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/streamed_image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using hog::render::testing::test_device;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr double radius = 1'000.0;
constexpr VkExtent2D extent{.width = 32, .height = 32};
// One per face, in face order: +x, +y, +z, -x, -y, -z.
constexpr std::array<Pixel, 6> face_colours{{{200, 0, 0, 255},
                                             {0, 200, 0, 255},
                                             {0, 0, 200, 255},
                                             {200, 200, 0, 255},
                                             {0, 200, 200, 255},
                                             {200, 0, 200, 255}}};

hog::render::StreamedImage coloured_faces(hog::render::Device& device) {
    constexpr VkExtent2D face{.width = 2, .height = 2};
    auto faces = hog::render::make_streamed_image(device, VK_FORMAT_R8G8B8A8_UNORM, face, 1, 6);
    REQUIRE(faces);
    std::vector<std::byte> texels;
    for (const Pixel& colour : face_colours) {
        for (std::uint32_t texel = 0; texel < face.width * face.height; ++texel) {
            for (const std::uint8_t channel : colour) {
                texels.push_back(static_cast<std::byte>(channel));
            }
        }
    }
    REQUIRE(device.submit_now(
        [&](VkCommandBuffer commands) { REQUIRE(faces->update(commands, 0, texels)); }));
    return std::move(faces).value();
}

// Heights with `steps` between texels across each face, from what's given per face and s and t.
template <typename Height>
hog::render::StreamedImage relief(hog::render::Device& device, std::uint32_t steps, Height height) {
    const VkExtent2D size{.width = steps + 1, .height = steps + 1};
    auto heights = hog::render::make_streamed_image(device, VK_FORMAT_R32_SFLOAT, size, 1, 6);
    REQUIRE(heights);
    std::vector<float> texels;
    for (int face = 0; face < 6; ++face) {
        for (std::uint32_t j = 0; j <= steps; ++j) {
            for (std::uint32_t i = 0; i <= steps; ++i) {
                texels.push_back(height(face, static_cast<float>(i) / static_cast<float>(steps),
                                        static_cast<float>(j) / static_cast<float>(steps)));
            }
        }
    }
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        REQUIRE(heights->update(commands, 0, std::as_bytes(std::span{texels})));
    }));
    return std::move(heights).value();
}

hog::render::StreamedImage level(hog::render::Device& device, float height = 0.0F) {
    return relief(device, 1, [height](int, float, float) { return height; });
}

hog::render::Globe make_globe(const hog::render::Device& device) {
    auto globe =
        hog::render::Globe::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 8);
    REQUIRE(globe);
    return std::move(globe).value();
}

// Draws the globe over a black background and reads it back.
std::vector<std::uint8_t> draw(hog::render::Device& device, const hog::render::Globe& globe,
                               const hog::render::GlobeImages& images,
                               const hog::render::GlobeView& view) {
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
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        globe.draw(commands, extent, images, view);
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t first = 4 * ((std::size_t{y} * extent.width) + x);
    return {pixels.at(first), pixels.at(first + 1), pixels.at(first + 2), pixels.at(first + 3)};
}

hog::render::GlobeView view_from(double latitude, double longitude, hog::DVec3 sun) {
    return {
        .camera = hog::render::orbit_camera(
            {.latitude = latitude, .longitude = longitude, .altitude = 2.0 * radius}, radius, 1.0),
        .radius = radius,
        .sun = sun,
        .night_light = 0.25,
        .twilight = 0.1};
}

} // namespace

TEST_CASE("the globe shows the face it's seen from, over the background", "[render][gpu]") {
    const auto device = test_device();
    const auto globe = make_globe(*device);
    const auto faces = coloured_faces(*device);
    const auto flat = level(*device);
    const hog::render::GlobeImages images{.faces = &faces.image(), .relief = &flat.image()};

    const auto from_x = draw(*device, globe, images, view_from(0.0, 0.0, {1.0, 0.0, 0.0}));
    CHECK(pixel_at(from_x, 16, 16) == face_colours[0]);
    CHECK(pixel_at(from_x, 1, 1) == Pixel{0, 0, 0, 255});
    CHECK(pixel_at(from_x, 30, 30) == Pixel{0, 0, 0, 255});

    const auto from_north = draw(*device, globe, images, view_from(1.5, 0.0, {0.0, 0.0, 1.0}));
    CHECK(pixel_at(from_north, 16, 16) == face_colours[2]);

    const auto from_minus_y = draw(*device, globe, images, view_from(0.0, -1.5, {0.0, -1.0, 0.0}));
    CHECK(pixel_at(from_minus_y, 16, 16) == face_colours[4]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the globe's night side stays dimly lit", "[render][gpu]") {
    const auto device = test_device();
    const auto globe = make_globe(*device);
    const auto faces = coloured_faces(*device);
    const auto flat = level(*device);

    const auto night = draw(*device, globe, {.faces = &faces.image(), .relief = &flat.image()},
                            view_from(0.0, 0.0, {-1.0, 0.0, 0.0}));
    CHECK(pixel_at(night, 16, 16) == Pixel{50, 0, 0, 255});
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("relief lifts the ground off the sphere", "[render][gpu]") {
    const auto device = test_device();
    const auto globe = make_globe(*device);
    const auto faces = coloured_faces(*device);
    const auto raised = level(*device, 1.0F);
    const hog::render::GlobeImages images{.faces = &faces.image(), .relief = &raised.image()};

    // From three radii out the sphere spans about two thirds of the way to the top edge, and half
    // as wide again reaches past it.
    auto view = view_from(0.0, 0.0, {1.0, 0.0, 0.0});
    CHECK(pixel_at(draw(*device, globe, images, view), 16, 2) == Pixel{0, 0, 0, 255});
    view.relief = 0.5;
    const auto lifted = draw(*device, globe, images, view);
    CHECK(pixel_at(lifted, 16, 2)[0] > 0);
    // Ground lifted evenly is lit as level ground is.
    CHECK(pixel_at(lifted, 16, 16) == face_colours[0]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("slopes facing the sun are lit more than those facing away", "[render][gpu]") {
    const auto device = test_device();
    const auto globe = make_globe(*device);
    const auto faces = coloured_faces(*device);
    // Rising along s on the +x face, so the slope faces back down s.
    const auto ramp =
        relief(*device, 8, [](int face, float s, float) { return face == 0 ? s : 0.0F; });
    const hog::render::GlobeImages images{.faces = &faces.image(), .relief = &ramp.image()};
    const hog::DVec3 uphill = glm::normalize(hog::planet::face_uv_to_xyz(0, 1.0, 0.0) -
                                             hog::planet::face_uv_to_xyz(0, 0.0, 0.0));

    const auto lit = [&](double relief, double towards) {
        auto view =
            view_from(0.0, 0.0, glm::normalize(hog::DVec3{1.0, 0.0, 0.0} + (towards * uphill)));
        view.relief = relief;
        return pixel_at(draw(*device, globe, images, view), 16, 16)[0];
    };
    const int level_ground = lit(0.0, -0.5);
    CHECK(lit(0.2, -0.5) > level_ground + 5);
    CHECK(lit(0.2, 0.5) < level_ground - 5);
    CHECK(device->validation_errors() == 0);
}
