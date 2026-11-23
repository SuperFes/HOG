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

// The trees the tree lab bakes: near, their meshes and leaves, thinned with distance as each
// part's pieces drop out; past that, their branch cards (crossed squares of leaves on mesh limbs);
// then the impostors baked of them, relit; all lit through the air as the ground is.
class Trees {
public:
    [[nodiscard]] static Result<Trees> create(Device& device, VkFormat colour_format,
                                              VkFormat depth_format, std::uint32_t frames = 1);

    // Uploads the first `most_variants` of the species' variants, their meshes, leaves and
    // impostors; species are numbered in the order added.
    [[nodiscard]] Result<> add(const flora::Species& from, std::uint32_t most_variants);
    [[nodiscard]] std::size_t species() const noexcept { return species_.size(); }

    // Metres from the eye trees are drawn to at each level: whole, then each level of branch
    // cards; past the last, as impostors to `far`. A variant with fewer levels draws its last
    // further.
    void set_reach(std::vector<double> levels, double far) {
        reaches_ = std::move(levels);
        far_ = far;
    }

    // How much sooner pieces drop out than the tree lab chose: each drawn as if a pixel spanned
    // this many times the metres it does; leaves `leaves` times sooner again, being most of what's
    // drawn near.
    void set_coarseness(double coarseness, double leaves = 1.0) noexcept {
        coarseness_ = coarseness;
        leaf_coarseness_ = leaves;
    }

    // The trees drawn from now on, in place of those shown before.
    void show(std::vector<TreeDraw> trees);

    // What's drawn of the trees, each recorded on its own so it can be timed on its own.
    enum class Stage : std::uint8_t {
        Whole,  // the nearest trees' meshes
        Leaves, // and their leaves
        Cards,
        Impostors,
    };

    // Readies drawing those shown within reach and in view into images of `extent`: which trees
    // are drawn how, and what the frame's stages read of them. The frame's last submit must have
    // finished.
    [[nodiscard]] Result<> prepare(VkExtent2D extent, const TreeView& view, std::uint32_t frame);
    // Records drawing a stage of what was readied for the frame into the colour and depth images
    // being rendered to.
    void draw(VkCommandBuffer commands, Stage stage, const TreeView& view, std::uint32_t frame);
    // Readies the frame and records every stage, nearest first.
    [[nodiscard]] Result<> draw(VkCommandBuffer commands, VkExtent2D extent, const TreeView& view,
                                std::uint32_t frame);

    // How many trees, triangles and leaves the last draw drew.
    struct Drawn {
        std::size_t trees = 0;
        std::size_t triangles = 0;
        std::size_t leaves = 0;
        std::size_t carded = 0; // of the trees, those drawn as cards, fading in or out too
        std::size_t impostors = 0;
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
        std::int32_t second = -1; // cards: their normals and leafiness
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
    struct Impostor {
        std::uint32_t views = 0;
        Vec3 centre{0.0F};
        float radius = 0.0F;
        std::array<std::int32_t, 5> textures{};
    };
    struct Variant {
        Vec3 lo{0.0F};
        Vec3 hi{0.0F};
        std::vector<std::vector<Part>> levels; // whole, then each level of cards
        std::optional<Leaves> leaves;
        std::optional<Impostor> impostor;
    };
    struct Species {
        float colour_range = 1.0F;
        Buffer leaf_shape;
        Buffer leaf_indices; // of the most leaves a variant has
        std::vector<std::optional<Image>> textures;
        std::vector<Variant> variants;
    };
    // Of the impostors' instances, a run drawn with one variant's impostor.
    struct Run {
        std::uint32_t species = 0;
        std::uint32_t variant = 0;
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };
    // One tree as drawn this frame.
    // One tree at one level as drawn this frame; within a hand-over it's drawn at both, each
    // keeping the pixels the other gives up.
    struct Placed {
        const Variant* variant = nullptr;
        const Species* species = nullptr;
        std::uint32_t level = 0;    // 0 whole, then each level of cards
        float handed = 0.0F;        // of its pixels, the share given up to the next level
        bool incoming = false;      // drawn as the next level, keeping the share given up
        std::array<Vec4, 4> axes{}; // foot and metres a pixel, then x, y and z with the tint
    };

    [[nodiscard]] Result<Part> upload_part(Species& species, const flora::Species& from,
                                           const flora::MeshPart& mesh);
    [[nodiscard]] Result<Variant> upload_variant(Species& species, const flora::Species& from,
                                                 const flora::Variant& variant);
    [[nodiscard]] Result<> upload_texture(Species& species, const flora::Species& from,
                                          std::int32_t id);
    [[nodiscard]] VkImageView texture(const Species& species, std::int32_t id) const;
    // Into placed_, the trees in mesh reach and in view, as drawn.
    void place(VkExtent2D extent, const TreeView& view);
    void draw_meshes(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame);
    // Of placed_, those drawn as cards in carded_, by variant and level, and their trees written
    // for the frame.
    [[nodiscard]] Result<> write_cards(std::uint32_t frame);
    // The level of cards a tree placed past the whole draws: its own, or its variant's last.
    [[nodiscard]] static std::size_t card_level(const Placed& placed);
    // A draw for each variant's level of cards.
    void draw_cards(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame);
    void draw_part(VkCommandBuffer commands, const Species& species, const Variant& variant,
                   const Part& part, std::uint32_t count, const Placed* tree, std::uint32_t first,
                   std::uint32_t trees);
    void draw_leaves(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame);
    // The shown trees' impostor instances, by run, from where the first shown stands.
    [[nodiscard]] Result<> build_instances();
    void draw_impostors(VkCommandBuffer commands, const TreeView& view, std::uint32_t frame);
    void push_air(VkCommandBuffer commands, const Pipeline& pipeline, const TreeView& view,
                  std::uint32_t frame) const;

    Device* device_ = nullptr;
    Pipeline meshes_;
    Pipeline leaves_;
    Pipeline impostors_;
    Sampler atlas_sampler_;
    Image white_;
    Sampler sampler_;
    std::vector<Buffer> frames_;
    std::vector<Species> species_;
    std::vector<Placed> placed_;
    std::vector<const Placed*> carded_;
    bool ready_ = false; // whether the frame's stages have what they draw
    std::vector<TreeDraw> shown_;
    bool shown_changed_ = false;
    WorldPosition origin_;
    Buffer instances_;
    std::vector<Run> runs_;
    // Instances let go, by the draw they were let go at, kept while a frame may read them.
    std::vector<std::pair<std::uint64_t, Buffer>> retiring_;
    std::uint64_t draws_ = 0;
    std::vector<double> reaches_{25.0, 60.0, 130.0, 280.0};
    // Per frame in flight, the trees drawn together this frame.
    std::vector<Buffer> drawn_trees_;
    double coarseness_ = 1.0;
    double leaf_coarseness_ = 1.0;
    double far_ = 1000.0;
    Drawn drawn_;
};

} // namespace hog::render
