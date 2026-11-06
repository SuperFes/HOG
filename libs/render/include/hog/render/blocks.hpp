#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/block_mesh.hpp>
#include <hog/terrain/material.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
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
    // Whole metres round the eye where ground isn't drawn, so what lies past it shows from inside
    // the ground, lit there by a lamp at the eye; none draws the ground as it is.
    double cutaway = 0.0;
    // What water standing on the ground mirrors: a cube as EnvironmentProbe takes it round the
    // eye, or none, which mirrors a plain sky.
    VkImageView environment = VK_NULL_HANDLE;
};

// The most a cutaway can be, in metres.
inline constexpr double block_cutaway_most = 200.0;
// The share of it, from the eye, where ground isn't drawn; the rest is left for what's drawn at
// its edge.
inline constexpr double block_cutaway_inside = 0.8;

// Metres a tile repeats over, and the far one laid over it, to break the near one's repetition.
inline constexpr double block_tile = 4.0;
inline constexpr double block_far_tile = 32.0;

// Surfaces as block meshes have them, lit by the sun and the sky, each triangle textured by the
// tiles its corners name: projected along the planet's axes by its normal, at two scales.
class Blocks {
public:
    // `tiles` in the order vertices name them, all of one size; `frames` in flight, each with its
    // own copy of the blocks shown.
    [[nodiscard]] static Result<Blocks> create(Device& device, VkFormat colour_format,
                                               VkFormat depth_format,
                                               std::span<const terrain::MaterialImage> tiles,
                                               std::uint32_t frames = 1);

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
    // The same in two halves, for drawing a frame's blocks more than once, as into a probe's
    // faces: readies the frame's copy and what's placed, once a frame, then draws them as often
    // as asked.
    [[nodiscard]] Result<> prepare(std::uint32_t frame);
    void draw_prepared(VkCommandBuffer commands, VkExtent2D extent, const BlockView& view,
                       std::uint32_t frame) const;

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

    // A mesh placed, on the GPU once uploaded, and a ball about it from its origin.
    struct Placed {
        std::shared_ptr<const terrain::BlockMesh> mesh;
        Buffer vertices;
        Buffer indices;
        std::uint32_t count = 0;
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        WorldPosition origin = {};
        DVec3 middle{0.0};
        double radius = 0.0;
    };

    [[nodiscard]] Result<> copy(Copy& copy) const;
    // Whether any of the mesh can be in the camera's view.
    [[nodiscard]] static bool seen(const Camera& camera, double aspect, const Placed& placed);
    [[nodiscard]] Result<> upload(Placed& placed) const;
    // Lets a mesh go once no frame in flight can still be drawing it.
    void retire(Placed placed);
    void draw_one(VkCommandBuffer commands, const Buffer& vertices, const Buffer& indices,
                  std::uint32_t count, WorldPosition origin, const BlockView& view,
                  double aspect) const;

    const Device* device_ = nullptr;
    Pipeline pipeline_;
    Image tiles_;
    Image nothing_; // a cube holding nothing, mirrored where no probe's been taken
    Sampler mirror_sampler_;
    Sampler sampler_;
    std::vector<Copy> copies_;
    std::shared_ptr<const terrain::BlockMesh> mesh_;
    std::uint64_t shown_ = 0;
    std::map<std::uint64_t, Placed> placed_;
    std::vector<std::pair<std::uint64_t, Placed>> retiring_; // by the draw they were let go at
    std::uint64_t draws_ = 0;
};

} // namespace hog::render
