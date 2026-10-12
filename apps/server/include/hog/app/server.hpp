#pragma once

#include <hog/app/logging.hpp>
#include <hog/core/log.hpp>
#include <hog/core/metrics.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/world.hpp>
#include <hog/server/catch_up.hpp>
#include <hog/server/persistence.hpp>
#include <hog/server/server_config.hpp>
#include <hog/server/snapshot_writer.hpp>
#include <hog/server/tick_loop.hpp>
#include <hog/sim/forest.hpp>
#include <hog/sim/weather.hpp>
#include <hog/sim/wildlife.hpp>
#include <hog/store/sqlite_change_store.hpp>
#include <hog/store/sqlite_snapshot_store.hpp>

#include <flecs.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

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
           store::SqliteChangeStore changes);
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

    // Ticks at the configured rate, saving periodically, until `stop` is set; then saves and
    // waits for every snapshot to be written.
    template <server::Timer T>
    [[nodiscard]] Result<void> run(T& timer, const std::atomic<bool>& stop);

    // Snapshots the world and waits until it is written.
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
    // Logs and counts what has happened to wildlife since last asked.
    void report_wildlife();
    // Logs the species that are gone from everywhere, as the server opens.
    void recall_extinctions() const;
    void submit();

    server::ServerConfig config_;
    std::uint64_t world_id_;
    planet::World planet_;
    store::SqliteChangeStore changes_;
    store::SqliteSnapshotStore snapshots_;
    // After the store it writes to.
    server::SnapshotWriter writer_;
    flecs::world entities_;
    server::Persistence persistence_;
    jobs::JobSystem jobs_;
    // Set once the planet's changes are in, since the weather starts from the ground.
    std::unique_ptr<sim::Weather> weather_;
    std::unique_ptr<sim::Wildlife> wildlife_;
    std::unique_ptr<sim::Forest> forest_;
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
        server::catch_up(clock, config_.catch_up_ticks, now,
                         [this](const server::TickClock& at) { simulate(at.now(), at.step()); });
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

template <server::Timer T>
Result<void> Server::run(T& timer, const std::atomic<bool>& stop) {
    server::TickLoop loop{timer, server::LoopConfig{.start = time_,
                                                    .first_tick = tick_,
                                                    .rate = config_.tick_rate,
                                                    .time_scale = server::default_time_scale,
                                                    .max_burst = config_.max_burst}};
    server::SaveTimer saves{time_, config_.save_interval};
    const auto interval =
        std::chrono::duration_cast<MonotonicClock::duration>(config_.metrics_interval);
    MonotonicClock::time_point next_report = timer.now() + interval;
    loop.run([&](const server::TickClock& clock) {
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
        if (interval > MonotonicClock::duration::zero() && timer.now() >= next_report) {
            report(loop.stats().slipped);
            next_report = timer.now() + interval;
        }
        if (stop.load(std::memory_order_relaxed)) {
            loop.stop();
        }
    });
    auto saved = save();
    report(loop.stats().slipped);
    return saved;
}

} // namespace hog::app
