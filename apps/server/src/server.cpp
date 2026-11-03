#include <hog/app/server.hpp>
#include <hog/chat/filter.hpp>
#include <hog/core/profile.hpp>
#include <hog/globe/brick_archive.hpp>
#include <hog/globe/material.hpp>
#include <hog/net/simulation.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/save/snapshot_store.hpp>
#include <hog/server/grain_sharing.hpp>
#include <hog/globe/grip.hpp>
#include <hog/server/ground_wetting.hpp>
#include <hog/server/spatial_tracking.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hog::app {

namespace {

// Seconds; a 20 Hz tick has 50 ms to spare.
constexpr std::array tick_bounds{0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005,
                                 0.01,   0.025,   0.05,   0.1,   0.25,   1.0};

[[nodiscard]] std::string format_time(GameTime time) {
    return std::format("{:%FT%T}Z",
                       std::chrono::sys_seconds{
                           std::chrono::floor<std::chrono::seconds>(time.time_since_epoch())});
}

[[nodiscard]] std::unexpected<Error> context(std::string_view what, const Error& error) {
    return make_error(error.code, std::format("{}: {}", what, error.message));
}

void mirror(metrics::Counter& counter, std::uint64_t total, std::uint64_t& mirrored) {
    counter.add(total - mirrored);
    mirrored = total;
}

} // namespace

Server::Server(Opened /*unused*/, const server::ServerConfig& config, const globe::Planet& globe,
               store::SqliteSnapshotStore snapshots, store::SqlitePlayerStore places,
               store::SqliteBrickArchive bricks)
    : config_(config), world_id_(globe::world_id(globe)), generator_(globe),
      voxels_(generator_.layers()), globe_(generator_, voxels_), flow_(globe_),
      bricks_(std::move(bricks)), bricks_shared_(voxels_), snapshots_(std::move(snapshots)),
      places_(std::move(places)), writer_(snapshots_, config.keep_snapshots),
      nearby_(std::min(planet::level_for(config.net.view.radius, globe.radius), planet::max_level)),
      persistence_(entities_), joined_(entities_, globe.radius, config.net.view),
      ground_(space_, globe_), ticks_(registry_.counter("hog_ticks_total", "Live ticks run.")),
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
      connections_(registry_.gauge("hog_connections", "Players' connections open.")),
      shared_snapshots_(registry_.counter("hog_player_snapshots_total",
                                          "Snapshots sent to players of those near them.")),
      shared_bytes_(
          registry_.counter("hog_player_snapshot_bytes_total", "Bytes of players' snapshots.")),
      corrected_moves_(registry_.counter("hog_player_moves_corrected_total",
                                         "Players' moves refused, and sent back.")),
      chat_lines_(registry_.counter("hog_chat_lines_total", "Lines of chat sent to players.")),
      hateful_lines_(registry_.counter("hog_chat_hateful_lines_total",
                                       "Lines of hate speech reported, sent on as small talk.")),
      bricks_sent_(
          registry_.counter("hog_bricks_sent_total", "Bricks of the voxel world sent to players.")),
      reporter_(registry_) {
    persistence_.persist<server::Position>();
    server::track_positions(entities_, nearby_);
}

Server::~Server() = default;

Result<std::unique_ptr<Server>> Server::open(const server::ServerConfig& config, GameTime now) {
    auto planet = planet::load_planet_config(config.planet);
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
    auto places = store::SqlitePlayerStore::open(config.world);
    if (!places) {
        return context(world_file, places.error());
    }
    auto bricks = store::SqliteBrickArchive::open(config.world);
    if (!bricks) {
        return context(world_file, bricks.error());
    }
    // The voxel world is Earth-like at the planet's seed and size, as the client's is by default.
    globe::Planet globe;
    globe.seed = planet->seed;
    globe.radius = planet->radius;
    if (auto valid = globe::validate(globe); !valid) {
        return std::unexpected(std::move(valid).error());
    }
    auto loaded = save::load_newest(*snapshots, globe::world_id(globe));
    if (!loaded) {
        return context(world_file, loaded.error());
    }

    auto server = std::make_unique<Server>(Opened{}, config, globe, std::move(*snapshots),
                                           std::move(*places), std::move(*bricks));
    auto voxels = globe::load(server->generator_.layers(), server->bricks_);
    if (!voxels) {
        return context("voxels", voxels.error());
    }
    server->voxels_ = std::move(*voxels);
    // Loaded bricks are told to players as they join, not as changes.
    server->bricks_shared_ = server::BrickSharing{server->voxels_};
    log::info(server_log, "world {:016x} in {}: {} bricks changed", server->world_id_, world_file,
              server->voxels_.size());

    const GameTime first = config.debug.start.value_or(now);
    for (const Error& skipped : loaded->skipped) {
        log::warn(save_log, "skipped damaged {}", skipped.message);
    }
    server->snapshots_skipped_.add(loaded->skipped.size());
    if (const auto& snapshot = loaded->snapshot) {
        const auto restored = server->persistence_.restore(*snapshot);
        if (!restored) {
            return context("snapshot", restored.error());
        }
        server->time_ = snapshot->time;
        server->tick_ = snapshot->tick;
        log::info(save_log, "restored {} entities at tick {}, {}", *restored,
                  snapshot->tick.value(), format_time(snapshot->time));
    } else {
        server->time_ = first;
        log::info(save_log, "no snapshot; starting a new world at {}", format_time(first));
    }
    if (config.weather.on) {
        server->weather_.emplace(server->generator_, planet->day_length, planet->axial_tilt,
                                 server->time_,
                                 server::WeatherSettings{.level = config.weather.level,
                                                         .layers = config.weather.layers,
                                                         .spin_up = config.weather.spin_up});
        const save::Section* kept =
            loaded->snapshot ? loaded->snapshot->find(server::WeatherKeeper::section) : nullptr;
        if (kept != nullptr && kept->version == server::WeatherKeeper::section_version) {
            if (auto resumed = server->weather_->restore(kept->bytes, server->time_); !resumed) {
                log::warn(save_log, "starting the air afresh: {}", resumed.error().message);
            } else {
                log::info(save_log, "restored the air");
            }
        }
    }
    server->open_players();
    return server;
}

void Server::simulate(GameTime from, GameDuration step) {
    HOG_ZONE("simulate");
    entities_.progress(std::chrono::duration<float>(step).count());
    flow_.step(std::chrono::duration<double>(step).count());
    // Ground changed anywhere is looked at for support in time, a brick a second, not only where
    // someone is digging.
    swept_ += step;
    if (swept_ >= std::chrono::seconds{1}) {
        flow_.sweep(1, std::chrono::duration<double>(swept_).count());
        swept_ = {};
    }
    wet_ground(step);
    if (weather_) {
        // A new weather is as old as the air's interval of game time, which the clock may hurry.
        const auto fade = std::max(
            std::chrono::milliseconds{500},
            std::chrono::duration_cast<std::chrono::milliseconds>(
                server::WeatherSettings{}.interval / std::max<std::uint32_t>(clock_scale_, 1)));
        if (const auto made = weather_->poll(from + step, fade)) {
            tell_weather(*made);
        }
    }
}

void Server::wet_ground(GameDuration step) {
    // Game seconds between wettings: wetting costs a pass over the ground and rewrites its
    // bricks, and ground does not change enough in a minute to show.
    constexpr GameDuration interval = std::chrono::minutes{1};
    unwetted_ += step;
    if (!weather_ || unwetted_ < interval) {
        return;
    }
    std::vector<server::GroundWetting::Place> places;
    for (const auto& located : joined_.located()) {
        const DVec3 up = glm::normalize(located.second.metres());
        if (const auto here = weather_->at(up)) {
            places.push_back({.up = up, .sky = server::GroundWetting::sky_of(*here)});
        }
    }
    const double seconds = std::chrono::duration<double>(unwetted_).count();
    unwetted_ = {};
    const std::vector<flow::Simulation::Wetted> columns =
        server::GroundWetting::around(generator_, places);
    flow_.wet(columns, seconds);
}

void Server::tell_weather(const protocol::Weather& weather) {
    for (const net::ConnectionId connection : joined_.connections()) {
        send_weather(connection, weather);
    }
}

void Server::send_weather(net::ConnectionId connection, const protocol::Weather& weather) {
    if (!players_) {
        return;
    }
    if (auto sent = players_->send(connection, net::Delivery::Bulk, protocol::encode(weather));
        !sent) {
        log::debug(server_log, "couldn't send connection {} the weather: {}",
                   std::to_underlying(connection), sent.error());
    }
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
    HOG_ZONE("serve players");
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
            if (const auto left = joined_.leave(lost->connection)) {
                keep(*left);
            }
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
        joined_.join(connection,
                     player ? player->name
                            : std::format("player {}", std::to_underlying(connection)),
                     player ? std::optional{player->account} : std::nullopt);
        if (player) {
            send_placed(connection, player->account);
        }
        handshakes_.erase(shaking);
        send_bricks(connection, bricks_shared_.all());
        if (weather_ && weather_->latest()) {
            send_weather(connection, *weather_->latest());
        }
        // A world at real time goes without saying.
        if (clock_scale_ != 1) {
            send_clock(connection);
        }
    }
}

void Server::heard(net::ConnectionId connection, std::span<const std::byte> bytes) {
    const auto message = protocol::decode(bytes);
    if (!message) {
        log::debug(server_log, "connection {} sent something unreadable: {}",
                   std::to_underlying(connection), message.error());
    } else if (const auto* moved = std::get_if<protocol::Moved>(&*message)) {
        const server::Grip grip = [this](WorldPosition feet) {
            return globe::grip_at(globe_, feet.metres());
        };
        const server::MoveResult result =
            joined_.move(connection, *moved, MonotonicClock::now(), space_, grip);
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
    } else if (const auto* said = std::get_if<protocol::Said>(&*message)) {
        say(connection, *said);
    } else if (const auto* dug = std::get_if<protocol::Dug>(&*message)) {
        dig(connection, *dug);
    } else if (const auto* built = std::get_if<protocol::Built>(&*message)) {
        build(connection, *built);
    } else if (const auto* hurry = std::get_if<protocol::Hurry>(&*message)) {
        if (joined_.find(connection)) {
            hurry_ = {connection, hurry->scale};
        }
    } else {
        log::debug(server_log, "connection {} sent a message out of place",
                   std::to_underlying(connection));
    }
}

void Server::keep(const server::Remembered& remembered) {
    const protocol::Placed& place = remembered.place;
    if (auto kept =
            places_.keep(remembered.account, {.at = place.at,
                                              .flying = place.stance == protocol::Stance::Flying,
                                              .heading = place.facing.heading,
                                              .pitch = place.facing.pitch});
        !kept) {
        log::warn(save_log, "couldn't keep where account {} was: {}", remembered.account,
                  kept.error());
    }
}

void Server::send_placed(net::ConnectionId connection, std::uint64_t account) {
    const auto found = places_.find(account);
    if (!found) {
        log::warn(save_log, "couldn't find where account {} was: {}", account, found.error());
        return;
    }
    if (!*found || !players_) {
        return;
    }
    const store::PlayerPlace& place = **found;
    const protocol::Placed placed{.at = place.at,
                                  .stance = place.flying ? protocol::Stance::Flying
                                                         : protocol::Stance::Walking,
                                  .facing = {.heading = place.heading, .pitch = place.pitch}};
    if (auto sent = players_->send(connection, net::Delivery::Reliable, protocol::encode(placed));
        !sent) {
        log::warn(server_log, "couldn't place connection {}: {}", std::to_underlying(connection),
                  sent.error());
    }
}

void Server::say(net::ConnectionId connection, const protocol::Said& said) {
    if (!players_ || said.text.empty()) {
        return;
    }
    const flecs::entity speaker = joined_.find(connection).value_or(flecs::entity{});
    if (!speaker.is_valid()) {
        return;
    }
    const auto* replicated = speaker.try_get<server::Replicated>();
    const std::string from = replicated != nullptr ? replicated->name : std::string{};
    std::string text = said.text;
    // Hate speech gets no platform: it's reported here for moderators, and everyone else, the
    // speaker included, reads small talk instead.
    if (chat::hateful(text)) {
        log::warn(moderation_log, "connection {} ({}) said hate speech: {}",
                  std::to_underlying(connection), from, text);
        hateful_lines_.add();
        text = chat::small_talk_for(text);
    }
    const std::vector<std::byte> line =
        protocol::encode(protocol::Said{.from = from, .text = std::move(text)});
    for (const net::ConnectionId hearer : joined_.connections()) {
        if (auto sent = players_->send(hearer, net::Delivery::Reliable, line); !sent) {
            log::debug(server_log, "couldn't send connection {} the chat: {}",
                       std::to_underlying(hearer), sent.error());
            continue;
        }
        chat_lines_.add();
    }
}

void Server::dig(net::ConnectionId connection, const protocol::Dug& dug) {
    if (!joined_.find(connection)) {
        return;
    }
    const std::uint64_t before = voxels_.revision();
    globe::dig(globe_, protocol::to_world(dug.centre).metres(), dug.radius / 1'000.0);
    for (const globe::BrickSite brick : voxels_.changed_since(before)) {
        ground_.changed(brick);
        flow_.touched(brick);
        flow_.disturbed(brick);
    }
}

void Server::build(net::ConnectionId connection, const protocol::Built& built) {
    if (!joined_.find(connection)) {
        return;
    }
    const auto material = static_cast<globe::Material>(built.material);
    if (built.material >= globe::material_count || material == globe::Material::Air ||
        material == globe::Material::Water) {
        log::debug(server_log, "connection {} would build of material {}",
                   std::to_underlying(connection), built.material);
        return;
    }
    const std::uint64_t before = voxels_.revision();
    globe::build(globe_, protocol::to_world(built.centre).metres(), built.radius / 1'000.0,
                 material);
    for (const globe::BrickSite brick : voxels_.changed_since(before)) {
        ground_.changed(brick);
        flow_.touched(brick);
        flow_.disturbed(brick);
    }
}

void Server::tell_bricks() {
    if (!players_) {
        return;
    }
    const std::vector<protocol::Bricks> changed = bricks_shared_.changed();
    if (changed.empty()) {
        return;
    }
    for (const net::ConnectionId connection : joined_.connections()) {
        send_bricks(connection, changed);
    }
}

void Server::tell_grains() {
    const std::vector<flow::Parcel>& parcels = flow_.region().parcels();
    if (parcels.empty() && grains_told_.empty()) {
        return;
    }
    for (const auto& [connection, at] : joined_.located()) {
        const protocol::Grains grains =
            server::GrainSharing::near(parcels, at, static_cast<std::uint32_t>(tick_.value()));
        if (grains.flying.empty() && !grains_told_.contains(connection)) {
            continue;
        }
        if (auto sent =
                players_->send(connection, net::Delivery::Unreliable, protocol::encode(grains));
            !sent) {
            log::debug(server_log, "couldn't send connection {} grain: {}",
                       std::to_underlying(connection), sent.error());
            continue;
        }
        if (grains.flying.empty()) {
            grains_told_.erase(connection);
        } else {
            grains_told_.insert(connection);
        }
    }
}

void Server::send_bricks(net::ConnectionId connection,
                         const std::vector<protocol::Bricks>& bricks) {
    if (!players_) {
        return;
    }
    for (const protocol::Bricks& message : bricks) {
        if (auto sent = players_->send(connection, net::Delivery::Bulk, protocol::encode(message));
            !sent) {
            log::debug(server_log, "couldn't send connection {} bricks: {}",
                       std::to_underlying(connection), sent.error());
            return;
        }
        bricks_sent_.add(message.bricks.size());
    }
}

void Server::share() {
    HOG_ZONE("share");
    if (!players_) {
        return;
    }
    tell_bricks();
    tell_grains();
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

void Server::tell_clock() {
    for (const net::ConnectionId connection : joined_.connections()) {
        send_clock(connection);
    }
}

void Server::send_clock(net::ConnectionId connection) {
    if (!players_) {
        return;
    }
    if (auto sent = players_->send(connection, net::Delivery::Reliable,
                                   protocol::encode(protocol::Clock{.scale = clock_scale_}));
        !sent) {
        log::debug(server_log, "couldn't send connection {} the clock: {}",
                   std::to_underlying(connection), sent.error());
    }
}

void Server::submit() {
    save::Snapshot snapshot{.world = world_id_, .time = time_, .tick = tick_, .sections = {}};
    persistence_.store(snapshot);
    if (weather_ && !weather_->saved().empty()) {
        snapshot.add({.id = server::WeatherKeeper::section,
                      .version = server::WeatherKeeper::section_version,
                      .bytes = weather_->saved()});
    }
    writer_.submit(std::move(snapshot));
}

Result<void> Server::save() {
    HOG_ZONE("save");
    for (const server::Remembered& remembered : joined_.remembered()) {
        keep(remembered);
    }
    submit();
    auto flushed = writer_.flush();
    if (!flushed) {
        log::error(save_log, "saving failed: {}", flushed.error().message);
        return flushed;
    }
    auto bricks = globe::save(voxels_, bricks_);
    if (!bricks) {
        log::error(save_log, "saving bricks failed: {}", bricks.error().message);
        return std::unexpected(std::move(bricks).error());
    }
    if (*bricks > 0) {
        log::info(save_log, "saved {} bricks", *bricks);
    }
    return {};
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
