#include <hog/app/server.hpp>
#include <hog/planet/projection.hpp>
#include <hog/save/snapshot_store.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <utility>

namespace hog::app {

namespace {

// Seconds; a 20 Hz tick has 50 ms to spare.
constexpr std::array tick_bounds{0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005,
                                 0.01,   0.025,   0.05,   0.1,   0.25,   1.0};

constexpr save::SectionId weather_section = save::section_id("WTHR");
constexpr save::SectionId wildlife_section = save::section_id("WILD");

[[nodiscard]] std::string format_time(GameTime time) {
    return std::format("{:%FT%T}Z",
                       std::chrono::sys_seconds{
                           std::chrono::floor<std::chrono::seconds>(time.time_since_epoch())});
}

[[nodiscard]] std::unexpected<Error> context(std::string_view what, const Error& error) {
    return make_error(error.code, std::format("{}: {}", what, error.message));
}

// A part of the simulation whose section can't be read starts over, unless a newer build wrote
// it.
template <typename Part>
[[nodiscard]] Result<void> restore_part(Part& part, const save::Snapshot& snapshot,
                                        save::SectionId id, std::string_view what) {
    const save::Section* section = snapshot.find(id);
    if (section == nullptr) {
        return {};
    }
    auto restored = part.load(section->version, section->bytes);
    if (!restored && restored.error().code == ErrorCode::Unsupported) {
        return context(what, restored.error());
    }
    if (!restored) {
        log::warn(save_log, "{} starts over: {}", what, restored.error().message);
    }
    return {};
}

void mirror(metrics::Counter& counter, std::uint64_t total, std::uint64_t& mirrored) {
    counter.add(total - mirrored);
    mirrored = total;
}

} // namespace

Server::Server(Opened /*unused*/, const server::ServerConfig& config,
               const planet::PlanetConfig& planet, store::SqliteSnapshotStore snapshots,
               store::SqliteChangeStore changes)
    : config_(config), world_id_(planet::world_id(planet)), planet_(planet),
      changes_(std::move(changes)), snapshots_(std::move(snapshots)),
      writer_(snapshots_, config.keep_snapshots), persistence_(entities_),
      ticks_(registry_.counter("hog_ticks_total", "Live ticks run.")),
      tick_seconds_(registry_.histogram("hog_tick_seconds", "Real time each live tick took.",
                                        {tick_bounds.begin(), tick_bounds.end()})),
      slipped_(registry_.counter("hog_ticks_slipped_total",
                                 "Ticks dropped because the server fell behind real time.")),
      catch_up_ticks_(
          registry_.counter("hog_catch_up_ticks_total", "Ticks run to catch up to now.")),
      persistent_entities_(registry_.gauge("hog_persistent_entities", "Entities that are saved.")),
      snapshots_written_(registry_.counter("hog_snapshots_written_total", "Snapshots written.")),
      snapshots_replaced_(registry_.counter("hog_snapshots_replaced_total",
                                            "Snapshots replaced by a newer one before written.")),
      snapshots_failed_(
          registry_.counter("hog_snapshots_failed_total", "Snapshots that failed to write.")),
      snapshots_skipped_(registry_.counter("hog_snapshots_skipped_total",
                                           "Damaged snapshots passed over when loading.")),
      weather_steps_(registry_.counter("hog_weather_steps_total", "Weather steps taken.")),
      weather_step_seconds_(registry_.histogram("hog_weather_step_seconds",
                                                "Real time each weather step took.",
                                                {tick_bounds.begin(), tick_bounds.end()})),
      wildlife_steps_(registry_.counter("hog_wildlife_steps_total", "Wildlife steps taken.")),
      wildlife_step_seconds_(registry_.histogram("hog_wildlife_step_seconds",
                                                 "Real time each wildlife step took.",
                                                 {tick_bounds.begin(), tick_bounds.end()})),
      extinct_species_(registry_.gauge("hog_wildlife_extinct_species",
                                       "Species gone from everywhere, waiting to come back.")),
      ecology_events_{
          &registry_.counter("hog_wildlife_collapses_total",
                             "Regions where a species fell far below its baseline."),
          &registry_.counter("hog_wildlife_overpopulations_total",
                             "Regions where a species rose far above its baseline."),
          &registry_.counter("hog_wildlife_local_extinctions_total",
                             "Regions a species died out of."),
          &registry_.counter("hog_wildlife_returns_total",
                             "Species back where they had died out, or anywhere after extinction."),
          &registry_.counter("hog_wildlife_extinctions_total", "Species that died out everywhere."),
      },
      reporter_(registry_) {
    persistence_.persist<server::Position>();
}

Server::~Server() = default;

Result<std::unique_ptr<Server>> Server::open(const server::ServerConfig& config, GameTime now) {
    const auto planet = planet::load_planet_config(config.planet);
    if (!planet) {
        return std::unexpected(planet.error());
    }
    if (config.world.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(config.world.parent_path(), error);
        if (error) {
            return make_error(ErrorCode::Io,
                              std::format("cannot create {}: {}",
                                          config.world.parent_path().string(), error.message()));
        }
    }
    const std::string world_file = config.world.string();
    auto snapshots = store::SqliteSnapshotStore::open(config.world);
    if (!snapshots) {
        return context(world_file, snapshots.error());
    }
    auto changes = store::SqliteChangeStore::open(config.world);
    if (!changes) {
        return context(world_file, changes.error());
    }
    auto loaded = save::load_newest(*snapshots, planet::world_id(*planet));
    if (!loaded) {
        return context(world_file, loaded.error());
    }
    const auto stored_changes = changes->load();
    if (!stored_changes) {
        return context(world_file, stored_changes.error());
    }

    auto server = std::make_unique<Server>(Opened{}, config, *planet, std::move(*snapshots),
                                           std::move(*changes));
    if (auto replayed = server->planet_.replay(*stored_changes); !replayed) {
        return context("planet changes", replayed.error());
    }
    log::info(server_log, "world {:016x} in {}: {} planet changes", server->world_id_, world_file,
              stored_changes->size());

    if (auto started =
            server->start_simulation(*planet, loaded->snapshot ? loaded->snapshot->time : now);
        !started) {
        return std::unexpected(started.error());
    }

    for (const Error& skipped : loaded->skipped) {
        log::warn(save_log, "skipped damaged {}", skipped.message);
    }
    server->snapshots_skipped_.add(loaded->skipped.size());
    if (const auto& snapshot = loaded->snapshot) {
        const auto restored = server->persistence_.restore(*snapshot);
        if (!restored) {
            return context("snapshot", restored.error());
        }
        if (const auto weather =
                restore_part(*server->weather_, *snapshot, weather_section, "weather");
            !weather) {
            return std::unexpected(weather.error());
        }
        if (server->wildlife_) {
            if (const auto wildlife =
                    restore_part(*server->wildlife_, *snapshot, wildlife_section, "wildlife");
                !wildlife) {
                return std::unexpected(wildlife.error());
            }
        }
        server->time_ = snapshot->time;
        server->tick_ = snapshot->tick;
        log::info(save_log, "restored {} entities at tick {}, {}", *restored,
                  snapshot->tick.value(), format_time(snapshot->time));
    } else {
        server->time_ = now;
        log::info(save_log, "no snapshot; starting a new world at {}", format_time(now));
    }
    if (server->wildlife_) {
        server->report_wildlife();
        server->recall_extinctions();
    }
    return server;
}

Result<void> Server::start_simulation(const planet::PlanetConfig& shape, GameTime at) {
    const sim::WeatherConfig weather{
        .level = std::min(planet::level_for(config_.weather.cell_size, shape.radius),
                          sim::CellGrid::max_level),
        .step = config_.weather.step,
        .day = config_.weather.day,
        .axial_tilt = shape.axial_tilt,
        .planet_radius = shape.radius,
        .seed = shape.seed,
    };
    weather_ = std::make_unique<sim::Weather>(
        weather, sim::ground_for(sim::CellGrid{weather.level}, planet_), at);
    log::info(server_log, "weather grid at level {}: {} cells", weather.level,
              weather_->grid().size());
    if (const auto& path = config_.wildlife.ecology) {
        auto ecology = sim::load_ecology(*path);
        if (!ecology) {
            return context(path->string(), ecology.error());
        }
        sim::WildlifeConfig wildlife{
            .level = weather.level,
            .step = config_.wildlife.step,
            .planet_radius = shape.radius,
            .seed = shape.seed,
            .ecology = std::move(*ecology),
        };
        const std::size_t species = wildlife.ecology.species.size();
        wildlife_ = std::make_unique<sim::Wildlife>(
            std::move(wildlife), sim::habitat_for(weather_->grid(), planet_), at);
        log::info(server_log, "wildlife: {} species", species);
    }
    return {};
}

void Server::simulate(GameTime from, GameDuration step) {
    entities_.progress(std::chrono::duration<float>(step).count());
    const GameTime to = from + step;
    // Wildlife steps see the weather as it was when they fell due.
    while (wildlife_ && wildlife_->next_step() <= to) {
        const GameTime at = wildlife_->next_step();
        advance_weather(at);
        const MonotonicClock::time_point started = MonotonicClock::now();
        wildlife_->advance_to(at, sim::conditions_of(*weather_), jobs_);
        wildlife_steps_.add(1);
        wildlife_step_seconds_.observe(
            std::chrono::duration<double>(MonotonicClock::now() - started).count());
        report_wildlife();
    }
    advance_weather(to);
}

namespace {

std::string describe(const sim::EcologyEvent& event, const sim::Species& species) {
    using Kind = sim::EcologyEvent::Kind;
    const std::string where = event.region == sim::EcologyEvent::everywhere
                                  ? "everywhere"
                                  : std::format("in region {}", event.region);
    std::string hunted;
    for (const sim::Contributor& one : event.hunters) {
        hunted += std::format("{} {} ({:.0f})", hunted.empty() ? ", hunted by" : ",", one.hunter,
                              one.kills);
    }
    switch (event.kind) {
    case Kind::Collapse:
        return std::format("{} collapsed {} to {:.0f}, from a usual {:.0f}{}", species.name, where,
                           event.count, event.baseline, hunted);
    case Kind::Overpopulation:
        return std::format("{} overran {} with {:.0f}, from a usual {:.0f}{}", species.name, where,
                           event.count, event.baseline, hunted);
    case Kind::LocalExtinction:
        return std::format("{} died out {}{}", species.name, where, hunted);
    case Kind::Return:
        return std::format("{} is back {}", species.name, where);
    case Kind::Extinction:
        return std::format("{} died out everywhere{}{}", species.name,
                           species.recovery.after_extinction == sim::Recovery::After::Admin
                               ? " and waits to be reintroduced"
                               : "",
                           hunted);
    }
    return {};
}

} // namespace

void Server::report_wildlife() {
    using Kind = sim::EcologyEvent::Kind;
    const auto& species = wildlife_->config().ecology.species;
    for (const sim::EcologyEvent& event : wildlife_->drain_events()) {
        ecology_events_.at(static_cast<std::size_t>(event.kind))->add();
        const std::string message = describe(event, species[event.species]);
        if (event.kind == Kind::Return || event.kind == Kind::Overpopulation) {
            log::info(wildlife_log, "{}", message);
        } else {
            log::warn(wildlife_log, "{}", message);
        }
    }
    std::size_t extinct = 0;
    for (std::uint32_t kind = 0; kind < species.size(); ++kind) {
        extinct += wildlife_->presence(kind).extinct ? 1U : 0U;
    }
    extinct_species_.set(static_cast<double>(extinct));
}

void Server::recall_extinctions() const {
    const auto& species = wildlife_->config().ecology.species;
    for (std::uint32_t kind = 0; kind < species.size(); ++kind) {
        if (wildlife_->presence(kind).extinct) {
            log::warn(wildlife_log, "{} is extinct{}", species[kind].name,
                      species[kind].recovery.after_extinction == sim::Recovery::After::Admin
                          ? " and waits to be reintroduced"
                          : "");
        }
    }
}

void Server::advance_weather(GameTime to) {
    const MonotonicClock::time_point started = MonotonicClock::now();
    if (const std::uint64_t steps = weather_->advance_to(to, jobs_); steps > 0) {
        weather_steps_.add(steps);
        weather_step_seconds_.observe(
            std::chrono::duration<double>(MonotonicClock::now() - started).count() /
            static_cast<double>(steps));
    }
}

void Server::submit() {
    save::Snapshot snapshot{.world = world_id_, .time = time_, .tick = tick_, .sections = {}};
    persistence_.store(snapshot);
    snapshot.add(save::Section{
        .id = weather_section, .version = sim::Weather::save_version, .bytes = weather_->save()});
    if (wildlife_) {
        snapshot.add(save::Section{.id = wildlife_section,
                                   .version = sim::Wildlife::save_version,
                                   .bytes = wildlife_->save()});
    }
    writer_.submit(std::move(snapshot));
}

Result<void> Server::save() {
    submit();
    auto flushed = writer_.flush();
    if (!flushed) {
        log::error(save_log, "saving failed: {}", flushed.error().message);
    }
    return flushed;
}

void Server::report(std::uint64_t slipped) {
    const server::SnapshotWriter::Stats writes = writer_.stats();
    if (writes.failed > mirrored_writes_.failed) {
        log::error(save_log, "{} snapshots failed to write",
                   writes.failed - mirrored_writes_.failed);
    }
    mirror(snapshots_written_, writes.written, mirrored_writes_.written);
    mirror(snapshots_replaced_, writes.replaced, mirrored_writes_.replaced);
    mirror(snapshots_failed_, writes.failed, mirrored_writes_.failed);
    mirror(slipped_, slipped, mirrored_slips_);
    persistent_entities_.set(static_cast<double>(persistence_.size()));
    reporter_.report(metrics_log);
}

} // namespace hog::app
