#pragma once

#include <hog/atmosphere/model.hpp>
#include <hog/chunks/globe_stream.hpp>
#include <hog/client/forecast.hpp>
#include <hog/client/globe_view.hpp>
#include <hog/client/picture.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/ephemeris.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/billboard.hpp>
#include <hog/render/blocks.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/capsules.hpp>
#include <hog/render/clouds.hpp>
#include <hog/render/environment_probe.hpp>
#include <hog/render/glare.hpp>
#include <hog/render/gpu_timer.hpp>
#include <hog/render/gusts.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/precipitation.hpp>
#include <hog/render/presenter.hpp>
#include <hog/render/render_graph.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/render/weather_maps.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/waves/waves.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::client {

// Vertical field of view, radians.
inline constexpr double vertical_fov = 1.0;

// Reversed, so near things keep their precision far from the eye.
inline constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

// Stills are sRGB, as the window shows them.
inline constexpr VkFormat still_format = VK_FORMAT_R8G8B8A8_SRGB;

// Of the weather's drift, how fast the wind blows what falls near the ground.
inline constexpr double surface_wind = 0.5;

// What a frame shows, and when.
struct Shot {
    render::Camera camera;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    GameTime time;
    double seconds = 0.0; // since the frame before, for the eye to adapt to the light over
    // Metres round the eye where ground isn't drawn, for looking round inside it; none to draw it.
    double cutaway = 0.0;
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

// The planet as the client draws it: the globe's surfaces meshed round the eye, the sea, the air
// lit by the sun at the planet's time, other players, whoever flies along, the weather's clouds
// and what falls from them, and the sun's glare over it all.
class Scene {
public:
    // Drawn into colour images of `colour_format`, `frames` in flight, with the sun and seasons
    // from `calendar`, on a world of `planet`'s size and seed.
    [[nodiscard]] static Result<Scene>
    create(render::Device& device, const globe::Planet& planet, jobs::JobSystem& jobs,
           const sim::Calendar& calendar, VkFormat colour_format, std::uint32_t frames,
           std::optional<render::Billboard> companion = std::nullopt);
    Scene(const Scene&) = delete;
    Scene(Scene&&) noexcept = default;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) noexcept = default;
    // Waits for the chunks in flight, which read the world. The GPU must have finished with it.
    ~Scene();

    // Records drawing the shot into the frame's target, leaving it a colour target. The frame's
    // slot must have finished its last submit.
    Result<> record(render::Frame& frame, const Shot& shot);
    // Waits for the globe's chunks in flight, so they arrive with the frames to come.
    void finish_bakes();

    // The last frame recorded wanted nothing more meshed, and all it wanted had arrived; and,
    // once it's finished, it took a probe just as the eye turned out under the water.
    [[nodiscard]] bool settled() const;
    // Whether the last frame recorded took what's out of the water all round, the eye under it.
    [[nodiscard]] bool probing() const noexcept { return probing_; }
    // How long the GPU took over the passes of the last frame recorded in the slot before, as
    // read when the last frame was recorded; empty where the GPU can't time.
    [[nodiscard]] std::span<const render::GpuSpan> timings() const noexcept { return timings_; }

    // Draws the globe's surfaces round the eye. The globe must outlive the scene, unchanged
    // while chunks are in flight.
    void follow_globe(const globe::World& globe);
    // The globe's stream of surfaces, once it's followed.
    [[nodiscard]] const chunks::GlobeStream* globe_stream() const noexcept {
        return globe_ ? &*globe_ : nullptr;
    }
    // Waits for the globe's chunks in flight, so the world may be changed; then, once it has,
    // meshes again the chunks the changed bricks touch.
    void finish_globe();
    void change_globe(std::span<const globe::BrickSite> bricks);
    // Draws surfaces a server sent in place of any under their keys, and takes the gone away.
    void show_chunks(chunks::GlobeStream::Changes changes) { show_globe(std::move(changes)); }
    // The ground's inside drawn round the eye, in place of any shown before; none takes it away.
    void show_cutaway(std::shared_ptr<const terrain::BlockMesh> shell);

    [[nodiscard]] render::CloudQuality cloud_quality() const noexcept { return clouds_.quality(); }
    // The GPU must have finished with the frames recorded before.
    Result<> set_cloud_quality(render::CloudQuality quality) {
        return clouds_.set_quality(quality);
    }
    [[nodiscard]] const atmosphere::Model& air() const noexcept { return model_; }
    [[nodiscard]] const sim::Calendar& calendar() const noexcept { return calendar_; }
    // The stars and moons that draw the tide, Earth's unless set.
    void set_system(planet::System system) {
        ephemeris_ =
            planet::Ephemeris{std::move(system), calendar_.day_length(), calendar_.axial_tilt()};
    }

private:
    Scene(const render::Device& device, const globe::Planet& planet, jobs::JobSystem& jobs,
          const sim::Calendar& calendar, render::Ocean ocean, render::Blocks blocks,
          render::Atmosphere atmosphere, render::Sky sky, render::Capsules others,
          render::WeatherMaps weather, render::Clouds clouds, render::Precipitation precipitation,
          render::Glare glare, render::GpuTimer timer, std::optional<render::Billboard> companion,
          std::uint32_t frames);

    render::SkyView sky_view(const Shot& shot, render::CloudShade shade);
    // Wants the globe's chunks round the eye, and shows those that have come.
    void follow_ground(const Shot& shot);
    // Shows the globe's chunks that have come, and takes away those gone.
    void show_globe(chunks::GlobeStream::Changes changes);
    // From under the water, as the sea last found the eye in `frame`'s slot, adds the pass
    // taking what's out of it all round, seen as `sky` has it; whether it did.
    [[nodiscard]] bool take_probe(render::RenderGraph& graph, const render::Frame& frame,
                                  const Shot& shot, const render::SkyView& sky,
                                  std::span<const render::GraphUse> through_air);
    // Adds the sea pass over what's drawn to `target` and `depth`, seeing through to what's
    // behind it.
    void add_water(render::RenderGraph& graph, const render::Frame& frame,
                   const render::OceanView& sea, std::span<const render::GraphUse> through_air,
                   render::GraphImage target, render::GraphImage depth,
                   const render::GraphTargets& drawn);
    // Adds the pass drawing the deep water and the globe's chunks on it.
    void add_ground(render::RenderGraph& graph, const render::Frame& frame, const Shot& shot,
                    const render::OceanView& sea, std::span<const render::GraphUse> through_air,
                    const render::GraphTargets& drawn);

    jobs::JobSystem* jobs_ = nullptr;
    // What's out of the water all round the eye, for its surface seen from under it: a probe for
    // each frame in flight.
    std::vector<render::EnvironmentProbe> probes_;
    render::Ocean ocean_;
    render::Blocks blocks_;
    // The globe's surfaces round the eye, drawn as the blocks once followed.
    std::optional<chunks::GlobeStream> globe_;
    std::vector<waves::Train> wave_trains_;
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
    planet::Ephemeris ephemeris_;
    std::optional<double> exposure_;
    // How many times weather had come when its maps were last taken.
    std::optional<std::uint64_t> weather_taken_;
    double flowed_ = 0.0;      // seconds the billows have flowed, within render::flow_period
    DVec3 foam_drifted_{0.0};  // and the water at the sea's surface
    double falling_for_ = 0.0; // seconds what falls has been falling
    render::Gusts gusts_;
    bool settled_ = false;
    // Whether the last frame recorded took a probe.
    bool probing_ = false;
};

// The shot drawn into a picture of `extent` once the scene has settled: frame after frame, with the
// chunks finished between, up to `most_frames`. The scene must draw into still_format.
[[nodiscard]] Result<Picture> draw_still(render::Device& device, Scene& scene, const Shot& shot,
                                         VkExtent2D extent, int most_frames = 500);

} // namespace hog::client
