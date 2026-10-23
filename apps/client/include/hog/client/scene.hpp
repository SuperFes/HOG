#pragma once

#include <hog/atmosphere/model.hpp>
#include <hog/client/anchors.hpp>
#include <hog/client/bushes.hpp>
#include <hog/client/forecast.hpp>
#include <hog/client/ground.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/woodlands.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/billboard.hpp>
#include <hog/render/bushes.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/capsules.hpp>
#include <hog/render/clouds.hpp>
#include <hog/render/forest.hpp>
#include <hog/render/glare.hpp>
#include <hog/render/gpu_timer.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/precipitation.hpp>
#include <hog/render/presenter.hpp>
#include <hog/render/render_graph.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/terrain.hpp>
#include <hog/render/trees.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/render/weather_maps.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/baker.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/shore.hpp>
#include <hog/terrain/stream.hpp>
#include <hog/waves/waves.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::client {

inline constexpr double vertical_fov = 1.0; // radians
inline constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
// Stills are sRGB, as the window shows them.
inline constexpr VkFormat still_format = VK_FORMAT_R8G8B8A8_SRGB;
// Of the weather's drift, how fast the wind blows what falls near the ground.
inline constexpr double surface_wind = 0.5;

// Metres above sea level of the ground under a position.
[[nodiscard]] double ground_under(const planet::World& world, WorldPosition at);

// What a frame shows, and when.
struct Shot {
    render::Camera camera;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    GameTime time;
    double seconds = 0.0; // since the frame before, for the eye to adapt to the light over
    // Whoever flies along, where there's someone to draw.
    std::optional<render::Standing> companion;
    // Other players.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::vector<render::Capsule> others = {};
    // The weather shown, if any is known, and the real time it's shown at, for its fades.
    const Forecast* forecast = nullptr;
    MonotonicClock::time_point now{};
    // Drawn on its own, with no frames before it: what's kept over frames is caught up on at once,
    // which would stall a frame in play.
    bool still = false;
};

// The weather at the eye as the shot shows it: a clear sky while none is known.
[[nodiscard]] protocol::CellWeather weather_at_eye(const Shot& shot);
// The sea under the eye as the shot shows it: a calm one while none is known.
[[nodiscard]] waves::SeaState sea_at_eye(const Shot& shot);
// The weather's drift at the eye in the planet's frame, m/s.
[[nodiscard]] DVec3 drift_at(DVec3 up, const protocol::CellWeather& weather);
// mm an hour falling round the eye: none above the cloud it falls from.
[[nodiscard]] double falling_at_eye(const terrain::Bend& bend,
                                    const protocol::CellWeather& weather) noexcept;
// What cloud over the eye lets through of the light falling on it: all of it above the cloud, and
// within it, what the cloud over the eye lets through.
[[nodiscard]] render::CloudShade shade_at_eye(const terrain::Bend& bend,
                                              const protocol::CellWeather& weather);

// The planet as the client draws it: the ground baked and streamed in round the eye, the sea, the
// air lit by the sun at the planet's time, other players, whoever flies along, the weather's
// clouds and what falls from them, and the sun's glare over it all.
class Scene {
public:
    // Drawn into colour images of `colour_format`, `frames` in flight, with the sun and seasons
    // from `calendar`, and the plants as far and thick as `budget` has them.
    [[nodiscard]] static Result<Scene>
    create(render::Device& device, const planet::World& world, jobs::JobSystem& jobs,
           const sim::Calendar& calendar, VkFormat colour_format, std::uint32_t frames,
           std::optional<render::Billboard> companion = std::nullopt,
           const GrassBudget& budget = {});

    Scene(const Scene&) = delete;
    Scene(Scene&&) noexcept = default;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) noexcept = default;
    // Waits for the bakes in flight, which read the world. The GPU must have finished with it.
    ~Scene();

    // Records drawing the shot into the frame's target, leaving it a colour target. The frame's
    // slot must have finished its last submit.
    Result<> record(render::Frame& frame, const Shot& shot);

    // Waits for the bakes and the plants' tiles in flight, so they arrive with the frames to come.
    void finish_bakes();

    // Takes the server's word of how the woods fare: the trees and far woods over the cells it
    // changed are grown again, drawn as they were until then.
    void fare(const protocol::Woodlands& told);

    // The last frame recorded wanted nothing more baked, and all it wanted had arrived.
    [[nodiscard]] bool settled() const noexcept { return settled_; }
    // How long the GPU took over the passes of the last frame recorded in the slot before, as
    // read when the last frame was recorded; empty where the GPU can't time.
    [[nodiscard]] std::span<const render::GpuSpan> timings() const noexcept { return timings_; }
    // How many trees the last frame recorded drew, for the overlay.
    [[nodiscard]] const TreeCounts& tree_counts() const noexcept { return tree_counts_; }

    // Whether grass is grown on the ground near the eye; it is unless turned off.
    void set_grass(bool grass) noexcept { grass_on_ = grass; }
    [[nodiscard]] bool grass() const noexcept { return grass_on_; }

    [[nodiscard]] render::CloudQuality cloud_quality() const noexcept { return clouds_.quality(); }
    // The GPU must have finished with the frames recorded before.
    Result<> set_cloud_quality(render::CloudQuality quality) {
        return clouds_.set_quality(quality);
    }

    [[nodiscard]] const atmosphere::Model& air() const noexcept { return model_; }
    [[nodiscard]] const sim::Calendar& calendar() const noexcept { return calendar_; }

private:
    Scene(const render::Device& device, const planet::World& world, jobs::JobSystem& jobs,
          const sim::Calendar& calendar, render::Terrain terrain, render::Ocean ocean,
          render::Grass grass, render::Bushes bushes, render::Trees trees, render::Forest forest,
          render::TerrainPool pool, render::AnchorPool anchors, render::AnchorPool bush_pool,
          render::Atmosphere atmosphere, render::Sky sky, render::Capsules others,
          render::WeatherMaps weather, render::Clouds clouds, render::Precipitation precipitation,
          render::Glare glare, render::GpuTimer timer, std::optional<render::Billboard> companion,
          std::uint32_t frames, const GrassBudget& budget);

    render::SkyView sky_view(const Shot& shot, render::CloudShade shade);
    // Takes in the bakes done, and streams the ground selected from `seen`.
    terrain::StreamFrame stream_ground(const terrain::Viewpoint& seen);
    // Wants the depths round the eye, and takes any worked out; or gives the ocean those taken.
    void follow_shore(const Shot& shot);
    Result<> give_shore(VkCommandBuffer commands, std::uint32_t slot);
    // Adds the pass growing the grass on the drawn patches, blown by `wind`, ready to draw; false
    // where none grows.
    [[nodiscard]] bool grow_grass(render::RenderGraph& graph, const render::Frame& frame,
                                  const Shot& shot, std::span<const terrain::StreamedPatch> patches,
                                  const render::AirBindings& air, DVec3 wind);
    // Adds the grass pass, if `lawn` was grown, and the bush and tree passes, blown by `wind`.
    void grow_vegetation(render::RenderGraph& graph, const render::Frame& frame, const Shot& shot,
                         bool lawn, std::span<const render::GraphUse> through_air,
                         const render::GraphTargets& drawn, const render::AirBindings& air,
                         DVec3 wind);

    const planet::World* world_ = nullptr;
    jobs::JobSystem* jobs_ = nullptr;
    TreeCounts tree_counts_;
    terrain::PatchBaker baker_;
    terrain::PatchStream stream_;
    std::vector<terrain::SelectedPatch> selected_;
    terrain::Viewpoint selected_from_;
    render::Terrain terrain_;
    render::Ocean ocean_;
    terrain::ShoreStream shore_;
    // Worked out, and yet to be given to the ocean.
    std::optional<terrain::ShoreDepths> shore_depths_;
    std::vector<waves::Train> wave_trains_;
    render::Grass grass_;
    render::Bushes bushes_;
    render::Trees trees_;
    render::Forest forest_;
    render::TerrainPool pool_;
    render::AnchorPool anchor_pool_;
    TreeAnchors anchors_;
    render::AnchorPool bush_pool_;
    BushAnchors bush_anchors_;
    Woodlands woodlands_;
    GroundSlots slots_;
    render::TransientImages transients_;
    render::Capsules others_;
    std::optional<render::Billboard> companion_;
    atmosphere::Model model_;
    render::Atmosphere atmosphere_;
    render::Sky sky_;
    render::WeatherMaps weather_;
    render::Clouds clouds_;
    render::Precipitation precipitation_;
    render::Glare glare_;
    render::GpuTimer timer_;
    std::vector<render::GpuSpan> timings_;
    sim::Calendar calendar_;
    GrassBudget budget_;
    terrain::SelectionSettings selection_;
    std::optional<double> exposure_;
    // How many times weather had come when its maps were last taken.
    std::optional<std::uint64_t> weather_taken_;
    DVec3 drifted_{0.0};       // metres the clouds have drifted
    DVec3 foam_drifted_{0.0};  // and the water at the sea's surface
    DVec3 gusts_drifted_{0.0}; // and the gusts over the ground
    double falling_for_ = 0.0; // seconds what falls has been falling
    double swaying_for_ = 0.0; // seconds the grass has swayed
    bool grass_on_ = true;
    bool settled_ = false;
};

// The shot drawn into a picture of `extent` once the scene has settled: frame after frame, with the
// bakes finished between, up to `most_frames`. The scene must draw into still_format.
[[nodiscard]] Result<Picture> draw_still(render::Device& device, Scene& scene, const Shot& shot,
                                         VkExtent2D extent, int most_frames = 500);

} // namespace hog::client
