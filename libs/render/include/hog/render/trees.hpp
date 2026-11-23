#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/flora/asset.hpp>
#include <hog/flora/placement.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::render {

class Device;

// A tree to draw: where its trunk meets the ground, and who it is, its species and variant as
// they were added.
struct TreeDraw {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition foot = {};
    flora::Planted tree;
};

// Where trees are seen from and lit by.
struct TreeView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
};

// Metres a pixel spans `distance` away, seen at `extent` high through `vertical_fov`.
[[nodiscard]] double pixel_metres(double distance, double vertical_fov, std::uint32_t height);

// Of a part's pieces (or leaves) sorted by the pixel they drop at, largest first, how many stay
// at `metres_per_pixel`.
[[nodiscard]] std::uint32_t kept_of(std::span<const float> drops, float metres_per_pixel);

// The trees the tree lab bakes, their meshes and leaves drawn whole near, thinned with distance
// as each part's pieces drop out, lit through the air as the ground is.
class Trees {
public:
    [[nodiscard]] static Result<Trees> create(Device& device, VkFormat colour_format,
                                              VkFormat depth_format, std::uint32_t frames = 1);

    // Uploads the first `most_variants` of the species' variants, its meshes and leaves; species
    // are numbered in the order added.
    [[nodiscard]] Result<> add(const flora::Species& from, std::uint32_t most_variants);
    [[nodiscard]] std::size_t species() const noexcept { return species_.size(); }

    // Metres from the eye trees are drawn to.
    void set_reach(double reach) noexcept { reach_ = reach; }

    // Records drawing those of `trees` within reach and in view into the colour and depth images
    // being rendered to, of `extent`. The frame's last submit must have finished.
    [[nodiscard]] Result<> draw(VkCommandBuffer commands, VkExtent2D extent, const TreeView& view,
                                std::span<const TreeDraw> trees, std::uint32_t frame);

    // How many trees, triangles and leaves the last draw drew.
    struct Drawn {
        std::size_t trees = 0;
        std::size_t triangles = 0;
        std::size_t leaves = 0;
    };
    [[nodiscard]] const Drawn& drawn() const noexcept { return drawn_; }

private:
    Trees() = default;

    struct Part {
        flora::MeshRole role = flora::MeshRole::Wood;
        std::uint32_t dims = 0;
        float target = 0.0F;
        float translucency = 0.0F;
        float cutoff = 0.0F;
        bool bark = false;
        Vec2 uv_scale{1.0F};
        std::int32_t albedo = -1;
        Buffer records;
        Buffer pieces;
        Buffer indices;
        std::vector<float> drops;
        std::vector<std::uint32_t> index_ends;
    };
    struct Leaves {
        float target = 0.0F;
        float translucency = 0.0F;
        float cutoff = 0.0F;
        std::int32_t albedo = -1;
        Buffer records;
        std::vector<float> drops;
    };
    struct Variant {
        Vec3 lo{0.0F};
        Vec3 hi{0.0F};
        std::vector<Part> meshes;
        std::optional<Leaves> leaves;
    };
    struct Species {
        float colour_range = 1.0F;
        Buffer leaf_shape;
        std::vector<std::optional<Image>> textures;
        std::vector<Variant> variants;
    };
    // One tree as drawn this frame.
    struct Placed {
        const Variant* variant = nullptr;
        const Species* species = nullptr;
        std::array<Vec4, 4> axes{}; // foot and metres a pixel, then x, y and z with the tint
    };

    [[nodiscard]] Result<Variant> upload_variant(Species& species, const flora::Species& from,
                                                 const flora::Variant& variant);
    [[nodiscard]] Result<> upload_texture(Species& species, const flora::Species& from,
                                          std::int32_t id);
    [[nodiscard]] VkImageView texture(const Species& species, std::int32_t id) const;
    // Into placed_, the trees in reach and in view, as drawn.
    void place(VkExtent2D extent, const TreeView& view, std::span<const TreeDraw> trees);
    void draw_meshes(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame);
    void draw_leaves(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame);
    void push_air(VkCommandBuffer commands, const Pipeline& pipeline, const TreeView& view,
                  std::uint32_t frame) const;

    Device* device_ = nullptr;
    Pipeline meshes_;
    Pipeline leaves_;
    Image white_;
    Sampler sampler_;
    std::vector<Buffer> frames_;
    std::vector<Species> species_;
    std::vector<Placed> placed_;
    double reach_ = 250.0;
    Drawn drawn_;
};

} // namespace hog::render
