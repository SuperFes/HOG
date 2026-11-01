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
#include <map>
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

    // Meshes drawn besides, each kept apart by its key and uploaded once, as it's next drawn,
    // as chunks streamed in and out are: placing one under a key already held replaces it.
    void place(std::uint64_t key, std::shared_ptr<const terrain::BlockMesh> mesh);
    void remove(std::uint64_t key);
    [[nodiscard]] std::size_t placed() const noexcept { return placed_.size(); }

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

    // A mesh placed, on the GPU once uploaded.
    struct Placed {
        std::shared_ptr<const terrain::BlockMesh> mesh;
        Buffer vertices;
        Buffer indices;
        std::uint32_t count = 0;
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        WorldPosition origin = {};
    };

    [[nodiscard]] Result<> copy(Copy& copy) const;
    [[nodiscard]] Result<> upload(Placed& placed) const;
    // Lets a mesh go once no frame in flight can still be drawing it.
    void retire(Placed placed);
    void draw_one(VkCommandBuffer commands, const Buffer& vertices, const Buffer& indices,
                  std::uint32_t count, WorldPosition origin, const BlockView& view,
                  double aspect) const;

    const Device* device_ = nullptr;
    Pipeline pipeline_;
    std::vector<Copy> copies_;
    std::shared_ptr<const terrain::BlockMesh> mesh_;
    std::uint64_t shown_ = 0;
    std::map<std::uint64_t, Placed> placed_;
    std::vector<std::pair<std::uint64_t, Placed>> retiring_; // by the draw they were let go at
    std::uint64_t draws_ = 0;
};

} // namespace hog::render
