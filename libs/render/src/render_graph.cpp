#include <hog/render/device.hpp>
#include <hog/render/gpu_timer.hpp>
#include <hog/render/render_graph.hpp>
#include <hog/render/rendering.hpp>

#include <algorithm>
#include <format>
#include <iterator>

namespace hog::render {

namespace {

// What it does with each image, its targets among them.
std::vector<GraphUse> wanted_uses(const GraphPass& pass) {
    std::vector<GraphUse> wanted = pass.uses;
    if (pass.targets) {
        wanted.push_back({.image = pass.targets->colour.image, .use = image_use::colour_target});
        if (pass.targets->depth) {
            wanted.push_back({.image = pass.targets->depth->image, .use = image_use::depth_target});
        }
    }
    return wanted;
}

Result<> check_uses(const GraphPass& pass, std::span<const GraphUse> wanted, std::size_t images) {
    for (auto use = wanted.begin(); use != wanted.end(); ++use) {
        if (use->image.index >= images) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("{} uses image {}, which isn't in the graph", pass.name,
                                          use->image.index));
        }
        if (std::any_of(wanted.begin(), use,
                        [&](const GraphUse& before) { return before.image == use->image; })) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("{} uses image {} twice", pass.name, use->image.index));
        }
    }
    return {};
}

bool same_targets(const GraphTargets& before, const GraphTargets& after) noexcept {
    const auto depth = [](const GraphTargets& targets) {
        return targets.depth ? std::optional{targets.depth->image} : std::nullopt;
    };
    return before.colour.image == after.colour.image && depth(before) == depth(after);
}

bool clears(const GraphTargets& targets) noexcept {
    return targets.colour.clear || (targets.depth && targets.depth->clear);
}

// Where no barrier is needed, the uses gathered, so what comes after waits on both.
ImageUse next_use(ImageUse from, ImageUse to) noexcept {
    if (needs_barrier(from, to)) {
        return to;
    }
    return {.stage = from.stage | to.stage, .access = from.access | to.access, .layout = to.layout};
}

bool is_target(const GraphPass& pass, GraphImage image) noexcept {
    return pass.targets && (pass.targets->colour.image == image ||
                            (pass.targets->depth && pass.targets->depth->image == image));
}

} // namespace

Result<GraphPlan> plan_graph(std::span<const ImageUse> first_uses,
                             std::span<const GraphPass> passes) {
    GraphPlan plan{.passes = {}, .last_uses = {first_uses.begin(), first_uses.end()}};
    std::vector<ImageUse>& uses = plan.last_uses;
    const GraphPass* before = nullptr;
    for (const GraphPass& pass : passes) {
        const std::vector<GraphUse> wanted = wanted_uses(pass);
        if (auto checked = check_uses(pass, wanted, uses.size()); !checked) {
            return std::unexpected(std::move(checked).error());
        }
        // Drawing into the same targets again needs no barrier between, rendering's own order
        // keeping the draws apart.
        const bool continues =
            before != nullptr && before->targets && pass.targets &&
            same_targets(*before->targets, *pass.targets) && !clears(*pass.targets) &&
            std::ranges::none_of(wanted, [&](const GraphUse& use) {
                return !is_target(pass, use.image) && needs_barrier(uses[use.image.index], use.use);
            });
        PlannedPass planned{.transitions = {}, .continues_rendering = continues};
        for (const GraphUse& use : wanted) {
            if (continues && is_target(pass, use.image)) {
                continue;
            }
            ImageUse& was = uses[use.image.index];
            const ImageUse next = next_use(was, use.use);
            if (next != was || needs_barrier(was, use.use)) {
                planned.transitions.push_back({.image = use.image, .from = was, .to = next});
                was = next;
            }
        }
        plan.passes.push_back(std::move(planned));
        before = &pass;
    }
    return plan;
}

TransientImages::TransientImages(const Device& device, std::uint32_t frames)
    : device_{&device}, slots_(frames) {}

Result<std::vector<const Image*>> TransientImages::take(std::uint32_t slot,
                                                        std::span<const ImageDesc> wanted) {
    std::vector<Image>& had = slots_.at(slot);
    std::vector<Image> kept;
    kept.reserve(wanted.size());
    for (const ImageDesc& desc : wanted) {
        // Those taken already are left without a handle.
        const auto same = std::ranges::find_if(had, [&](const Image& image) {
            return image.handle() != VK_NULL_HANDLE && image.desc() == desc;
        });
        if (same != had.end()) {
            kept.push_back(std::move(*same));
            continue;
        }
        auto made = make_image(*device_, desc);
        if (!made) {
            std::ranges::move(kept, std::back_inserter(had));
            return std::unexpected(std::move(made).error());
        }
        kept.push_back(std::move(made).value());
    }
    had = std::move(kept);
    std::vector<const Image*> images;
    images.reserve(had.size());
    for (const Image& image : had) {
        images.push_back(&image);
    }
    return images;
}

GraphImage RenderGraph::import(TrackedImage& image, VkImageView view, VkExtent2D extent) {
    images_.push_back(
        {.imported = &image, .made = {}, .view = view, .extent = extent, .desc = std::nullopt});
    return {.index = static_cast<std::uint32_t>(images_.size() - 1)};
}

GraphImage RenderGraph::transient(const ImageDesc& desc) {
    images_.push_back({.imported = nullptr,
                       .made = {},
                       .view = VK_NULL_HANDLE,
                       .extent = desc.extent,
                       .desc = desc});
    return {.index = static_cast<std::uint32_t>(images_.size() - 1)};
}

Result<> RenderGraph::make_transients(const GraphFrame& frame) {
    std::vector<ImageDesc> wanted;
    for (const Image& image : images_) {
        if (image.desc) {
            wanted.push_back(*image.desc);
        }
    }
    if (wanted.empty()) {
        return {};
    }
    if (frame.transients == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "a graph making images needs somewhere to keep them");
    }
    auto taken = frame.transients->take(frame.slot, wanted);
    if (!taken) {
        return std::unexpected(std::move(taken).error());
    }
    auto made = taken->begin();
    for (Image& image : images_) {
        if (image.desc) {
            image.made = {(*made)->handle(), image.desc->aspect};
            image.view = (*made)->view();
            ++made;
        }
    }
    return {};
}

void RenderGraph::add_pass(GraphPass pass, Record record) {
    passes_.push_back(std::move(pass));
    records_.push_back(std::move(record));
}

VkImageView RenderGraph::view(GraphImage image) const noexcept {
    return images_[image.index].view;
}

VkExtent2D RenderGraph::extent(GraphImage image) const noexcept {
    return images_[image.index].extent;
}

Result<> RenderGraph::check_targets() const {
    for (const GraphPass& pass : passes_) {
        if (!pass.targets || !pass.targets->depth) {
            continue;
        }
        const VkExtent2D colour = extent(pass.targets->colour.image);
        const VkExtent2D depth = extent(pass.targets->depth->image);
        if (colour.width != depth.width || colour.height != depth.height) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("{} draws into a {}x{} colour target and {}x{} depth",
                                          pass.name, colour.width, colour.height, depth.width,
                                          depth.height));
        }
    }
    return {};
}

Result<> RenderGraph::execute(VkCommandBuffer commands, GraphFrame frame) {
    std::vector<ImageUse> uses;
    uses.reserve(images_.size());
    for (std::size_t image = 0; image < images_.size(); ++image) {
        uses.push_back(tracked(image).use());
    }
    const auto plan = plan_graph(uses, passes_);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (auto checked = check_targets(); !checked) {
        return checked;
    }
    if (auto made = make_transients(frame); !made) {
        return made;
    }
    bool rendering = false;
    Result<> recorded;
    std::vector<VkImageMemoryBarrier2> barriers;
    for (std::size_t at = 0; at < passes_.size() && recorded; ++at) {
        const PlannedPass& planned = plan->passes[at];
        const GraphPass& pass = passes_[at];
        if (rendering && !planned.continues_rendering) {
            vkCmdEndRendering(commands);
            rendering = false;
        }
        barriers.clear();
        for (const GraphTransition& transition : planned.transitions) {
            const TrackedImage& image = tracked(transition.image.index);
            if (transition.barrier()) {
                barriers.push_back(
                    image_barrier(image.handle(), image.aspect(), transition.from, transition.to));
            }
            uses[transition.image.index] = transition.to;
        }
        record_barriers(commands, barriers);
        if (frame.timer != nullptr) {
            frame.timer->begin(commands, frame.slot, pass.name);
        }
        if (pass.targets && !rendering) {
            begin_targets(commands, *pass.targets);
            rendering = true;
        }
        recorded = records_[at](commands);
        if (frame.timer != nullptr) {
            frame.timer->end(commands, frame.slot);
        }
    }
    if (rendering) {
        vkCmdEndRendering(commands);
    }
    for (std::size_t image = 0; image < images_.size(); ++image) {
        tracked(image).used_as(uses[image]);
    }
    return recorded;
}

void RenderGraph::begin_targets(VkCommandBuffer commands, const GraphTargets& targets) const {
    const GraphImage colour = targets.colour.image;
    if (targets.depth) {
        begin_rendering(commands, view(colour), extent(colour), targets.colour.clear,
                        {.view = view(targets.depth->image), .clear = targets.depth->clear});
    } else {
        begin_rendering(commands, view(colour), extent(colour), targets.colour.clear);
    }
}

} // namespace hog::render
