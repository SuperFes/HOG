#pragma once

#include <hog/core/result.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/vulkan.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace hog::render {

class GpuTimer;

// An image of a graph, by when it was added.
struct GraphImage {
    std::uint32_t index = 0;

    friend bool operator==(GraphImage, GraphImage) = default;
};

// A pass's use of an image other than drawing into it.
struct GraphUse {
    GraphImage image;
    ImageUse use;
};

struct ColourTarget {
    GraphImage image;
    std::optional<VkClearColorValue> clear; // without it, the contents are kept
};

struct DepthTarget {
    GraphImage image;
    // Depth is reversed, so 0 is as far as can be. Without it, the contents are kept.
    std::optional<float> clear = 0.0F;
};

// What a pass draws into, the same size.
struct GraphTargets {
    ColourTarget colour;
    std::optional<DepthTarget> depth;
};

struct GraphPass {
    std::string_view name; // must outlive the timer timing it
    std::vector<GraphUse> uses;
    std::optional<GraphTargets> targets;
};

// An image moving from one use to the next. Reads in the same layout are gathered into one use, so
// the next write waits on them all; those moves need no barrier.
struct GraphTransition {
    GraphImage image;
    ImageUse from;
    ImageUse to;

    [[nodiscard]] bool barrier() const noexcept { return needs_barrier(from, to); }
};

struct PlannedPass {
    std::vector<GraphTransition> transitions; // before it
    // Draws into the same targets as the pass before, clearing neither and needing no barrier,
    // so it carries on in the same rendering.
    bool continues_rendering = false;
};

struct GraphPlan {
    std::vector<PlannedPass> passes;
    std::vector<ImageUse> last_uses; // of each image
};

// The transitions of passes run in order on images starting in `first_uses`. Refuses an image not
// among them, or used twice in a pass.
[[nodiscard]] Result<GraphPlan> plan_graph(std::span<const ImageUse> first_uses,
                                           std::span<const GraphPass> passes);

class Device;

// Images made for frames' graphs, kept one set per frame in flight and made again only when what's
// wanted of them changes.
class TransientImages {
public:
    TransientImages(const Device& device, std::uint32_t frames);

    // Images as described for a frame in `slot`, whose last frame must have finished. Those of its
    // last frame are kept where they're wanted the same, and let go where they're not. What's
    // returned lasts until the slot is taken again.
    [[nodiscard]] Result<std::vector<const Image*>> take(std::uint32_t slot,
                                                         std::span<const ImageDesc> wanted);

    // Held for frames in the slot.
    [[nodiscard]] std::size_t held(std::uint32_t slot) const noexcept {
        return slots_[slot].size();
    }

private:
    const Device* device_;
    std::vector<std::vector<Image>> slots_;
};

// The frame a graph is recorded for.
struct GraphFrame {
    std::uint32_t slot = 0;                // of those in flight
    GpuTimer* timer = nullptr;             // or not timed
    TransientImages* transients = nullptr; // needed where the graph makes images
};

// A frame's passes, each recorded after the barriers into the uses it declares, and timed.
class RenderGraph {
public:
    using Record = std::function<Result<>(VkCommandBuffer)>;

    // An image from outside the graph, starting in the use it's tracked in and left in its last.
    // It must outlive the graph's execution.
    GraphImage import(TrackedImage& image, VkImageView view, VkExtent2D extent);
    // An image made for the frame, its contents not kept from one to the next.
    GraphImage transient(const ImageDesc& desc);

    // Adds a pass to run after those before it, recorded by `record`, which may return nothing or
    // Result<>. Views of the graph's images are valid while it records.
    template <typename Recorder>
        requires std::invocable<Recorder&, VkCommandBuffer>
    void add(GraphPass pass, Recorder record) {
        if constexpr (std::is_void_v<std::invoke_result_t<Recorder&, VkCommandBuffer>>) {
            add_pass(std::move(pass),
                     [record = std::move(record)](VkCommandBuffer commands) mutable -> Result<> {
                         record(commands);
                         return {};
                     });
        } else {
            add_pass(std::move(pass), Record{std::move(record)});
        }
    }

    [[nodiscard]] VkImageView view(GraphImage image) const noexcept;
    [[nodiscard]] VkExtent2D extent(GraphImage image) const noexcept;

    // Makes the graph's images and records every pass, refusing a graph whose plan is refused or
    // whose targets differ in size; stops at the first pass that fails, ending any rendering begun.
    [[nodiscard]] Result<> execute(VkCommandBuffer commands, GraphFrame frame = {});

private:
    struct Image {
        TrackedImage* imported = nullptr;
        TrackedImage made; // otherwise
        VkImageView view = VK_NULL_HANDLE;
        VkExtent2D extent{};
        std::optional<ImageDesc> desc; // where made
    };

    [[nodiscard]] TrackedImage& tracked(std::size_t image) noexcept {
        return images_[image].imported != nullptr ? *images_[image].imported : images_[image].made;
    }
    [[nodiscard]] Result<> make_transients(const GraphFrame& frame);

    void add_pass(GraphPass pass, Record record);
    [[nodiscard]] Result<> check_targets() const;
    void begin_targets(VkCommandBuffer commands, const GraphTargets& targets) const;

    std::vector<Image> images_;
    std::vector<GraphPass> passes_;
    std::vector<Record> records_;
};

} // namespace hog::render
