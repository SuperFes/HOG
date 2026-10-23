#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/scene.hpp>
#include <hog/core/hash.hpp>
#include <hog/core/profile.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/gusts.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/render_graph.hpp>
#include <hog/sim/weather.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/waves/foam.hpp>
#include <hog/waves/waves.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace hog::client {

namespace {

// Behind the sky, which covers it.
constexpr VkClearColorValue cleared{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}};

// Mixed into the planet's seed for its waves, so they don't follow its ground.
constexpr std::uint64_t waves_salt = 0x5741564553ULL;

// Seabed showing through the sea no more than this is left undrawn: about half a step of 8-bit
// colour.
constexpr double seabed_unseen = 0.002;

// How the ground is selected all round the eye, so it's baked before it's turned to: as finely as
// its own roughness shows, as the grass needs near the eye, and coarse enough elsewhere to keep
// the triangles a few pixels across.
terrain::SelectionSettings ground_selection() {
    const GrassBudget budget;
    return {.max_spacing = 32.0,
            .max_error = 1.0,
            .fine_within = {{.within = render::grass_far, .spacing = budget.finest},
                            {.within = budget.tall_far, .spacing = budget.tall_coarsest}},
            .only_in_view = false,
            .seabed_unseen = seabed_unseen};
}

std::unexpected<Error> failed(std::string_view doing, const Error& error) {
    return std::unexpected(
        Error{.code = error.code, .message = std::format("can't {}: {}", doing, error.message)});
}

} // namespace

double ground_under(const planet::World& world, WorldPosition at) {
    return static_cast<double>(world.state(planet::cell_at(at)).elevation) /
           static_cast<double>(planet::fixed_one);
}

protocol::CellWeather weather_at_eye(const Shot& shot) {
    if (shot.forecast == nullptr || !shot.forecast->known()) {
        return {};
    }
    return shot.forecast->at(shot.bend.up, shot.now);
}

waves::SeaState sea_at_eye(const Shot& shot) {
    if (shot.forecast == nullptr) {
        return {};
    }
    return shot.forecast->sea_at(shot.bend.up, shot.now);
}

DVec3 drift_at(DVec3 up, const protocol::CellWeather& weather) {
    return planet::tangent_frame(up).to_world(
        {static_cast<double>(weather.drift.x), static_cast<double>(weather.drift.y), 0.0});
}

double falling_at_eye(const terrain::Bend& bend, const protocol::CellWeather& weather) noexcept {
    return bend.eye_height < static_cast<double>(weather.base)
               ? static_cast<double>(weather.falling)
               : 0.0;
}

render::CloudShade shade_at_eye(const terrain::Bend& bend, const protocol::CellWeather& weather) {
    const auto base = static_cast<double>(weather.base);
    const auto top = static_cast<double>(weather.top);
    if (bend.eye_height >= top) {
        return {};
    }
    // Within the cloud, only what's over the eye shades it.
    const double over =
        top > base ? std::clamp((top - bend.eye_height) / (top - base), 0.0, 1.0) : 1.0;
    return render::cloud_shade(static_cast<double>(sim::cloud_cover(weather.cloud)),
                               static_cast<double>(weather.cloud) * over);
}

Result<Scene> Scene::create(render::Device& device, const planet::World& world,
                            jobs::JobSystem& jobs, const sim::Calendar& calendar,
                            VkFormat colour_format, std::uint32_t frames,
                            std::optional<render::Billboard> companion) {
    const terrain::StreamSettings settings;
    const std::vector<terrain::MaterialImage> materials = biome_materials();
    const BiomePalette palette = biome_palette();
    auto terrain = render::Terrain::create(
        device, colour_format, depth_format,
        {.materials = materials, .sea = true, .air = true, .canopy = render::trees_far});
    if (!terrain) {
        return failed("draw the ground", terrain.error());
    }
    auto ocean = render::Ocean::create(device, colour_format, depth_format, true, frames);
    if (!ocean) {
        return failed("draw the sea", ocean.error());
    }
    auto grass = render::Grass::create(device, colour_format, depth_format, palette);
    if (!grass) {
        return failed("grow the grass", grass.error());
    }
    const GrassBudget budget;
    auto bushes = render::Bushes::create(device, colour_format, depth_format, budget.tall_far);
    if (!bushes) {
        return failed("grow the bushes", bushes.error());
    }
    auto trees = render::Trees::create(device, colour_format, depth_format, render::forest_far);
    if (!trees) {
        return failed("grow the trees", trees.error());
    }
    auto forest = render::Forest::create(device, colour_format, depth_format, budget.forest_full,
                                         budget.forest_mid);
    if (!forest) {
        return failed("grow the forest", forest.error());
    }
    auto pool = render::TerrainPool::create(
        device, {.patches = settings.capacity, .frames = frames, .uploads = settings.uploads});
    if (!pool) {
        return failed("hold the ground", pool.error());
    }
    // Enough tiles for the plants' whole reach round the eye, within reason on a small planet.
    const auto tiles_for = [&](int level, double reach) {
        const double tile_span = (std::numbers::pi / 2.0) * world.geology().planet_radius() /
                                 static_cast<double>(std::uint32_t{1} << level);
        const double rings = (reach / tile_span) + 3.0;
        return static_cast<std::uint32_t>(
            std::clamp(std::numbers::pi * rings * rings, 64.0, 1024.0));
    };
    auto anchors = render::AnchorPool::create(
        device,
        {.tiles = tiles_for(anchor_tile_level, budget.tree_far), .frames = frames, .uploads = 4});
    if (!anchors) {
        return failed("hold the trees", anchors.error());
    }
    auto bush_pool = render::AnchorPool::create(
        device,
        {.tiles = tiles_for(bush_tile_level, budget.tall_far), .frames = frames, .uploads = 4});
    if (!bush_pool) {
        return failed("hold the bushes", bush_pool.error());
    }
    auto atmosphere = render::Atmosphere::create(device);
    if (!atmosphere) {
        return failed("draw the air", atmosphere.error());
    }
    auto sky = render::Sky::create(device, colour_format, depth_format, frames);
    if (!sky) {
        return failed("draw the sky", sky.error());
    }
    auto others = render::Capsules::create(device, colour_format, depth_format, true);
    if (!others) {
        return failed("draw other players", others.error());
    }
    auto weather = render::WeatherMaps::create(device, weather_map_size, frames);
    if (!weather) {
        return failed("hold the weather", weather.error());
    }
    auto clouds = render::Clouds::create(device, colour_format, jobs);
    if (!clouds) {
        return failed("draw the clouds", clouds.error());
    }
    auto precipitation = render::Precipitation::create(device, colour_format, depth_format);
    if (!precipitation) {
        return failed("draw the rain", precipitation.error());
    }
    auto glare = render::Glare::create(device, colour_format);
    if (!glare) {
        return failed("draw the sun's glare", glare.error());
    }
    auto timer = render::GpuTimer::create(device, frames);
    if (!timer) {
        return failed("time the GPU", timer.error());
    }
    return Scene{device,
                 world,
                 jobs,
                 calendar,
                 std::move(terrain).value(),
                 std::move(ocean).value(),
                 std::move(grass).value(),
                 std::move(bushes).value(),
                 std::move(trees).value(),
                 std::move(forest).value(),
                 std::move(pool).value(),
                 std::move(anchors).value(),
                 std::move(bush_pool).value(),
                 std::move(atmosphere).value(),
                 std::move(sky).value(),
                 std::move(others).value(),
                 std::move(weather).value(),
                 std::move(clouds).value(),
                 std::move(precipitation).value(),
                 std::move(glare).value(),
                 std::move(timer).value(),
                 std::move(companion),
                 frames};
}

Scene::Scene(const render::Device& device, const planet::World& world, jobs::JobSystem& jobs,
             const sim::Calendar& calendar, render::Terrain terrain, render::Ocean ocean,
             render::Grass grass, render::Bushes bushes, render::Trees trees, render::Forest forest,
             render::TerrainPool pool, render::AnchorPool anchors, render::AnchorPool bush_pool,
             render::Atmosphere atmosphere, render::Sky sky, render::Capsules others,
             render::WeatherMaps weather, render::Clouds clouds,
             render::Precipitation precipitation, render::Glare glare, render::GpuTimer timer,
             std::optional<render::Billboard> companion, std::uint32_t frames)
    : world_{&world}, jobs_{&jobs}, baker_{world, jobs}, terrain_{std::move(terrain)},
      ocean_{std::move(ocean)},
      shore_{world.geology().terrain(), world.geology().planet_radius(), jobs},
      wave_trains_{waves::planet_trains(hash_combine(world.geology().seed(), waves_salt))},
      grass_{std::move(grass)}, bushes_{std::move(bushes)}, trees_{std::move(trees)},
      forest_{std::move(forest)}, pool_{std::move(pool)}, anchor_pool_{std::move(anchors)},
      anchors_{biome_palette(), &jobs}, bush_pool_{std::move(bush_pool)},
      bush_anchors_{biome_palette(), &jobs}, transients_{device, frames},
      others_{std::move(others)}, companion_{std::move(companion)},
      atmosphere_{std::move(atmosphere)}, sky_{std::move(sky)}, weather_{std::move(weather)},
      clouds_{std::move(clouds)}, precipitation_{std::move(precipitation)},
      glare_{std::move(glare)}, timer_{std::move(timer)}, calendar_{calendar},
      selection_{ground_selection()} {}

Scene::~Scene() {
    (void)baker_.finish();
    (void)shore_.finish();
}

void Scene::finish_bakes() {
    anchors_.finish();
    bush_anchors_.finish();
    for (terrain::Patch& patch : baker_.finish()) {
        stream_.baked(std::move(patch));
    }
    if (auto depths = shore_.finish()) {
        shore_depths_ = std::move(depths);
    }
}

void Scene::fare(const protocol::Woodlands& told) {
    const std::vector<planet::CellId> changed = woodlands_.receive(told);
    if (changed.empty()) {
        return;
    }
    anchors_.fare(woodlands_.fates(), changed);
    bush_anchors_.fare(woodlands_.fates(), changed);
    // Bakes in flight read the fates they started with, and are dropped as stale on arrival.
    baker_.fates(woodlands_.fates());
    for (const planet::CellId cell : changed) {
        stream_.changed(cell);
    }
}

render::SkyView Scene::sky_view(const Shot& shot, render::CloudShade shade) {
    const DVec3 sun = calendar_.sun_direction(shot.time);
    const EyeLight light =
        light_at_eye(model_, shot.bend.eye_height * render::air_scale(model_.air(), shot.bend),
                     glm::dot(sun, shot.bend.up));
    const double wanted = exposure_for(under_cloud(light, shade));
    exposure_ = exposure_ ? adapted(*exposure_, wanted, shot.seconds) : wanted;
    return {.camera = shot.camera,
            .sun = sun,
            .bend = shot.bend,
            .exposure = *exposure_,
            .sky_light = light.sky,
            .shade = shade,
            .daylight = daylight_at(calendar_, shot.time, shot.bend.up)};
}

void Scene::grow_vegetation(render::RenderGraph& graph, const render::Frame& frame,
                            const Shot& shot, std::span<const terrain::StreamedPatch> patches,
                            std::span<const render::GraphUse> through_air,
                            const render::GraphTargets& drawn, const render::AirBindings& air,
                            DVec3 wind) {
    HOG_ZONE("vegetation");
    const std::vector<render::GrassDraw> lawn =
        grass_draws(slots_, patches, shot.camera.eye, shot.bend.radius);
    if (!lawn.empty()) {
        graph.add(
            {.name = "grass", .uses = {through_air.begin(), through_air.end()}, .targets = drawn},
            [&, lawn, wind](VkCommandBuffer commands) {
                grass_.draw(commands, frame.extent, pool_, lawn,
                            {.camera = shot.camera,
                             .bend = shot.bend,
                             .air = air,
                             .wind = wind,
                             .seconds = swaying_for_});
            });
    }
    const Sight sight =
        sight_of(shot.camera, static_cast<double>(frame.extent.width) /
                                  static_cast<double>(std::max(frame.extent.height, 1U)));
    const std::vector<render::StandDraw> shrubs =
        bush_anchors_.draws(sight, GrassBudget{}.tall_far);
    if (!shrubs.empty()) {
        graph.add(
            {.name = "bushes", .uses = {through_air.begin(), through_air.end()}, .targets = drawn},
            [&, shrubs, wind](VkCommandBuffer commands) {
                bushes_.draw(commands, frame.extent, bush_pool_, shrubs,
                             {.camera = shot.camera,
                              .bend = shot.bend,
                              .air = air,
                              .wind = wind,
                              .seconds = swaying_for_});
            });
    }
    // Both tree tiers draw from the same anchored stands: the stamps over whole tiles, holding off
    // where the grown forest covers, and the grown trees over the near blocks alone.
    const std::vector<render::StandDraw> stands = anchors_.draws(sight);
    if (!stands.empty()) {
        graph.add(
            {.name = "trees", .uses = {through_air.begin(), through_air.end()}, .targets = drawn},
            [&, stands, wind](VkCommandBuffer commands) {
                trees_.draw(commands, frame.extent, anchor_pool_, stands,
                            {.camera = shot.camera,
                             .bend = shot.bend,
                             .air = air,
                             .wind = wind,
                             .seconds = swaying_for_});
            });
    }
    const std::vector<render::StandDraw> woods = anchors_.forest_draws(sight);
    tree_counts_ = {
        .tiles = anchors_.size(), .stamped = anchors_in(stands), .grown = anchors_in(woods)};
    if (!woods.empty()) {
        graph.add(
            {.name = "forest", .uses = {through_air.begin(), through_air.end()}, .targets = drawn},
            [&, woods, wind](VkCommandBuffer commands) {
                forest_.draw(commands, frame.extent, anchor_pool_, woods,
                             {.camera = shot.camera,
                              .bend = shot.bend,
                              .air = air,
                              .wind = wind,
                              .seconds = swaying_for_});
            });
    }
}

void Scene::follow_shore(const Shot& shot) {
    HOG_ZONE("shore");
    shore_.follow(glm::normalize(shot.camera.eye.metres()));
    if (auto depths = shore_.collect()) {
        shore_depths_ = std::move(depths);
    }
}

Result<> Scene::give_shore(VkCommandBuffer commands, std::uint32_t slot) {
    if (!shore_depths_) {
        return {};
    }
    const terrain::ShoreDepths& depths = shore_depths_.value();
    if (auto updated = ocean_.update_shore(commands, slot, depths); !updated) {
        return updated;
    }
    shore_depths_.reset();
    return {};
}

namespace {

// What's drawn before the sea, copied for it to see through.
struct SeaCopies {
    render::GraphImage colour;
    render::GraphImage depth;
};

// Where the frame's target can be copied, sampled by the sea's pass, which `uses` them.
std::optional<SeaCopies> copy_behind_sea(render::RenderGraph& graph, render::GraphImage target,
                                         render::GraphImage depth, const render::Frame& frame,
                                         std::vector<render::GraphUse>& uses) {
    if (!frame.copyable) {
        return std::nullopt;
    }
    const auto copy_of = [&](VkFormat format, VkImageAspectFlags aspect) {
        return graph.transient(
            {.format = format,
             .extent = frame.extent,
             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             .aspect = aspect,
             .layers = 1,
             .levels = 1});
    };
    const SeaCopies copies{.colour = copy_of(frame.format, VK_IMAGE_ASPECT_COLOR_BIT),
                           .depth = copy_of(depth_format, VK_IMAGE_ASPECT_DEPTH_BIT)};
    graph.copy("behind sea", target, copies.colour);
    graph.copy("behind sea depth", depth, copies.depth);
    uses.push_back({.image = copies.colour, .use = render::image_use::sampled});
    uses.push_back({.image = copies.depth, .use = render::image_use::sampled});
    return copies;
}

// The deep water beneath the sea, unless it sees through what's drawn beneath it, as copied.
Result<> draw_deep(const render::Ocean& ocean, VkCommandBuffer commands, const render::Frame& frame,
                   const render::OceanView& sea) {
    if (frame.copyable) {
        return {};
    }
    return ocean.draw_deep(commands, frame.extent, sea, frame.slot);
}

std::optional<render::SeaBehind> seen_behind(const render::RenderGraph& graph,
                                             const std::optional<SeaCopies>& copies) {
    if (!copies) {
        return std::nullopt;
    }
    return render::SeaBehind{.colour = graph.view(copies->colour),
                             .depth = graph.view(copies->depth)};
}

} // namespace

terrain::StreamFrame Scene::stream_ground(const terrain::Viewpoint& seen) {
    for (terrain::Patch& patch : baker_.collect()) {
        stream_.baked(std::move(patch));
    }
    // All round the eye, so the ground is baked before it's turned to. Settled, the same view
    // selects the same again.
    if (!stream_.settled() || seen != selected_from_) {
        selected_ = terrain::select_patches(seen, seen.bend.radius, selection_,
                                            stream_.known_ground(), jobs_);
        selected_from_ = seen;
    }
    terrain::StreamFrame streamed = stream_.update(selected_);
    for (const planet::CellId cell : streamed.bakes) {
        baker_.start(cell);
    }
    return streamed;
}

Result<> Scene::record(render::Frame& frame, const Shot& shot) {
    HOG_ZONE("scene");
    const std::span<const render::GpuSpan> timed = timer_.read(frame.commands, frame.slot);
    timings_.assign(timed.begin(), timed.end());
    terrain::Viewpoint seen = viewpoint(shot.camera, frame.extent);
    seen.bend = shot.bend;
    const terrain::StreamFrame streamed = stream_ground(seen);
    // The ground into its slots, and the anchored trees and bushes round the eye, a few tiles a
    // frame.
    const GrassBudget budget;
    if (auto taken = slots_.take(pool_, frame.slot, streamed)
                         .and_then([&] {
                             return anchors_.update(anchor_pool_, frame.slot, *world_,
                                                    shot.camera.eye, budget.tree_far);
                         })
                         .and_then([&] {
                             return bush_anchors_.update(bush_pool_, frame.slot, *world_,
                                                         shot.camera.eye, budget.tall_far);
                         });
        !taken) {
        return taken;
    }
    follow_shore(shot);
    settled_ = streamed.bakes.empty() && streamed.arrived.empty() && streamed.evicted.empty() &&
               baker_.in_flight() == 0 && anchors_.settled() && bush_anchors_.settled() &&
               !shore_.in_flight() && !shore_depths_;
    const protocol::CellWeather here = weather_at_eye(shot);
    const DVec3 sun = calendar_.sun_direction(shot.time);
    const double sun_mu = glm::dot(sun, shot.bend.up);
    const render::CloudShade shade =
        render::under_mist(shade_at_eye(shot.bend, here), static_cast<double>(here.humidity),
                           static_cast<double>(here.fog_top), shot.bend.eye_height, sun_mu);
    const DVec3 drift = drift_at(shot.bend.up, here);
    const double falling = falling_at_eye(shot.bend, here);
    drifted_ = render::within_period(drifted_ + (drift * shot.seconds));
    gusts_drifted_ += render::gust_drift(drift * surface_wind, shot.bend.up) * shot.seconds;
    falling_for_ += shot.seconds;
    swaying_for_ += shot.seconds;
    const bool new_weather = shot.forecast != nullptr && shot.forecast->known() &&
                             weather_taken_ != shot.forecast->received();
    const bool cloudy = shot.forecast != nullptr && shot.forecast->known();
    const double faded = cloudy ? shot.forecast->faded(shot.now) : 1.0;
    // Taken as the passes are recorded, after the uploads: until the maps are first taken, a clear
    // sky.
    const auto weather = [&] { return weather_.bindings(faded); };
    render::SkyView sky = sky_view(shot, shade);
    sky.gusts_drifted = gusts_drifted_;
    sky.gusts_wind = glm::length(drift * surface_wind);
    sky.swaying_for = swaying_for_;
    sky.mist =
        render::mist_through(static_cast<double>(here.humidity), static_cast<double>(here.fog_top),
                             shot.bend.eye_height, sun_mu);
    sky.haze =
        render::haze_through(static_cast<double>(here.humidity), static_cast<double>(here.fog_top),
                             shot.bend.eye_height, sun_mu);
    if (cloudy) {
        sky.cloud_shadow = clouds_.shadow_image().view();
    }
    const render::AirBindings through = sky_.bindings(frame.slot, atmosphere_, sky.cloud_shadow);
    const auto clouds_view = [&] {
        return render::CloudsView{.camera = shot.camera,
                                  .bend = shot.bend,
                                  .air = through,
                                  .weather = weather(),
                                  .moved = drifted_};
    };

    render::RenderGraph graph;
    const render::GraphImage target = graph.import(frame.target, frame.view, frame.extent);
    const render::GraphImage depth =
        graph.transient({.format = depth_format,
                         .extent = frame.extent,
                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                         .layers = 1,
                         .levels = 1});
    // Made afresh each frame, so what the slot's last frame left in them isn't needed.
    const render::Image& sky_table = sky_.table(frame.slot);
    const render::Image& aerial_volume = sky_.aerial_volume(frame.slot);
    render::TrackedImage tracked_table{sky_table.handle(), VK_IMAGE_ASPECT_COLOR_BIT};
    render::TrackedImage tracked_aerial{aerial_volume.handle(), VK_IMAGE_ASPECT_COLOR_BIT};
    const render::GraphImage table =
        graph.import(tracked_table, sky_table.view(), sky_table.desc().extent);
    const render::GraphImage aerial =
        graph.import(tracked_aerial, aerial_volume.view(), aerial_volume.desc().extent);
    std::vector<render::GraphUse> through_air{{.image = table, .use = render::image_use::sampled},
                                              {.image = aerial, .use = render::image_use::sampled}};
    // Made afresh each frame too, when there's cloud to cast it.
    const render::Image& shadow_map = clouds_.shadow_image();
    render::TrackedImage tracked_shadow{shadow_map.handle(), VK_IMAGE_ASPECT_COLOR_BIT};
    std::optional<render::GraphImage> shadow;
    if (cloudy) {
        shadow = graph.import(tracked_shadow, shadow_map.view(), shadow_map.desc().extent);
        through_air.push_back({.image = *shadow, .use = render::image_use::sampled});
    }
    const render::GraphTargets drawn{.colour = {.image = target, .clear = std::nullopt},
                                     .depth = {{.image = depth, .clear = std::nullopt}}};

    graph.add({.name = "uploads", .uses = {}, .targets = std::nullopt},
              [&](VkCommandBuffer commands) -> Result<> {
                  if (new_weather) {
                      if (auto updated =
                              weather_.update(commands, frame.slot,
                                              std::as_bytes(std::span{shot.forecast->before()}),
                                              std::as_bytes(std::span{shot.forecast->after()}),
                                              std::as_bytes(std::span{shot.forecast->sky_before()}),
                                              std::as_bytes(std::span{shot.forecast->sky_after()}));
                          !updated) {
                          return updated;
                      }
                  }
                  return give_shore(commands, frame.slot)
                      .and_then([&] { return pool_.record_uploads(commands, frame.slot); })
                      .and_then([&] { return anchor_pool_.record_uploads(commands, frame.slot); })
                      .and_then([&] { return bush_pool_.record_uploads(commands, frame.slot); });
              });
    if (shadow) {
        graph.add({.name = "cloud shadow",
                   .uses = {{.image = *shadow, .use = render::image_use::storage}},
                   .targets = std::nullopt},
                  [&](VkCommandBuffer commands) { clouds_.shadow(commands, clouds_view()); });
    }
    std::vector<render::GraphUse> tables{{.image = table, .use = render::image_use::storage},
                                         {.image = aerial, .use = render::image_use::storage}};
    if (shadow) {
        tables.push_back({.image = *shadow, .use = render::image_use::sampled});
    }
    graph.add({.name = "sky tables", .uses = tables, .targets = std::nullopt},
              [&](VkCommandBuffer commands) {
                  render::SkyView weathered = sky;
                  weathered.weather = weather();
                  return sky_.record_tables(commands, frame.extent, frame.slot, atmosphere_,
                                            weathered);
              });
    graph.add({.name = "sky",
               .uses = through_air,
               .targets = {{.colour = {.image = target, .clear = cleared},
                            .depth = {{.image = depth, .clear = 0.0F}}}}},
              [&](VkCommandBuffer commands) {
                  sky_.draw(commands, frame.extent, frame.slot, atmosphere_, sky);
              });
    const waves::SeaState sea_state = sea_at_eye(shot);
    foam_drifted_ += waves::foam_drift(sea_state) * shot.seconds;
    const std::vector<waves::Wave> waves = waves::waves_at(wave_trains_, shot.bend.up, sea_state);
    render::DrawnWaves drawn_waves =
        render::drawn_waves_at(wave_trains_, waves, sea_state, shot.camera.eye, shot.bend,
                               shot.time, foam_drifted_, calendar_.day_length());
    drawn_waves.rain = {
        static_cast<float>(render::rain_ring_chance(falling, static_cast<double>(here.snow))),
        static_cast<float>(render::rain_ring_seconds(shot.time)), 0.0F, 0.0F};
    const render::OceanView sea{.camera = shot.camera,
                                .sun = sky.sun,
                                .bend = shot.bend,
                                .air = through,
                                .waves = drawn_waves};
    graph.add({.name = "foam", .uses = {}, .targets = std::nullopt}, [&](VkCommandBuffer commands) {
        return ocean_.step_foam(commands, sea, frame.slot, shot.time, waves::foam_drift(sea_state),
                                shot.still);
    });
    graph.add({.name = "ripples", .uses = {}, .targets = std::nullopt},
              [&](VkCommandBuffer commands) {
                  return ocean_.step_ripples(commands, sea, frame.slot, shot.time,
                                             waves::foam_drift(sea_state), shot.others);
              });
    graph.add({.name = "sea laid", .uses = {}, .targets = std::nullopt},
              [&](VkCommandBuffer commands) {
                  return ocean_.lay_water(commands, sea, frame.slot, frame.copyable);
              });
    graph.add({.name = "ground", .uses = through_air, .targets = drawn},
              [&](VkCommandBuffer commands) {
                  Result<> deep = draw_deep(ocean_, commands, frame, sea);
                  terrain_.draw(commands, frame.extent, pool_, slots_.draws(streamed.drawn),
                                {.camera = shot.camera,
                                 .sun = sky.sun,
                                 .bend = shot.bend,
                                 .air = through,
                                 .sea = ocean_.bindings(frame.slot)});
                  return deep;
              });
    if (grass_on_) {
        grow_vegetation(graph, frame, shot, streamed.drawn, through_air, drawn, through,
                        drift * surface_wind);
    }
    if (!shot.others.empty()) {
        graph.add({.name = "others", .uses = through_air, .targets = drawn},
                  [&](VkCommandBuffer commands) {
                      others_.draw(commands, frame.extent,
                                   {.camera = shot.camera,
                                    .sun = sky.sun,
                                    .bend = shot.bend,
                                    .air = through,
                                    .sea = ocean_.bindings(frame.slot)},
                                   shot.others);
                  });
    }
    if (companion_ && shot.companion) {
        graph.add(
            {.name = "companion", .uses = {}, .targets = drawn}, [&](VkCommandBuffer commands) {
                companion_->draw(commands, frame.extent, shot.camera, *shot.companion, shot.bend);
            });
    }
    // Over all that stands in the sea, each seen through the water to its surface, as copied.
    std::vector<render::GraphUse> sea_uses = through_air;
    const std::optional<SeaCopies> behind = copy_behind_sea(graph, target, depth, frame, sea_uses);
    graph.add({.name = "sea", .uses = sea_uses, .targets = drawn},
              [&, behind](VkCommandBuffer commands) {
                  render::OceanView looking = sea;
                  looking.behind = seen_behind(graph, behind);
                  return ocean_.draw(commands, frame.extent, looking, frame.slot);
              });
    if (render::precipitation_count(falling, static_cast<double>(here.snow)) > 0) {
        graph.add({.name = "rain", .uses = through_air, .targets = drawn},
                  [&](VkCommandBuffer commands) {
                      precipitation_.draw(commands, frame.extent,
                                          {.camera = shot.camera,
                                           .air = through,
                                           .up = shot.bend.up,
                                           .wind = drift * surface_wind,
                                           .falling = falling,
                                           .snow = static_cast<double>(here.snow),
                                           .seconds = falling_for_});
                  });
    }
    // The clouds and glare read what's drawn in front of them from depth, so they're drawn
    // without it.
    if (cloudy) {
        const VkExtent2D marched_extent = clouds_.march_extent(frame.extent);
        const auto marched_image = [&](VkFormat format) {
            return graph.transient(
                {.format = format,
                 .extent = marched_extent,
                 .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                 .layers = 1,
                 .levels = 1});
        };
        const render::GraphImage marched = marched_image(render::Clouds::marched_format);
        const render::GraphImage distances = marched_image(render::Clouds::distances_format);
        graph.add({.name = "cloud march",
                   .uses = {{.image = aerial, .use = render::image_use::sampled},
                            {.image = *shadow, .use = render::image_use::sampled},
                            {.image = depth, .use = render::image_use::sampled},
                            {.image = marched, .use = render::image_use::storage},
                            {.image = distances, .use = render::image_use::storage}},
                   .targets = std::nullopt},
                  // By value what's made in this block, as the passes run after it.
                  [&, marched, distances](VkCommandBuffer commands) {
                      return clouds_.march(commands, frame.extent, graph.view(depth),
                                           graph.view(marched), graph.view(distances),
                                           clouds_view());
                  });
        graph.add({.name = "clouds",
                   .uses = {{.image = aerial, .use = render::image_use::sampled},
                            {.image = *shadow, .use = render::image_use::sampled},
                            {.image = depth, .use = render::image_use::sampled},
                            {.image = distances, .use = render::image_use::sampled}},
                   .targets = {{.colour = {.image = target, .clear = std::nullopt},
                                .depth = std::nullopt}}},
                  [&, distances](VkCommandBuffer commands) {
                      clouds_.draw(commands, frame.extent, graph.view(depth), graph.view(distances),
                                   clouds_view());
                  });
    }
    const double height = shot.bend.eye_height * render::air_scale(model_.air(), shot.bend);
    // Dimmed by the cloud's shadow, through the air.
    std::vector<render::GraphUse> glare_uses = through_air;
    glare_uses.push_back({.image = depth, .use = render::image_use::sampled});
    graph.add(
        {.name = "glare",
         .uses = glare_uses,
         .targets = {{.colour = {.image = target, .clear = std::nullopt}, .depth = std::nullopt}}},
        [&](VkCommandBuffer commands) {
            glare_.draw(
                commands, frame.extent, graph.view(depth),
                {.camera = shot.camera,
                 .towards = sky.sun,
                 .irradiance = model_.transmittance(height, glm::dot(sky.sun, shot.bend.up)),
                 .exposure = sky.exposure,
                 .air = through,
                 .clouds = cloudy ? clouds_.marched() : VK_NULL_HANDLE});
        });
    if (new_weather) {
        weather_taken_ = shot.forecast->received();
    }
    return graph.execute(frame.commands,
                         {.slot = frame.slot, .timer = &timer_, .transients = &transients_});
}

Result<Picture> draw_still(render::Device& device, Scene& scene, const Shot& shot,
                           VkExtent2D extent, int most_frames) {
    auto target = render::make_image(
        device, {.format = still_format,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    if (!target) {
        return std::unexpected(std::move(target).error());
    }
    Shot still = shot;
    still.still = true;
    // At least once, though the scene settled drawing another shot.
    for (int drawn = 0; drawn < most_frames && (drawn == 0 || !scene.settled()); ++drawn) {
        Result<> recorded;
        if (auto submitted = device.submit_now([&](VkCommandBuffer commands) {
                render::Frame frame{.commands = commands,
                                    .target = {target->handle(), VK_IMAGE_ASPECT_COLOR_BIT},
                                    .view = target->view(),
                                    .format = still_format,
                                    .extent = extent,
                                    .slot = 0,
                                    .image = 0,
                                    .copyable = true};
                recorded = scene.record(frame, still);
            });
            !submitted) {
            return std::unexpected(std::move(submitted).error());
        }
        if (!recorded) {
            return std::unexpected(std::move(recorded).error());
        }
        scene.finish_bakes();
    }
    if (!scene.settled()) {
        return make_error(
            ErrorCode::OutOfRange,
            std::format("the ground was still arriving after {} frames", most_frames));
    }
    auto pixels = render::read_pixels(device, *target, render::image_use::colour_target);
    if (!pixels) {
        return std::unexpected(std::move(pixels).error());
    }
    const auto bytes = std::as_bytes(std::span{*pixels});
    return Picture{
        .width = extent.width, .height = extent.height, .texels = {bytes.begin(), bytes.end()}};
}

} // namespace hog::client
