#pragma once

#include <hog/app/logging.hpp>
#include <hog/app/probe_layers.hpp>
#include <hog/core/log.hpp>
#include <hog/core/metrics.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/net/server.hpp>
#include <hog/physics/ground.hpp>
#include <hog/physics/space.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/world.hpp>
#include <hog/protocol/handshake.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/server/catch_up.hpp>
#include <hog/server/persistence.hpp>
#include <hog/server/players.hpp>
#include <hog/server/probe_service.hpp>
#include <hog/server/server_config.hpp>
#include <hog/server/snapshot_writer.hpp>
#include <hog/server/spatial_index.hpp>
#include <hog/server/tick_loop.hpp>
#include <hog/sim/forest.hpp>
#include <hog/sim/weather.hpp>
#include <hog/sim/wildlife.hpp>
#include <hog/store/sqlite_change_store.hpp>
#include <hog/store/sqlite_player_store.hpp>
#include <hog/store/sqlite_snapshot_store.hpp>

#include <flecs.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::app {

// A world and everything that keeps it: the planet with its changes, the persistent entities,
// their snapshots, and the server's metrics.
class Server {
    // Lets open() construct through make_unique while nothing else can.
    struct Opened {
        explicit Opened() = default;
    };

public:
    // Opens the world database, replays the planet's changes and restores the newest snapshot.
    // A world without one starts at `now`. Fails on a snapshot of another world or from a newer
    // build rather than start over.
    [[nodiscard]] static Result<std::unique_ptr<Server>> open(const server::ServerConfig& config,
                                                              GameTime now);

    Server(Opened /*unused*/, const server::ServerConfig& config,
           const planet::PlanetConfig& planet, store::SqliteSnapshotStore snapshots,
           store::SqliteChangeStore changes, store::SqlitePlayerStore places);
    Server(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(const Server&) = delete;
    Server& operator=(Server&&) = delete;
    ~Server();

    // Runs the world forward until it is within a tick of now(), saving once if that took more
    // than live ticks.
    template <typename Now>
        requires std::same_as<std::invoke_result_t<Now&>, GameTime>
    server::CatchUpStats catch_up(Now now);

    // Fastest debug tools may run the clock, in game seconds per real second.
    static constexpr std::uint32_t fastest = 86'400;

    // Ticks at the configured rate, saving periodically, until `stop` is set; then saves and
    // waits for every snapshot to be written. now() is real time, for debug tools to rejoin.
    template <server::Timer T, typename Now>
        requires std::same_as<std::invoke_result_t<Now&>, GameTime>
    [[nodiscard]] Result<void> run(T& timer, Now now, const std::atomic<bool>& stop);

    // Snapshots the world and waits until it is written, keeping where the players in are.
    [[nodiscard]] Result<void> save();

    // The game time and tick the world has reached.
    [[nodiscard]] GameTime time() const noexcept { return time_; }
    [[nodiscard]] Tick tick() const noexcept { return tick_; }
    [[nodiscard]] std::uint64_t world_id() const noexcept { return world_id_; }

    [[nodiscard]] server::Persistence& persistence() noexcept { return persistence_; }
    [[nodiscard]] const metrics::Registry& metrics() const noexcept { return registry_; }
    [[nodiscard]] const planet::World& planet() const noexcept { return planet_; }
    [[nodiscard]] const sim::Weather& weather() const noexcept { return *weather_; }
    // Null when no ecology is configured.
    [[nodiscard]] const sim::Wildlife* wildlife() const noexcept { return wildlife_.get(); }
    // Null when no vegetation is configured.
    [[nodiscard]] const sim::Forest* forest() const noexcept { return forest_.get(); }
    // Null without a debug port, or when it couldn't be opened.
    [[nodiscard]] const server::ProbeService* probe() const noexcept { return probe_.get(); }
    // Null without a players' port, or when it couldn't be opened.
    [[nodiscard]] const net::Server* players() const noexcept {
        return players_ ? &*players_ : nullptr;
    }

    // Logs the metrics, bringing those mirrored from elsewhere up to date first.
    void report(std::uint64_t slipped);

private:
    // Advances the world by one tick of game time.
    void simulate(GameTime from, GameDuration step);
    // Builds the weather, wildlife and forest as of `at`, on the planet with its changes.
    [[nodiscard]] Result<void> start_simulation(const planet::PlanetConfig& shape, GameTime at);
    // Reads the weather, wildlife and forest from a snapshot; a part it can't read starts over.
    [[nodiscard]] Result<void> restore_simulation(const save::Snapshot& snapshot);
    void advance_weather(GameTime to);
    // When the wildlife or the forest next steps, whichever is sooner; none without either.
    [[nodiscard]] std::optional<GameTime> next_due() const;
    void step_wildlife(GameTime at);
    void step_forest(GameTime at);
    // Counts the fires and measures the forest.
    void report_forest();
    // Gives a new world's forest its history: the woods nearest the start burned, if
    // configured.
    void burn_history();
    // Logs and counts what has happened to wildlife since last asked.
    void report_wildlife();
    // Logs the species that are gone from everywhere, as the server opens.
    void recall_extinctions() const;
    void submit();
    [[nodiscard]] LayerSources layer_sources() const;
    // Opens the debug port, if one is configured, logging rather than failing if it can't.
    void open_probe();
    // Opens the players' port, if one is configured, logging rather than failing if it can't.
    void open_players();
    // Takes in what players' connections sent and what became of them, shaking hands with those
    // just connected.
    void serve_players();
    // Does what a handshake answered: replies, and closes the connection or lets it in.
    void answer(net::ConnectionId connection, const protocol::Answer& answer);
    // Does what a player who's in asks.
    void heard(net::ConnectionId connection, std::span<const std::byte> bytes);
    // Keeps where an account's player was, to place them there when they come back.
    void keep(const server::Remembered& remembered);
    // Sends a player who's been in before where they were.
    void send_placed(net::ConnectionId connection, std::uint64_t account);
    // Sends a player's chat to everyone, under the name the server holds for them.
    void say(net::ConnectionId connection, const protocol::Said& said);
    // Sends each player a snapshot of those near them.
    void share();
    // Sends every player the weather once it has stepped, to fade to over a step at `scale`.
    void tell_weather(std::int64_t scale);
    void send_weather(net::ConnectionId connection);
    // Sends every player how the woods fare once the forest has stepped, if that's changed.
    void tell_woodlands();
    void send_woodlands(net::ConnectionId connection);
    // Sends every player how fast the world's clock runs.
    void tell_clock();
    void send_clock(net::ConnectionId connection);
    // Runs the clock as the newest player's wish asked, if any, telling everyone how it runs now.
    template <typename Loop, typename Now>
    void hurry_clock(Loop& loop, Now& now);
    // Answers debug tools waiting on the world, telling them how clock() runs and asking
    // steer(control) to change it.
    template <typename Clock, typename Steer>
    void serve_probe(Clock clock, Steer steer);
    // Changes how `loop` runs as a debug tool asked, if this world allows it.
    template <typename Loop, typename Now>
    [[nodiscard]] probe::Controlled steer(Loop& loop, const probe::Control& control, Now& now);

    server::ServerConfig config_;
    std::uint64_t world_id_;
    planet::World planet_;
    store::SqliteChangeStore changes_;
    store::SqliteSnapshotStore snapshots_;
    store::SqlitePlayerStore places_;
    // After the store it writes to.
    server::SnapshotWriter writer_;
    // Before the world, which it must outlive.
    server::SpatialIndex nearby_;
    flecs::world entities_;
    server::Persistence persistence_;
    jobs::JobSystem jobs_;
    // Set once the planet's changes are in, since the weather starts from the ground.
    std::unique_ptr<sim::Weather> weather_;
    std::unique_ptr<sim::Wildlife> wildlife_;
    std::unique_ptr<sim::Forest> forest_;
    // After what it reads, so it stops first.
    std::unique_ptr<server::ProbeService> probe_;
    std::optional<net::Server> players_;
    std::vector<net::Event> player_events_;
    // Connections still to say hello, and the players let in.
    std::map<net::ConnectionId, protocol::Handshake> handshakes_;
    // The weather players were last sent, coded, and the step it's from.
    std::vector<std::byte> weather_told_;
    std::optional<GameTime> weather_told_at_;
    // The same for the woods.
    std::vector<std::byte> woodlands_told_;
    std::optional<GameTime> woodlands_told_at_;
    // The newest wish to run the clock at another speed, and whose it was.
    std::optional<std::pair<net::ConnectionId, std::uint32_t>> hurry_;
    // The loop's scale as of its last turn, for players joining between them.
    std::uint32_t clock_scale_ = 1;
    server::Players joined_;
    // The ground near players, to check their moves against.
    physics::Space space_;
    physics::Ground ground_;
    // The login service's, when only players it logged in may join.
    std::optional<login::PublicKey> login_key_;
    GameTime time_;
    Tick tick_;

    metrics::Registry registry_;
    metrics::Counter& ticks_;
    metrics::Histogram& tick_seconds_;
    metrics::Counter& slipped_;
    metrics::Counter& catch_up_ticks_;
    metrics::Gauge& persistent_entities_;
    metrics::Counter& snapshots_written_;
    metrics::Counter& snapshots_replaced_;
    metrics::Counter& snapshots_failed_;
    metrics::Counter& snapshots_skipped_;
    metrics::Counter& weather_steps_;
    metrics::Histogram& weather_step_seconds_;
    metrics::Counter& wildlife_steps_;
    metrics::Histogram& wildlife_step_seconds_;
    metrics::Gauge& extinct_species_;
    // By ecology event kind.
    std::array<metrics::Counter*, 5> ecology_events_;
    metrics::Counter& forest_steps_;
    metrics::Histogram& forest_step_seconds_;
    metrics::Counter& fires_started_;
    metrics::Counter& alight_km2_;
    metrics::Gauge& burning_cells_;
    metrics::Gauge& biomass_tonnes_;
    metrics::Gauge& connections_;
    metrics::Counter& shared_snapshots_;
    metrics::Counter& shared_bytes_;
    metrics::Counter& corrected_moves_;
    metrics::Counter& chat_lines_;
    metrics::Counter& hateful_lines_;
    // km² alight not yet counted, being under one.
    double alight_left_ = 0.0;
    metrics::Reporter reporter_;
    // What the mirrored counters were last brought up to.
    server::SnapshotWriter::Stats mirrored_writes_;
    std::uint64_t mirrored_slips_ = 0;
};

template <typename Now>
    requires std::same_as<std::invoke_result_t<Now&>, GameTime>
server::CatchUpStats Server::catch_up(Now now) {
    server::TickClock clock{time_, config_.tick_rate, server::default_time_scale, tick_};
    const MonotonicClock::time_point started = MonotonicClock::now();
    const server::CatchUpStats stats =
        server::catch_up(clock, config_.catch_up_ticks, now, [this](const server::TickClock& at) {
            simulate(at.now(), at.step());
            time_ = at.now() + at.step();
            serve_probe([] { return probe::Clock{}; },
                        [](const probe::Control& /*unused*/) {
                            return probe::Controlled{.clock = {},
                                                     .error = "the world is still catching up"};
                        });
        });
    time_ = clock.now();
    tick_ = clock.tick();
    catch_up_ticks_.add(stats.ticks);
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(MonotonicClock::now() - started);
    // A gap the live rate covered was only startup time, not downtime worth a snapshot.
    if (stats.largest_scale > server::default_time_scale) {
        log::info(server_log, "caught up {} ticks at up to {}x in {:%Q%q}", stats.ticks,
                  stats.largest_scale, took);
        submit();
    } else if (stats.ticks > 0) {
        log::debug(server_log, "caught up {} ticks in {:%Q%q}", stats.ticks, took);
    }
    return stats;
}

template <typename Clock, typename Steer>
void Server::serve_probe(Clock clock, Steer steer) {
    if (probe_) {
        probe_->serve(
            [&](const probe::Fetch& fetch) {
                probe::Layers layers = fetch_layers(layer_sources(), fetch, time_);
                layers.clock = clock();
                return layers;
            },
            steer,
            [&](const probe::Inspect& inspect) {
                return inspect_layers(layer_sources(), inspect, time_);
            });
    }
}

namespace detail {

template <typename Loop>
probe::Clock clock_of(const Loop& loop) {
    return probe::Clock{.paused = loop.paused(), .scale = static_cast<std::uint32_t>(loop.scale())};
}

} // namespace detail

template <typename Loop, typename Now>
probe::Controlled Server::steer(Loop& loop, const probe::Control& control, Now& now) {
    if (!config_.debug.time_control) {
        return {.clock = detail::clock_of(loop), .error = "time control is off for this world"};
    }
    switch (control.action) {
    case probe::Action::Pause:
        loop.pause();
        break;
    case probe::Action::Resume:
        loop.resume();
        break;
    case probe::Action::Step:
        if (!loop.paused() || control.amount == 0) {
            return {.clock = detail::clock_of(loop),
                    .error = "step at least one tick, while paused"};
        }
        loop.step_ticks(control.amount);
        break;
    case probe::Action::Speed:
        if (control.amount == 0 || control.amount > fastest) {
            return {.clock = detail::clock_of(loop),
                    .error = std::format("speed must be from 1 to {}", fastest)};
        }
        loop.set_scale(control.amount);
        break;
    case probe::Action::RealTime: {
        const GameTime target = now();
        if (time_ > target) {
            return {.clock = detail::clock_of(loop),
                    .error = std::format("{} ahead of real time",
                                         std::chrono::floor<std::chrono::seconds>(time_ - target))};
        }
        loop.set_scale(server::default_time_scale);
        loop.resume();
        loop.catch_up_to(target, config_.catch_up_ticks);
        break;
    }
    }
    return {.clock = detail::clock_of(loop), .error = {}};
}

template <typename Loop, typename Now>
void Server::hurry_clock(Loop& loop, Now& now) {
    clock_scale_ = static_cast<std::uint32_t>(loop.scale());
    const auto hurry = std::exchange(hurry_, std::nullopt);
    if (!hurry) {
        return;
    }
    const probe::Controlled done =
        steer(loop, probe::Control{.action = probe::Action::Speed, .amount = hurry->second}, now);
    if (!done.error.empty()) {
        log::info(server_log, "connection {} asked to run time at {}x: {}",
                  std::to_underlying(hurry->first), hurry->second, done.error);
        return;
    }
    log::info(server_log, "connection {} set time to {}x", std::to_underlying(hurry->first),
              done.clock.scale);
    clock_scale_ = done.clock.scale;
    tell_clock();
}

template <server::Timer T, typename Now>
    requires std::same_as<std::invoke_result_t<Now&>, GameTime>
Result<void> Server::run(T& timer, Now now, const std::atomic<bool>& stop) {
    server::TickLoop loop{timer, server::LoopConfig{.start = time_,
                                                    .first_tick = tick_,
                                                    .rate = config_.tick_rate,
                                                    .time_scale = server::default_time_scale,
                                                    .max_burst = config_.max_burst}};
    server::SaveTimer saves{time_, config_.save_interval};
    const auto interval =
        std::chrono::duration_cast<MonotonicClock::duration>(config_.metrics_interval);
    MonotonicClock::time_point next_report = timer.now() + interval;
    MonotonicClock::time_point next_share = timer.now();
    // Between ticks, and in place of them while paused.
    const auto between = [&] {
        serve_probe([&loop] { return detail::clock_of(loop); },
                    [&](const probe::Control& control) { return steer(loop, control, now); });
        serve_players();
        hurry_clock(loop, now);
        tell_weather(loop.scale());
        tell_woodlands();
        if (const MonotonicClock::time_point at = timer.now(); at >= next_share) {
            share();
            next_share += protocol::snapshot_interval;
            // After a stall, carry on from now rather than send the missed ones at once.
            if (next_share <= at) {
                next_share = at + protocol::snapshot_interval;
            }
        }
        if (interval > MonotonicClock::duration::zero() && timer.now() >= next_report) {
            report(loop.stats().slipped);
            next_report = timer.now() + interval;
        }
        if (stop.load(std::memory_order_relaxed)) {
            loop.stop();
        }
    };
    loop.run(
        [&](const server::TickClock& clock) {
            const MonotonicClock::time_point started = MonotonicClock::now();
            simulate(clock.now(), clock.step());
            time_ = clock.now() + clock.step();
            tick_ = clock.tick() + TickDelta{1};
            if (saves.due(time_)) {
                submit();
            }
            ticks_.add();
            tick_seconds_.observe(
                std::chrono::duration<double>(MonotonicClock::now() - started).count());
            between();
        },
        [&](const server::TickClock& /*unused*/) { between(); });
    auto saved = save();
    report(loop.stats().slipped);
    return saved;
}

} // namespace hog::app
