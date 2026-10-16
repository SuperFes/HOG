#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/gpu_timer.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/render_graph.hpp>
#include <hog/shaders/fill.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using hog::Result;
using hog::render::GraphImage;
using hog::render::GraphPass;
using hog::render::GraphTransition;
using hog::render::ImageUse;
using hog::render::plan_graph;
using hog::render::RenderGraph;
using hog::render::TrackedImage;
using hog::render::TransientImages;
using hog::render::testing::test_device;
namespace use = hog::render::image_use;

namespace {

constexpr ImageUse fragment_read{.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
constexpr ImageUse compute_read{.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

constexpr GraphImage colour{.index = 0};
constexpr GraphImage depth{.index = 1};
constexpr GraphImage table{.index = 2};

constexpr VkClearColorValue black{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}};

GraphPass drawing(std::string_view name, bool clear, bool with_depth = true) {
    GraphPass pass{.name = name,
                   .uses = {},
                   .targets = {{.colour = {.image = colour,
                                           .clear = clear ? std::optional{black} : std::nullopt},
                                .depth = std::nullopt}}};
    if (with_depth) {
        pass.targets->depth = {.image = depth, .clear = clear ? std::optional{0.0F} : std::nullopt};
    }
    return pass;
}

// Images starting as a frame's would: nothing kept in the targets, the table just written.
const std::array<ImageUse, 3> starting{use::discard, use::discard, use::storage};

} // namespace

TEST_CASE("an image's first use in a graph waits on what it was imported in",
          "[render][render_graph]") {
    const std::array passes{drawing("sky", true)};
    const auto plan = plan_graph(starting, passes);
    REQUIRE(plan);
    REQUIRE(plan->passes.size() == 1);
    const auto& transitions = plan->passes[0].transitions;
    REQUIRE(transitions.size() == 2);
    CHECK(transitions[0].image == colour);
    CHECK(transitions[0].from == use::discard);
    CHECK(transitions[0].to == use::colour_target);
    CHECK(transitions[0].barrier());
    CHECK(transitions[1].image == depth);
    CHECK(transitions[1].to == use::depth_target);
    CHECK_FALSE(plan->passes[0].continues_rendering);
    CHECK(plan->last_uses[0] == use::colour_target);
    CHECK(plan->last_uses[1] == use::depth_target);
    CHECK(plan->last_uses[2] == use::storage);
}

TEST_CASE("reads in a graph are gathered, and a later write waits on them all, as one write does "
          "on another",
          "[render][render_graph]") {
    const std::array passes{
        GraphPass{.name = "sky", .uses = {{.image = table, .use = fragment_read}}, .targets = {}},
        GraphPass{.name = "again", .uses = {{.image = table, .use = fragment_read}}, .targets = {}},
        GraphPass{.name = "air", .uses = {{.image = table, .use = compute_read}}, .targets = {}},
        GraphPass{.name = "tables", .uses = {{.image = table, .use = use::storage}}, .targets = {}},
        GraphPass{.name = "more", .uses = {{.image = table, .use = use::storage}}, .targets = {}}};
    const auto plan = plan_graph(starting, passes);
    REQUIRE(plan);
    REQUIRE(plan->passes[0].transitions.size() == 1);
    CHECK(plan->passes[0].transitions[0].barrier());
    CHECK(plan->passes[0].transitions[0].from == use::storage);
    CHECK(plan->passes[1].transitions.empty());
    REQUIRE(plan->passes[2].transitions.size() == 1);
    const GraphTransition& gathered = plan->passes[2].transitions[0];
    CHECK_FALSE(gathered.barrier());
    CHECK(gathered.to.stage ==
          (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
    REQUIRE(plan->passes[3].transitions.size() == 1);
    const GraphTransition& written = plan->passes[3].transitions[0];
    CHECK(written.barrier());
    CHECK(written.from == gathered.to);
    CHECK(written.to == use::storage);
    // Writing after a write waits on it, though the use is the same.
    REQUIRE(plan->passes[4].transitions.size() == 1);
    CHECK(plan->passes[4].transitions[0].barrier());
}

TEST_CASE("passes drawing into the same targets carry on in the same rendering",
          "[render][render_graph]") {
    GraphPass sampling = drawing("sampling", false);
    sampling.uses = {{.image = table, .use = fragment_read}};
    GraphPass glare = drawing("glare", false, false);
    glare.uses = {{.image = depth, .use = use::sampled}};
    const std::array passes{drawing("sky", true),   drawing("ground", false), sampling,
                            drawing("sea", false),  drawing("cleared", true), glare,
                            drawing("after", false)};
    const auto plan = plan_graph(starting, passes);
    REQUIRE(plan);
    const auto& planned = plan->passes;
    CHECK_FALSE(planned[0].continues_rendering);
    CHECK(planned[1].continues_rendering);
    CHECK(planned[1].transitions.empty());
    // The table it samples needs a barrier, so it starts its own rendering.
    CHECK_FALSE(planned[2].continues_rendering);
    CHECK(planned[2].transitions.size() == 3);
    CHECK(planned[3].continues_rendering);
    CHECK_FALSE(planned[4].continues_rendering);
    // Sampling what was a target means drawing without it.
    CHECK_FALSE(planned[5].continues_rendering);
    REQUIRE_FALSE(planned[5].transitions.empty());
    CHECK(planned[5].transitions[0].image == depth);
    CHECK(planned[5].transitions[0].to == use::sampled);
    // After a pass with other targets.
    CHECK_FALSE(planned[6].continues_rendering);
    CHECK(plan->last_uses[1] == use::depth_target);
}

TEST_CASE("a rendering isn't carried on past a pass drawing nothing", "[render][render_graph]") {
    const std::array passes{drawing("sky", true),
                            GraphPass{.name = "nothing", .uses = {}, .targets = {}},
                            drawing("ground", false)};
    const auto plan = plan_graph(starting, passes);
    REQUIRE(plan);
    CHECK_FALSE(plan->passes[2].continues_rendering);
}

TEST_CASE("a graph refuses an image not in it, or used twice in a pass", "[render][render_graph]") {
    const std::array missing{GraphPass{
        .name = "lost", .uses = {{.image = {.index = 3}, .use = use::sampled}}, .targets = {}}};
    const auto lost = plan_graph(starting, missing);
    REQUIRE_FALSE(lost);
    CHECK(lost.error().code == hog::ErrorCode::InvalidArgument);
    GraphPass both = drawing("both", true);
    both.uses = {{.image = depth, .use = use::sampled}};
    CHECK_FALSE(plan_graph(starting, std::array{both}));
    const std::array twice{GraphPass{
        .name = "twice",
        .uses = {{.image = table, .use = use::sampled}, {.image = table, .use = use::storage}},
        .targets = {}}};
    CHECK_FALSE(plan_graph(starting, twice));
}

TEST_CASE("a graph refuses targets of different sizes before recording anything",
          "[render][render_graph]") {
    TrackedImage first{VK_NULL_HANDLE, VK_IMAGE_ASPECT_COLOR_BIT};
    TrackedImage second{VK_NULL_HANDLE, VK_IMAGE_ASPECT_DEPTH_BIT};
    RenderGraph graph;
    const GraphImage drawn = graph.import(first, VK_NULL_HANDLE, {.width = 4, .height = 4});
    const GraphImage tested = graph.import(second, VK_NULL_HANDLE, {.width = 2, .height = 4});
    bool recorded = false;
    graph.add(
        {.name = "drawn",
         .uses = {},
         .targets = {{.colour = {.image = drawn, .clear = black}, .depth = {{.image = tested}}}}},
        [&](VkCommandBuffer) { recorded = true; });
    const auto executed = graph.execute(VK_NULL_HANDLE);
    REQUIRE_FALSE(executed);
    CHECK(executed.error().code == hog::ErrorCode::InvalidArgument);
    CHECK_FALSE(recorded);
    CHECK(first.use() == use::discard);
}

TEST_CASE("a graph making images refuses to without somewhere to keep them",
          "[render][render_graph]") {
    RenderGraph graph;
    const GraphImage made = graph.transient({.format = VK_FORMAT_R8G8B8A8_UNORM,
                                             .extent = {.width = 4, .height = 4},
                                             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT});
    bool recorded = false;
    graph.add({.name = "made",
               .uses = {},
               .targets = {{.colour = {.image = made, .clear = black}, .depth = std::nullopt}}},
              [&](VkCommandBuffer) { recorded = true; });
    const auto executed = graph.execute(VK_NULL_HANDLE);
    REQUIRE_FALSE(executed);
    CHECK(executed.error().code == hog::ErrorCode::InvalidArgument);
    CHECK_FALSE(recorded);
}

namespace {

constexpr VkExtent2D extent{.width = 4, .height = 4};

constexpr hog::render::ImageDesc depth_desc{.format = VK_FORMAT_D32_SFLOAT,
                                            .extent = extent,
                                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                                            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                                            .layers = 1,
                                            .levels = 1};

struct Fill {
    std::array<float, 4> colour{};
    float depth = 0.5F;
};

struct Targets {
    explicit Targets(hog::render::Device& device) {
        auto made_colour =
            hog::render::make_image(device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                             .extent = extent,
                                             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
        REQUIRE(made_colour);
        colour = std::move(made_colour).value();
        auto made_depth = hog::render::make_image(device, depth_desc);
        REQUIRE(made_depth);
        depth = std::move(made_depth).value();
        auto made = hog::render::make_pipeline(device, {.spirv = hog::shaders::fill,
                                                        .depth_format = VK_FORMAT_D32_SFLOAT,
                                                        .push_constant_bytes = sizeof(Fill)});
        REQUIRE(made);
        fill = std::move(made).value();
        tracked_colour = {colour.handle(), VK_IMAGE_ASPECT_COLOR_BIT};
        tracked_depth = {depth.handle(), VK_IMAGE_ASPECT_DEPTH_BIT};
    }

    void draw(VkCommandBuffer commands, const Fill& with) const {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, fill.handle());
        vkCmdPushConstants(commands, fill.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(Fill), &with);
        vkCmdDraw(commands, 3, 1, 0, 0);
    }

    // The frame: a far red cleared into, a near green drawn after, then a pass sampling depth,
    // which is made by the graph where it's transient.
    void add_to(RenderGraph& graph, bool transient = false) {
        const GraphImage drawn = graph.import(tracked_colour, colour.view(), extent);
        const GraphImage tested = transient ? graph.transient(depth_desc)
                                            : graph.import(tracked_depth, depth.view(), extent);
        graph.add({.name = "far",
                   .uses = {},
                   .targets = {{.colour = {.image = drawn, .clear = black},
                                .depth = {{.image = tested}}}}},
                  [this](VkCommandBuffer commands) {
                      draw(commands, {.colour = {1.0F, 0.0F, 0.0F, 1.0F}, .depth = 0.2F});
                  });
        graph.add({.name = "near",
                   .uses = {},
                   .targets = {{.colour = {.image = drawn, .clear = std::nullopt},
                                .depth = {{.image = tested, .clear = std::nullopt}}}}},
                  [this](VkCommandBuffer commands) {
                      draw(commands, {.colour = {0.0F, 1.0F, 0.0F, 1.0F}, .depth = 0.8F});
                  });
        graph.add({.name = "sampled",
                   .uses = {{.image = tested, .use = use::sampled}},
                   .targets = {{.colour = {.image = drawn, .clear = std::nullopt},
                                .depth = std::nullopt}}},
                  [](VkCommandBuffer) {});
    }

    hog::render::Image colour;
    hog::render::Image depth;
    TrackedImage tracked_colour;
    TrackedImage tracked_depth;
    hog::render::Pipeline fill;
};

using Pixel = std::array<std::uint8_t, 4>;

Pixel middle(hog::render::Device& device, const Targets& targets) {
    const auto pixels =
        hog::render::read_pixels(device, targets.colour, targets.tracked_colour.use());
    REQUIRE(pixels);
    const std::size_t at = std::size_t{4} * ((std::size_t{2} * extent.width) + 2);
    return {pixels->at(at), pixels->at(at + 1), pixels->at(at + 2), pixels->at(at + 3)};
}

} // namespace

TEST_CASE("a graph's passes draw in order through the barriers it works out",
          "[render][gpu][render_graph]") {
    const auto device = test_device();
    Targets targets{*device};
    RenderGraph graph;
    targets.add_to(graph);
    Result<> executed;
    REQUIRE(
        device->submit_now([&](VkCommandBuffer commands) { executed = graph.execute(commands); }));
    REQUIRE(executed);
    CHECK(targets.tracked_colour.use() == use::colour_target);
    CHECK(targets.tracked_depth.use() == use::sampled);
    CHECK(middle(*device, targets) == Pixel{0, 255, 0, 255});
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a graph stops at a pass that fails, ending its rendering",
          "[render][gpu][render_graph]") {
    const auto device = test_device();
    Targets targets{*device};
    RenderGraph graph;
    const GraphImage drawn = graph.import(targets.tracked_colour, targets.colour.view(), extent);
    bool after = false;
    graph.add({.name = "failing",
               .uses = {},
               .targets = {{.colour = {.image = drawn, .clear = black}, .depth = std::nullopt}}},
              [](VkCommandBuffer) -> Result<> {
                  return hog::make_error(hog::ErrorCode::Internal, "failed");
              });
    graph.add({.name = "after", .uses = {}, .targets = {}}, [&](VkCommandBuffer) { after = true; });
    Result<> executed;
    REQUIRE(
        device->submit_now([&](VkCommandBuffer commands) { executed = graph.execute(commands); }));
    REQUIRE_FALSE(executed);
    CHECK(executed.error().message == "failed");
    CHECK_FALSE(after);
    CHECK(targets.tracked_colour.use() == use::colour_target);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a graph times each of its passes", "[render][gpu][render_graph]") {
    const auto device = test_device();
    auto timer = hog::render::GpuTimer::create(*device, 1);
    REQUIRE(timer);
    if (!timer->timing()) {
        SKIP("the Vulkan device's queue can't time");
    }
    Targets targets{*device};
    std::vector<std::string_view> names;
    for (int frame = 0; frame < 2; ++frame) {
        RenderGraph graph;
        targets.add_to(graph);
        Result<> executed;
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            names.clear();
            for (const hog::render::GpuSpan& span : timer->read(commands, 0)) {
                names.push_back(span.name);
            }
            executed =
                graph.execute(commands, {.slot = 0, .timer = &*timer, .transients = nullptr});
        }));
        REQUIRE(executed);
    }
    CHECK(names == std::vector<std::string_view>{"far", "near", "sampled"});
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a graph draws with the images it makes", "[render][gpu][render_graph]") {
    const auto device = test_device();
    Targets targets{*device};
    TransientImages transients{*device, 1};
    for (int frame = 0; frame < 2; ++frame) {
        targets.tracked_colour.used_as(use::discard);
        RenderGraph graph;
        targets.add_to(graph, true);
        Result<> executed;
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            executed =
                graph.execute(commands, {.slot = 0, .timer = nullptr, .transients = &transients});
        }));
        REQUIRE(executed);
        CHECK(middle(*device, targets) == Pixel{0, 255, 0, 255});
        CHECK(transients.held(0) == 1);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("transient images are kept across frames, one set per frame in flight",
          "[render][gpu][render_graph]") {
    const auto device = test_device();
    TransientImages transients{*device, 2};
    constexpr hog::render::ImageDesc colour_desc{.format = VK_FORMAT_R8G8B8A8_UNORM,
                                                 .extent = extent,
                                                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                 .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                 .layers = 1,
                                                 .levels = 1};
    const std::array both{depth_desc, colour_desc};
    const auto first = transients.take(0, both);
    REQUIRE(first);
    REQUIRE(first->size() == 2);
    CHECK(first->at(0)->desc() == depth_desc);
    CHECK(first->at(1)->desc() == colour_desc);
    auto* const depth_image = first->at(0)->handle();
    auto* const colour_image = first->at(1)->handle();

    const auto other = transients.take(1, both);
    REQUIRE(other);
    CHECK(other->at(0)->handle() != depth_image);

    // Asked for in another order, the same images.
    const std::array swapped{colour_desc, depth_desc};
    const auto again = transients.take(0, swapped);
    REQUIRE(again);
    CHECK(again->at(0)->handle() == colour_image);
    CHECK(again->at(1)->handle() == depth_image);

    // Two the same are two images.
    const std::array twins{colour_desc, colour_desc};
    const auto two = transients.take(0, twins);
    REQUIRE(two);
    CHECK(two->at(0)->handle() == colour_image);
    CHECK(two->at(1)->handle() != colour_image);
    CHECK(transients.held(0) == 2);

    // What's no longer wanted is let go.
    hog::render::ImageDesc larger = colour_desc;
    larger.extent = {.width = 8, .height = 8};
    const auto changed = transients.take(0, std::array{larger});
    REQUIRE(changed);
    CHECK(changed->at(0)->desc() == larger);
    CHECK(transients.held(0) == 1);
    CHECK(transients.take(0, {}));
    CHECK(transients.held(0) == 0);
    CHECK(transients.held(1) == 2);
}
