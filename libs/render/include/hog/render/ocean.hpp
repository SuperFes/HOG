#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/foam.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/ripples.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/shore.hpp>
#include <hog/waves/shoaling.hpp>
#include <hog/waves/waves.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::render {

class Device;
struct Capsule;

// The most waves the sea is drawn with. As shaders/waves.slang has it.
inline constexpr std::size_t drawn_waves = 48;
// The plane waves summed for the pattern of its foam. Likewise.
inline constexpr std::size_t foam_waves = 26;

// The waves the sea is drawn with, as shaders/waves.slang takes them.
struct DrawnWaves {
    struct Wave {
        glm::vec4 heading_wavenumber{0.0F};
        glm::vec4 amplitude_phase_slope_heave{0.0F};
        glm::vec4 along{0.0F};
        glm::vec4 breaking{0.0F};
        glm::vec4 still{0.0F};
    };
    struct FoamWave {
        glm::vec4 wavenumber_phase{0.0F};
        glm::vec4 amplitude_length{0.0F};
    };
    glm::vec4 count_left_radius{0.0F};
    glm::vec4 up_height{0.0F};
    glm::vec4 first{0.0F};
    glm::vec4 second{0.0F};
    glm::vec4 foam{0.0F};
    glm::vec4 foam_left{0.0F};
    // Where the depths round the eye lie, as shaders/shore.slang takes them: none unless any.
    glm::vec4 shore_anchor{0.0F};
    glm::vec4 shore_east{0.0F};
    glm::vec4 shore_north{0.0F};
    // xyz how the tide rises per metre from the eye, in the planet's frame; w how high it stands
    // over the sea's mean at the eye.
    glm::vec4 tide{0.0F};
    // Of the waves running up the shore, as shaders/waves.slang takes them: their scale, the depth
    // they set the sea up from, and how fast each part of their swash turns; then each's wavenumber
    // along the ground and phase through the eye.
    glm::vec4 run_up{0.0F};
    glm::vec4 swash_incident{0.0F};
    glm::vec4 swash_surging{0.0F};
    // Where the grid of foam left round the eye lies, as shaders/waves.slang takes it from a
    // GridPlacing: none unless any.
    glm::vec4 foam_anchor{0.0F};
    glm::vec4 foam_east{0.0F};
    glm::vec4 foam_north{0.0F};
    glm::vec4 foam_drifted{0.0F};
    // Where the grid of ripples round the eye lies, likewise.
    glm::vec4 ripple_anchor{0.0F};
    glm::vec4 ripple_east{0.0F};
    glm::vec4 ripple_north{0.0F};
    // Of rain ringing the water: x rain_ring_chance, y seconds, counted round now and then.
    glm::vec4 rain{0.0F};
    // Of the waves on still water, as shaders/waves.slang takes them: the wind over it and how they
    // grow by their fetch under it, how white they'd be grown full, and of those left out.
    glm::vec4 still_wind{0.0F};
    glm::vec4 still_growth{0.0F};
    glm::vec4 still_white{0.0F};
    std::array<glm::vec4, waves::bands / 4> still_left{};
    std::array<Wave, drawn_waves> waves{};
    std::array<FoamWave, foam_waves> foam_pattern{};
};

// The strongest of the waves round an eye at `eye`, bent by `bend`, at `time`, as
// waves::strongest picks them, each as it runs under the eye; where `sea`, which raised them, is
// white with foam, carried with the water at its surface, which has drifted `drifted` metres
// since the epoch; and the tide round the eye on a planet whose day lasts `day`, none if none.
// Picked with them, the waves the sea's wind raises on still water, as far as it has grown there.
[[nodiscard]] DrawnWaves drawn_waves_at(std::span<const waves::Train> trains,
                                        std::span<const waves::Wave> waves,
                                        const waves::SeaState& sea, WorldPosition eye,
                                        const terrain::Bend& bend, GameTime time, DVec3 drifted,
                                        GameDuration day = {});

// What draws over the ground at sea level read of the sea: as shaders/waves.slang's Waves and the
// depths and fetches shaders/shore.slang reads, and the foam a FoamMap keeps.
struct SeaBindings {
    VkBuffer waves = VK_NULL_HANDLE;
    VkImageView shore = VK_NULL_HANDLE;
    VkImageView fetches = VK_NULL_HANDLE;
    VkImageView foam = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

// A calm sea at its mean with no depths round the eye, for what's drawn without the sea's own.
class CalmSea {
public:
    CalmSea() = default;
    [[nodiscard]] static Result<CalmSea> create(Device& device);

    [[nodiscard]] SeaBindings bindings() const;
    // A cube holding nothing, its alpha 0, for an eye in the water with no probe taken.
    [[nodiscard]] VkImageView environment() const noexcept { return environment_.view(); }

private:
    Buffer waves_;
    Image depths_;
    Image fetches_;
    Image foam_;
    Image environment_;
    Sampler sampler_;
};

// Depths of the sea and of still water, and the level still water is kept to, as
// terrain::ShoreDepths has them, as the ocean's shaders sample them: four half floats a texel, the
// sea's depth, still water's, and the level split in two, a half float near it and what's left.
[[nodiscard]] std::vector<std::uint16_t> shore_texels(std::span<const float> depths,
                                                      std::span<const float> still,
                                                      std::span<const float> levels);
// Layers of the reach each layer of depths has: the fetch's, four headings to each, then one of
// how far offshore it lies, metres and how fast that grows east and north.
inline constexpr std::uint32_t reach_layers = (terrain::fetch_headings / 4) + 1;
// Fetches and how far offshore as terrain::shore_fetches and shore_offshore work them out, and how
// much of what rivers carry is mixed in, as terrain::shore_plumes, as the ocean's shaders sample
// them: half floats, reach_layers of them to each of the depths', the plumes alongside how far
// offshore. Without `offshore`, all of it lies as far offshore as is kept; without `plumes`, no
// river's mixed in.
[[nodiscard]] std::vector<std::uint16_t> reach_texels(std::span<const float> fetches,
                                                      std::span<const float> offshore,
                                                      std::span<const float> plumes = {});

// Copies of what's drawn before the sea, colour and depth, as it's seen through.
struct SeaBehind {
    VkImageView colour = VK_NULL_HANDLE;
    VkImageView depth = VK_NULL_HANDLE;
    // What's out of the water all round, as EnvironmentProbe's cube, seen from in it; or as
    // copied, as far as that shows it, without one.
    VkImageView environment = VK_NULL_HANDLE;
};

// Where the sea is seen from and what it reflects.
struct OceanView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0};      // unit, towards the sun
    Vec3 sky{0.35F, 0.55F, 0.85F}; // unless lit through the air
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // The frame's air, for the sea lit through it.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
    // A calm sea unless any.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    DrawnWaves waves = {};
    // What's drawn beneath it, as copied, for a sea lit through the air to see through: where it
    // lays the water itself, as lay_water works out, over what's drawn there as in air.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::optional<SeaBehind> behind = {};
};

// The sea's surface, moved by its waves about sea level, out to the horizon round the point under
// the eye. It reflects the sky and the sun over what's drawn beneath it, more the more glancing the
// view, and hides what's drawn after it below.
class Ocean {
public:
    // Lit through the air, it reflects the sky as the eye sees it and the sun's light through the
    // air, is seen through the air and is tone mapped; otherwise it reflects a sky of one colour.
    // `frames` in flight, each with its own waves.
    [[nodiscard]] static Result<Ocean> create(Device& device, VkFormat colour_format,
                                              VkFormat depth_format, bool air = false,
                                              std::uint32_t frames = 1);

    // Records drawing into the colour and depth images being rendered to, of `extent`, over what's
    // already drawn there. Seeing through what's drawn beneath it, as copied, where the eye is in
    // the water it lays the water over all it sees from there too. The frame's last submit must
    // have finished.
    Result<> draw(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                  std::uint32_t frame = 0) const;
    // Lit through the air and seen from above the sea, records the water beneath the surface as
    // the ground's shading has it where it lies too deep to show, behind all else: drawn before the
    // ground, so ground under the sea left undrawn looks as it would. A sea seeing through what's
    // drawn beneath it, as copied, needs none.
    Result<> draw_deep(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                       std::uint32_t frame = 0) const;

    // Lit through the air, records working out whether the sea lays its water over what's drawn
    // beneath it: where it's `seen_through`, given what's drawn there as copied, from over its
    // surface or under it; and the plane of its surface at the eye. Written into the frame's
    // lighting, view.air's, for all drawn after to read, which are then drawn as in air under it.
    Result<> lay_water(VkCommandBuffer commands, const OceanView& view, std::uint32_t frame,
                       bool seen_through) const;

    // Records copying in the depths round the eye, of the sea and of still water and the fetch
    // over both, for the frames from `frame` on, whose last submit must have finished.
    Result<> update_shore(VkCommandBuffer commands, std::uint32_t frame,
                          const terrain::ShoreDepths& shore);
    [[nodiscard]] const std::optional<terrain::ShoreFrame>& shore() const noexcept {
        return shore_;
    }

    // The sea of the frame `frame`, as it's drawn there: ground drawn before the sea in that frame
    // reads it as the sea draws itself.
    [[nodiscard]] SeaBindings bindings(std::uint32_t frame = 0) const;

    // Records stepping the foam left on the sea round the eye on to `time`, as the sea's seen in
    // the frame `frame`, the water at its surface drifting at `drift`, m/s, outside any rendering;
    // where nothing's kept, catching up on what was left before if `catch_up`. The frame's last
    // submit must have finished.
    Result<> step_foam(VkCommandBuffer commands, const OceanView& view, std::uint32_t frame,
                       GameTime time, DVec3 drift, bool catch_up);
    [[nodiscard]] const FoamMap& foam() const noexcept { return foam_; }
    // Records stepping the ripples on the water round the eye on to `time`, as step_foam does the
    // foam, pushed by `bodies` where they cross the surface; and drawing the rain's rings on them.
    Result<> step_ripples(VkCommandBuffer commands, const OceanView& view, std::uint32_t frame,
                          GameTime time, DVec3 drift, std::span<const Capsule> bodies = {});
    [[nodiscard]] const RippleMap& ripples() const noexcept { return ripples_; }
    [[nodiscard]] RippleMap& ripples() noexcept { return ripples_; }
    [[nodiscard]] const RainTile& rain() const noexcept { return rain_; }

private:
    Ocean() = default;

    // Of the frame `frame`, writes its waves as the sea's seen.
    [[nodiscard]] Result<> write_waves(const OceanView& view, std::uint32_t frame) const;

    Result<> record(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                    std::uint32_t frame, const Pipeline& pipeline) const;

    Pipeline pipeline_;
    Pipeline deep_;
    // Laying the water over what's drawn beneath, as copied, and whether to.
    Pipeline through_;
    Pipeline laid_;
    // Laying the water over what's seen from an eye in it, as copied.
    Pipeline under_;
    Buffer vertices_;
    Buffer indices_;
    std::vector<Buffer> waves_;
    StreamedImage shore_depths_;
    StreamedImage shore_fetches_;
    // Deep water, until there are depths.
    CalmSea calm_;
    Sampler shore_sampler_;
    std::optional<terrain::ShoreFrame> shore_;
    // Of the lakes round the eye, as terrain::lake_levels has them.
    std::vector<float> lake_levels_;
    DriftingGrid foam_grid_{foam_size, foam_spacing};
    FoamMap foam_;
    std::optional<GameTime> foam_time_;
    DriftingGrid ripple_grid_{ripple_size, ripple_spacing};
    RippleMap ripples_;
    RainTile rain_;
    std::optional<GameTime> ripple_time_;
    std::uint32_t index_count_ = 0;
    bool air_ = false;
};

} // namespace hog::render
