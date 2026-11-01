#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/blocks.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace hog::render {

class Device;

// Where blocks are seen from and lit by.
struct BlockView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // How bright the sky leaves sides facing away from the sun: all of it looking up, 0.6 of it
    // side on, 0.2 looking down.
    double night = 0.25;
};

// The volume as blocks, as terrain::BlockStream fills them in: each side flat, in its material's
// colour, lit by the sun and the sky.
class Blocks {
public:
    // `frames` in flight, each with its own copy of the blocks.
    [[nodiscard]] static Result<Blocks> create(Device& device, VkFormat colour_format,
                                               VkFormat depth_format, std::uint32_t frames = 1);

    // The blocks drawn from now on; each frame copies them as it's next drawn.
    void show(std::shared_ptr<const terrain::BlockMesh> mesh);

    // Records drawing into the colour and depth images being rendered to, of `extent`. The
    // frame's last submit must have finished.
    [[nodiscard]] Result<> draw(VkCommandBuffer commands, VkExtent2D extent, const BlockView& view,
                                std::uint32_t frame);

private:
    Blocks() = default;

    // A frame's copy of the blocks.
    struct Copy {
        Buffer vertices;
        Buffer indices;
        std::uint32_t count = 0;
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        WorldPosition origin = {};
        std::uint64_t shown = 0;
    };

    [[nodiscard]] Result<> copy(Copy& copy) const;

    const Device* device_ = nullptr;
    Pipeline pipeline_;
    std::vector<Copy> copies_;
    std::shared_ptr<const terrain::BlockMesh> mesh_;
    std::uint64_t shown_ = 0;
};

} // namespace hog::render
