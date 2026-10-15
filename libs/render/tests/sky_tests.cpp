#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/shaders/aerial_probe.hpp>
#include <hog/shaders/fill.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <numbers>
#include <vector>

using hog::DVec3;
using hog::render::Atmosphere;
using hog::render::Sky;
using hog::render::SkyView;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 48, .height = 27};
constexpr VkFormat colour_format = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double earth = 6'371'000.0;
constexpr double degree = std::numbers::pi / 180.0;

// Unit, `elevation` above the horizon and `azimuth` round from +X, with up +Z.
DVec3 towards(double elevation, double azimuth = 0.0) {
    return {std::cos(elevation * degree) * std::cos(azimuth * degree),
            std::cos(elevation * degree) * std::sin(azimuth * degree),
            std::sin(elevation * degree)};
}

// Seen from `height` over Earth, looking out level towards +X.
SkyView view_from(double height, DVec3 sun, DVec3 forward = {1.0, 0.0, 0.0}) {
    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 right = glm::normalize(glm::cross(forward, up));
    return {.camera = {.eye = hog::WorldPosition{up * (earth + height)},
                       .forward = forward,
                       .up = glm::cross(right, forward),
                       .vertical_fov = 1.2},
            .sun = sun,
            .bend = {.up = up, .eye_height = height, .radius = earth, .faked = earth},
            .exposure = 1.0};
}

// The view through a point of the picture, from 0 to 1 across it, as the shaders find it.
DVec3 through(const SkyView& view, hog::DVec2 uv) {
    const double aspect = static_cast<double>(extent.width) / extent.height;
    const glm::dmat4 eye_from_clip =
        glm::inverse(glm::dmat4{hog::render::view_projection(view.camera, aspect)});
    const glm::dvec4 far =
        eye_from_clip * glm::dvec4{(uv.x * 2.0) - 1.0, (uv.y * 2.0) - 1.0, 1.0, 1.0};
    return glm::normalize(DVec3{far} / far.w);
}

DVec3 through(const SkyView& view, std::uint32_t x, std::uint32_t y) {
    return through(view, hog::DVec2{(x + 0.5) / extent.width, (y + 0.5) / extent.height});
}

using Picture = std::vector<std::array<float, 4>>;

// The sky drawn into a float image, with anything else drawn after it.
Picture draw(hog::render::Device& device, const Atmosphere& atmosphere, const SkyView& view,
             const std::function<void(VkCommandBuffer)>& after = {}) {
    auto sky = Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    auto colour = hog::render::make_image(
        device, {.format = colour_format,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(colour);
    auto depth =
        hog::render::make_image(device, {.format = depth_format,
                                         .extent = extent,
                                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depth);
    const auto drawn = device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        CHECK(sky->record_frame(commands, extent, 0, atmosphere, view));
        hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, colour->view(), extent,
                                     VkClearColorValue{.float32 = {1.0F, 0.0F, 1.0F, 1.0F}},
                                     {.view = depth->view()});
        sky->draw(commands, extent, 0, atmosphere, view);
        if (after) {
            after(commands);
        }
        vkCmdEndRendering(commands);
    });
    REQUIRE(drawn);
    const auto bytes =
        hog::render::read_pixels(device, *colour, hog::render::image_use::colour_target);
    REQUIRE(bytes);
    Picture picture(std::size_t{extent.width} * extent.height);
    REQUIRE(bytes->size() == picture.size() * sizeof(picture[0]));
    std::memcpy(picture.data(), bytes->data(), bytes->size());
    return picture;
}

// The radiance a pixel was tone mapped from, at an exposure of 1.
DVec3 radiance(const std::array<float, 4>& pixel) {
    return {-std::log1p(-static_cast<double>(pixel[0])),
            -std::log1p(-static_cast<double>(pixel[1])),
            -std::log1p(-static_cast<double>(pixel[2]))};
}

const std::array<float, 4>& at(const Picture& picture, std::uint32_t x, std::uint32_t y) {
    return picture.at((std::size_t{y} * extent.width) + x);
}

} // namespace

TEST_CASE("the sky drawn matches the CPU model's", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = Atmosphere::create(*device);
    REQUIRE(atmosphere);
    const hog::atmosphere::Model model;
    const double height = GENERATE(2.0, 3'000.0);
    const SkyView view = view_from(height, towards(15.0, 40.0));
    const Picture picture = draw(*device, *atmosphere, view);
    const double scale = hog::render::air_scale(atmosphere->air(), view.bend);

    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const DVec3 direction = through(view, x, y);
            // The sun's disc is beyond the model's sky.
            if (glm::dot(direction, view.sun) > std::cos(0.01)) {
                continue;
            }
            const DVec3 expected = model.march(height * scale, direction, view.sun).light;
            const DVec3 found = radiance(at(picture, x, y));
            INFO("height " << height << ", pixel " << x << ", " << y << ": " << found.r << ", "
                           << found.g << ", " << found.b << " against " << expected.r << ", "
                           << expected.g << ", " << expected.b);
            for (glm::length_t channel = 0; channel < 3; ++channel) {
                CHECK(std::abs(found[channel] - expected[channel]) <=
                      1e-3 + (0.06 * expected[channel]));
            }
        }
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sun's disc outshines the sky round it, and reddens as it sets",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = Atmosphere::create(*device);
    REQUIRE(atmosphere);
    const std::uint32_t middle_x = extent.width / 2;
    const std::uint32_t middle_y = extent.height / 2;

    // Looking straight at it, with the picture's middle pixel on it.
    const auto looking_at = [&](double elevation) {
        SkyView view = view_from(2.0, towards(elevation));
        const DVec3 sun = view.sun;
        view.camera.forward = sun;
        view.camera.up = glm::normalize(DVec3{0.0, 0.0, 1.0} - (sun * sun.z));
        view.camera.vertical_fov = 0.01;
        view.exposure = 0.001;
        return draw(*device, *atmosphere, view);
    };
    const Picture high = looking_at(60.0);
    const Picture setting = looking_at(1.0);
    const std::array<float, 4>& disc = at(high, middle_x, middle_y);
    const std::array<float, 4>& beside = at(high, 0, 0);
    CHECK(disc[2] > 0.9F);
    CHECK(beside[2] < 0.1F * disc[2]);
    const std::array<float, 4>& low = at(setting, middle_x, middle_y);
    CHECK(radiance(low).r > 3.0 * radiance(low).b);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("from above the air, the sky is black but for the air's glow over the planet's limb",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = Atmosphere::create(*device);
    REQUIRE(atmosphere);
    const hog::atmosphere::Model model;
    // Looking down to the limb.
    const double height = 400'000.0;
    const SkyView view = view_from(height, towards(30.0), towards(-19.0));
    const Picture picture = draw(*device, *atmosphere, view);
    const double scale = hog::render::air_scale(atmosphere->air(), view.bend);
    bool glowing = false;
    bool black = false;
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const DVec3 direction = through(view, x, y);
            const DVec3 expected = model.march(height * scale, direction, view.sun).light;
            const DVec3 found = radiance(at(picture, x, y));
            glowing = glowing || found.b > 0.01;
            black = black || found == DVec3{0.0};
            INFO("pixel " << x << ", " << y << ": " << found.b << " against " << expected.b);
            CHECK(std::abs(found.b - expected.b) <= 2e-3 + (0.1 * expected.b));
        }
    }
    CHECK(glowing);
    CHECK(black);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sky stays behind what's drawn after it", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = Atmosphere::create(*device);
    REQUIRE(atmosphere);
    struct Fill {
        std::array<float, 4> colour{};
        float depth = 0.0F;
    };
    auto fill = hog::render::make_pipeline(*device, {.spirv = hog::shaders::fill,
                                                     .colour_format = colour_format,
                                                     .depth_format = depth_format,
                                                     .push_constant_bytes = sizeof(Fill)});
    REQUIRE(fill);
    // Far off, as the ground at the horizon is.
    const Fill far{.colour = {0.0F, 1.0F, 0.0F, 1.0F}, .depth = 1e-6F};
    const Picture picture =
        draw(*device, *atmosphere, view_from(2.0, towards(15.0)), [&](VkCommandBuffer commands) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, fill->handle());
            vkCmdPushConstants(commands, fill->layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(far), &far);
            vkCmdDraw(commands, 3, 1, 0, 0);
        });
    CHECK(at(picture, 0, 0) == far.colour);
    CHECK(at(picture, extent.width - 1, extent.height - 1) == far.colour);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the aerial perspective held matches the CPU model's", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = Atmosphere::create(*device);
    REQUIRE(atmosphere);
    const hog::atmosphere::Model model;
    auto sky = Sky::create(*device, colour_format, depth_format);
    REQUIRE(sky);
    const double height = 1'500.0;
    SkyView view = view_from(height, towards(10.0, 30.0));
    // Looking a little down, so some views meet the ground within reach.
    view.camera.forward = towards(-5.0);
    view.camera.up = towards(85.0);
    const double scale = hog::render::air_scale(atmosphere->air(), view.bend);

    // At texel centres across, and at the ends of slices out from the eye and between them.
    std::vector<std::array<float, 4>> asked;
    for (const double across : {3.5, 16.5, 28.5}) {
        for (const double down : {2.5, 15.5, 30.5}) {
            for (const double share : {4.0, 10.0, 15.5, 20.0, 32.0}) {
                const double distance = hog::render::aerial_reach * (share / 32.0) * (share / 32.0);
                asked.push_back({static_cast<float>(across / 32.0), static_cast<float>(down / 32.0),
                                 static_cast<float>(distance),
                                 static_cast<float>(hog::render::aerial_reach)});
            }
        }
    }
    constexpr auto binding = [](std::uint32_t at, VkDescriptorType type) {
        return VkDescriptorSetLayoutBinding{.binding = at,
                                            .descriptorType = type,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = nullptr};
    };
    const std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                              binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    auto probe = hog::render::make_compute_pipeline(
        *device, {.spirv = hog::shaders::aerial_probe, .descriptors = bindings});
    REQUIRE(probe);
    auto questions = hog::render::make_filled_buffer(*device, std::as_bytes(std::span{asked}),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    auto answers =
        hog::render::make_buffer(*device, std::span{asked}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    const hog::render::AirBindings air = sky->bindings(0, *atmosphere);
    const auto run = device->submit_now([&](VkCommandBuffer commands) {
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, view));
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        hog::render::push_sampled_image(commands, *probe, 0, air.aerial, air.sampler);
        hog::render::push_storage_buffer(commands, *probe, 1, questions->handle());
        hog::render::push_storage_buffer(commands, *probe, 2, answers->handle());
        vkCmdDispatch(commands, 1, 1, 1);
        const VkMemoryBarrier2 to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .pNext = nullptr,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                       .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &to_host,
                                          .bufferMemoryBarrierCount = 0,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(commands, &dependency);
    });
    REQUIRE(run);
    REQUIRE(answers->invalidate());
    std::vector<std::array<float, 4>> found(asked.size());
    std::memcpy(found.data(), answers->mapped().data(), std::span{found}.size_bytes());

    for (std::size_t at = 0; at < asked.size(); ++at) {
        const std::array<float, 4>& question = asked[at];
        const DVec3 direction = through(
            view, hog::DVec2{static_cast<double>(question[0]), static_cast<double>(question[1])});
        const hog::atmosphere::Scattered expected =
            model.march(height * scale, direction, view.sun, static_cast<double>(question[2]));
        const double passed =
            (expected.transmittance.r + expected.transmittance.g + expected.transmittance.b) / 3.0;
        INFO("at " << question[0] << ", " << question[1] << ", " << question[2]
                   << " m: " << found[at][0] << ", " << found[at][1] << ", " << found[at][2] << ", "
                   << found[at][3] << " against " << expected.light.r << ", " << expected.light.g
                   << ", " << expected.light.b << ", " << passed);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double light = expected.light[static_cast<glm::length_t>(channel)];
            CHECK(std::abs(static_cast<double>(found[at].at(channel)) - light) <=
                  1e-3 + (0.1 * light));
        }
        CHECK(std::abs(static_cast<double>(found[at][3]) - passed) <= 0.02);
    }
    CHECK(device->validation_errors() == 0);
}
