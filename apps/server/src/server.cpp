#include <hog/app/server.hpp>
#include <hog/net/simulation.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/projection.hpp>
#include <hog/save/snapshot_store.hpp>
#include <hog/server/spatial_tracking.hpp>
#include <hog/server/survey.hpp>
#include <hog/server/weather_report.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hog::app {

namespace {

// Seconds; a 20 Hz tick has 50 ms to spare.
constexpr std::array tick_bounds{0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005,
                                 0.01,   0.025,   0.05,   0.1,   0.25,   1.0};

constexpr save::SectionId weather_section = save::section_id("WTHR");
constexpr save::SectionId wildlife_section = save::section_id("WILD");
constexpr save::SectionId forest_section = save::section_id("FRST");

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
      writer_(snapshots_, config.keep_snapshots),
      nearby_(
          std::min(planet::level_for(config.net.view.radius, planet.radius), planet::max_level)),
      persistence_(entities_), joined_(entities_, planet.radius, config.net.view),
      ground_(space_, planet_), ticks_(registry_.counter("hog_ticks_total", "Live ticks run.")),
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
      forest_steps_(registry_.counter("hog_forest_steps_total", "Forest steps taken.")),
      forest_step_seconds_(registry_.histogram("hog_forest_step_seconds",
                                               "Real time each forest step took.",
                                               {tick_bounds.begin(), tick_bounds.end()})),
      fires_started_(registry_.counter("hog_forest_fires_started_total",
                                       "Fires started by lightning or by hand.")),
      alight_km2_(registry_.counter("hog_forest_alight_km2_total",
                                    "Area of cells that caught fire, however they caught it.")),
      burning_cells_(registry_.gauge("hog_forest_burning_cells", "Forest cells alight.")),
      biomass_tonnes_(registry_.gauge("hog_forest_biomass_tonnes", "Living plants on the planet.")),
      connections_(registry_.gauge("hog_connections", "Players' connections open.")),
      shared_snapshots_(registry_.counter("hog_player_snapshots_total",
                                          "Snapshots sent to players of those near them.")),
      shared_bytes_(
          registry_.counter("hog_player_snapshot_bytes_total", "Bytes of players' snapshots.")),
      corrected_moves_(registry_.counter("hog_player_moves_corrected_total",
                                         "Players' moves refused, and sent back.")),
      reporter_(registry_) {
    persistence_.persist<server::Position>();
    server::track_positions(entities_, nearby_);
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

    const GameTime first = config.debug.start.value_or(now);
    if (auto started =
            server->start_simulation(*planet, loaded->snapshot ? loaded->snapshot->time : first);
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
        if (auto simulation = server->restore_simulation(*snapshot); !simulation) {
            return std::unexpected(simulation.error());
        }
        server->time_ = snapshot->time;
        server->tick_ = snapshot->tick;
        log::info(save_log, "restored {} entities at tick {}, {}", *restored,
                  snapshot->tick.value(), format_time(snapshot->time));
    } else {
        server->time_ = first;
        log::info(save_log, "no snapshot; starting a new world at {}", format_time(first));
    }
    if (server->wildlife_) {
        server->report_wildlife();
        server->recall_extinctions();
    }
    if (server->forest_) {
        server->report_forest();
    }
    server->open_probe();
    server->open_players();
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
    if (const auto& path = config_.forest.vegetation) {
        auto vegetation = sim::load_vegetation(*path);
        if (!vegetation) {
            return context(path->string(), vegetation.error());
        }
        const sim::ForestConfig forest{
            .level = weather.level,
            .step = config_.forest.step,
            .planet_radius = shape.radius,
            .seed = shape.seed,
            .vegetation = *vegetation,
        };
        forest_ =
            std::make_unique<sim::Forest>(forest, sim::soil_for(weather_->grid(), planet_), at);
        log::info(server_log, "forest: {:.0f} Mt of plants", forest_->total() / 1e9);
    }
    return {};
}

void Server::simulate(GameTime from, GameDuration step) {
    entities_.progress(std::chrono::duration<float>(step).count());
    const GameTime to = from + step;
    // Wildlife and forest steps see the weather as it was when they fell due.
    for (auto due = next_due(); due && *due <= to; due = next_due()) {
        advance_weather(*due);
        if (wildlife_ && wildlife_->next_step() == *due) {
            step_wildlife(*due);
        }
        if (forest_ && forest_->next_step() == *due) {
            step_forest(*due);
        }
    }
    advance_weather(to);
}

Result<void> Server::restore_simulation(const save::Snapshot& snapshot) {
    if (auto weather = restore_part(*weather_, snapshot, weather_section, "weather"); !weather) {
        return weather;
    }
    if (wildlife_) {
        if (auto wildlife = restore_part(*wildlife_, snapshot, wildlife_section, "wildlife");
            !wildlife) {
            return wildlife;
        }
    }
    if (forest_) {
        return restore_part(*forest_, snapshot, forest_section, "forest");
    }
    return {};
}

std::optional<GameTime> Server::next_due() const {
    std::optional<GameTime> due;
    if (wildlife_) {
        due = wildlife_->next_step();
    }
    if (forest_) {
        due = due ? std::min(*due, forest_->next_step()) : forest_->next_step();
    }
    return due;
}

void Server::step_wildlife(GameTime at) {
    const MonotonicClock::time_point started = MonotonicClock::now();
    wildlife_->advance_to(at, sim::conditions_of(*weather_), jobs_);
    wildlife_steps_.add(1);
    wildlife_step_seconds_.observe(
        std::chrono::duration<double>(MonotonicClock::now() - started).count());
    report_wildlife();
}

void Server::step_forest(GameTime at) {
    const MonotonicClock::time_point started = MonotonicClock::now();
    forest_->advance_to(at, sim::forest_conditions_of(*weather_), jobs_);
    forest_steps_.add(1);
    forest_step_seconds_.observe(
        std::chrono::duration<double>(MonotonicClock::now() - started).count());
    report_forest();
}

void Server::report_forest() {
    const sim::FireReport fires = forest_->drain_fires();
    fires_started_.add(fires.started);
    alight_left_ += fires.alight;
    const double whole = std::floor(alight_left_);
    alight_km2_.add(static_cast<std::uint64_t>(whole));
    alight_left_ -= whole;
    burning_cells_.set(static_cast<double>(forest_->burning()));
    biomass_tonnes_.set(forest_->total() / 1e3);
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

LayerSources Server::layer_sources() const {
    return LayerSources{.planet = planet_,
                        .weather = *weather_,
                        .wildlife = wildlife_.get(),
                        .forest = forest_.get()};
}

void Server::open_players() {
    const std::optional<std::uint16_t> port = config_.net.port;
    if (!port) {
        return;
    }
    if (!config_.net.simulate.off()) {
        net::simulate(config_.net.simulate);
        log::warn(server_log, "simulating a bad network: {}", net::describe(config_.net.simulate));
    }
    if (const auto& key_file = config_.net.login_key) {
        auto key = login::load_public_key(*key_file);
        if (!key) {
            log::error(server_log,
                       "no players' port without the login service's key: {}; "
                       "hog-login makes it on its first run",
                       key.error());
            return;
        }
        login_key_ = *key;
    }
    auto listening = net::Server::listen(*port);
    if (!listening) {
        log::warn(server_log, "no players' port: {}", listening.error());
        return;
    }
    players_ = std::move(*listening);
    log::info(server_log, "players connect on port {}{}", *port,
              login_key_ ? ", logged in" : ", anyone at all");
}

void Server::serve_players() {
    if (!players_) {
        return;
    }
    players_->poll(player_events_);
    for (const net::Event& event : player_events_) {
        if (const auto* connected = std::get_if<net::Connected>(&event)) {
            log::info(server_log, "connection {} opened",
                      std::to_underlying(connected->connection));
            handshakes_.emplace(connected->connection,
                                protocol::Handshake{MonotonicClock::now(), login_key_});
        } else if (const auto* message = std::get_if<net::Message>(&event)) {
            const auto shaking = handshakes_.find(message->from());
            if (shaking != handshakes_.end()) {
                answer(message->from(),
                       shaking->second.received(
                           message->bytes(),
                           std::chrono::floor<std::chrono::seconds>(WallClock::now())));
            } else {
                heard(message->from(), message->bytes());
            }
        } else if (const auto* lost = std::get_if<net::Disconnected>(&event)) {
            log::info(server_log, "connection {} closed: {}", std::to_underlying(lost->connection),
                      lost->reason);
            handshakes_.erase(lost->connection);
            joined_.leave(lost->connection);
        }
    }
    const MonotonicClock::time_point now = MonotonicClock::now();
    std::vector<std::pair<net::ConnectionId, protocol::Answer>> late;
    for (const auto& [connection, handshake] : handshakes_) {
        if (auto waited = handshake.waited(now)) {
            late.emplace_back(connection, std::move(*waited));
        }
    }
    for (const auto& [connection, waited] : late) {
        answer(connection, waited);
    }
    connections_.set(static_cast<double>(handshakes_.size() + joined_.size()));
}

void Server::answer(net::ConnectionId connection, const protocol::Answer& answer) {
    if (!players_) {
        return;
    }
    if (answer.reply) {
        if (auto sent = players_->send(connection, net::Delivery::Reliable,
                                       protocol::encode(*answer.reply));
            !sent) {
            log::warn(server_log, "couldn't answer connection {}: {}",
                      std::to_underlying(connection), sent.error());
        }
    }
    if (answer.close) {
        log::info(server_log, "connection {} refused: {}", std::to_underlying(connection),
                  answer.reason);
        players_->disconnect(connection, answer.reason);
        handshakes_.erase(connection);
        return;
    }
    if (const auto shaking = handshakes_.find(connection);
        shaking != handshakes_.end() && shaking->second.welcomed()) {
        const auto& player = shaking->second.player();
        log::info(server_log, "connection {} welcomed{}", std::to_underlying(connection),
                  player ? std::format(": {}", player->name) : std::string{});
        joined_.join(connection, player ? player->name
                                        : std::format("player {}", std::to_underlying(connection)));
        handshakes_.erase(shaking);
        send_weather(connection);
    }
}

void Server::heard(net::ConnectionId connection, std::span<const std::byte> bytes) {
    const auto message = protocol::decode(bytes);
    if (!message) {
        log::debug(server_log, "connection {} sent something unreadable: {}",
                   std::to_underlying(connection), message.error());
    } else if (const auto* moved = std::get_if<protocol::Moved>(&*message)) {
        const server::MoveResult result =
            joined_.move(connection, *moved, MonotonicClock::now(), space_);
        if (result.correction && players_) {
            log::debug(server_log, "connection {} moved {}; sent back",
                       std::to_underlying(connection), server::describe(result.ruling));
            corrected_moves_.add();
            if (auto sent = players_->send(connection, net::Delivery::Reliable,
                                           protocol::encode(*result.correction));
                !sent) {
                log::debug(server_log, "couldn't correct connection {}: {}",
                           std::to_underlying(connection), sent.error());
            }
        }
    } else if (const auto* ack = std::get_if<protocol::Ack>(&*message)) {
        joined_.acknowledged(connection, ack->tick);
    } else {
        log::debug(server_log, "connection {} sent a message out of place",
                   std::to_underlying(connection));
    }
}

void Server::share() {
    if (!players_) {
        return;
    }
    if (auto kept = ground_.keep(joined_.placed()); !kept) {
        log::warn(server_log, "couldn't keep the ground near players: {}", kept.error());
    }
    joined_.share(
        nearby_, [this](net::ConnectionId connection, const protocol::Snapshot& snapshot) {
            const std::vector<std::byte> bytes = protocol::encode(snapshot);
            if (auto sent = players_->send(connection, net::Delivery::Unreliable, bytes); !sent) {
                log::debug(server_log, "couldn't send connection {} a snapshot: {}",
                           std::to_underlying(connection), sent.error());
                return;
            }
            shared_snapshots_.add();
            shared_bytes_.add(bytes.size());
        });
}

void Server::tell_weather(std::int64_t scale) {
    if (!players_ || weather_told_at_ == weather_->time()) {
        return;
    }
    weather_told_at_ = weather_->time();
    weather_told_ = protocol::encode(server::weather_report(
        weather_->grid(), weather_->fields(), weather_->ground(), weather_->config(),
        std::chrono::duration_cast<std::chrono::milliseconds>(weather_->config().step) / scale));
    for (const net::ConnectionId connection : joined_.connections()) {
        send_weather(connection);
    }
}

void Server::send_weather(net::ConnectionId connection) {
    // Players joining before the weather is first told are sent it with everyone else.
    if (!players_ || weather_told_.empty()) {
        return;
    }
    if (auto sent = players_->send(connection, net::Delivery::Reliable, weather_told_); !sent) {
        log::debug(server_log, "couldn't send connection {} the weather: {}",
                   std::to_underlying(connection), sent.error());
    }
}

void Server::open_probe() {
    const std::optional<std::uint16_t> port = config_.debug.port;
    if (!port) {
        return;
    }
    probe::Welcome welcome{.version = probe::protocol_version,
                           .world_id = world_id_,
                           .level = static_cast<std::uint8_t>(weather_->config().level),
                           .planet_radius = weather_->config().planet_radius,
                           .layers = layer_catalogue(layer_sources()),
                           .controllable = config_.debug.time_control,
                           .day_length = weather_->config().day,
                           .axial_tilt = weather_->config().axial_tilt};
    const std::size_t layers = welcome.layers.size();
    const auto survey = [&caves = planet_.caves(), radius = welcome.planet_radius](
                            const probe::Survey& asked, jobs::JobSystem& jobs) {
        return server::survey_ground(caves, radius, asked, jobs);
    };
    auto started = server::ProbeService::start(*port, std::move(welcome), survey);
    if (!started) {
        log::warn(server_log, "no debug port: {}", started.error());
        return;
    }
    probe_ = std::move(*started);
    log::info(server_log, "debug tools on 127.0.0.1:{}, {} layers{}", probe_->port(), layers,
              config_.debug.time_control ? ", time control on" : "");
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
    if (forest_) {
        snapshot.add(save::Section{
            .id = forest_section, .version = sim::Forest::save_version, .bytes = forest_->save()});
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
