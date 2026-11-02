#pragma once

#include <hog/app/logging.hpp>
#include <hog/core/log.hpp>
#include <hog/core/metrics.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/flow/simulation.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/login/keys.hpp>
#include <hog/net/server.hpp>
#include <hog/physics/ground.hpp>
#include <hog/physics/space.hpp>
#include <hog/protocol/handshake.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/server/brick_sharing.hpp>
#include <hog/server/catch_up.hpp>
#include <hog/server/persistence.hpp>
#include <hog/server/players.hpp>
#include <hog/server/server_config.hpp>
#include <hog/server/snapshot_writer.hpp>
#include <hog/server/spatial_index.hpp>
#include <hog/server/tick_loop.hpp>
#include <hog/store/sqlite_brick_archive.hpp>
#include <hog/store/sqlite_player_store.hpp>
#include <hog/store/sqlite_snapshot_store.hpp>

#include <flecs.h>

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

// A world and everything that keeps it: the voxel world with its bricks, the persistent entities,
// their snapshots, and the server's metrics.
class Server {
    // Lets open() construct through make_unique while nothing else can.
    struct Opened {
        explicit Opened() = default;
    };

public:
    // Opens the world database, loads the bricks changed and restores the newest snapshot. A
    // world without one starts at `now`. Fails on a snapshot of another world or from a newer
    // build rather than start over.
    [[nodiscard]] static Result<std::unique_ptr<Server>> open(const server::ServerConfig& config,
                                                              GameTime now);
    Server(Opened /*unused*/, const server::ServerConfig& config, const globe::Planet& globe,
           store::SqliteSnapshotStore snapshots, store::SqlitePlayerStore places,
           store::SqliteBrickArchive bricks);
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

    // Fastest players may run the clock, in game seconds per real second.
    static constexpr std::uint32_t fastest = 86'400;

    // Ticks at the configured rate, saving periodically, until `stop` is set; then saves and
    // waits for every snapshot to be written. now() is real time, which nothing needs yet.
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
    // The voxel world: the generator over the bricks changed, which only the server writes.
    [[nodiscard]] globe::World globe() noexcept { return globe_; }
    [[nodiscard]] const globe::Generator& generator() const noexcept { return generator_; }
    [[nodiscard]] const globe::VoxelStore& voxels() const noexcept { return voxels_; }
    // Null without a players' port, or when it couldn't be opened.
    [[nodiscard]] const net::Server* players() const noexcept {
        return players_ ? &*players_ : nullptr;
    }

    // Logs the metrics, bringing those mirrored from elsewhere up to date first.
    void report(std::uint64_t slipped);

private:
    // Advances the world by one tick of game time.
    void simulate(GameTime from, GameDuration step);
    void submit();
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
    // Digs or builds for a player, where the ball is one they may; the bricks changed are told
    // to everyone with the tick's.
    void dig(net::ConnectionId connection, const protocol::Dug& dug);
    void build(net::ConnectionId connection, const protocol::Built& built);
    void tell_bricks();
    void send_bricks(net::ConnectionId connection, const std::vector<protocol::Bricks>& bricks);
    // Sends each player a snapshot of those near them.
    void share();
    // Sends every player how fast the world's clock runs.
    void tell_clock();
    void send_clock(net::ConnectionId connection);
    // Runs the clock as the newest player's wish asked, if any, telling everyone how it runs now.
    template <typename Loop>
    void hurry_clock(Loop& loop);

    server::ServerConfig config_;
    std::uint64_t world_id_;
    globe::Generator generator_;
    globe::VoxelStore voxels_;
    globe::World globe_;
    flow::Simulation flow_;
    store::SqliteBrickArchive bricks_;
    server::BrickSharing bricks_shared_;
    store::SqliteSnapshotStore snapshots_;
    store::SqlitePlayerStore places_;
    // After the store it writes to.
    server::SnapshotWriter writer_;
    // Before the world, which it must outlive.
    server::SpatialIndex nearby_;
    flecs::world entities_;
    server::Persistence persistence_;
    jobs::JobSystem jobs_;
    std::optional<net::Server> players_;
    std::vector<net::Event> player_events_;
    // Connections still to say hello, and the players let in.
    std::map<net::ConnectionId, protocol::Handshake> handshakes_;
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
    metrics::Gauge& connections_;
    metrics::Counter& shared_snapshots_;
    metrics::Counter& shared_bytes_;
    metrics::Counter& corrected_moves_;
    metrics::Counter& chat_lines_;
    metrics::Counter& hateful_lines_;
    metrics::Counter& bricks_sent_;
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

template <typename Loop>
void Server::hurry_clock(Loop& loop) {
    clock_scale_ = static_cast<std::uint32_t>(loop.scale());
    const auto hurry = std::exchange(hurry_, std::nullopt);
    if (!hurry) {
        return;
    }
    const auto [connection, scale] = *hurry;
    if (!config_.debug.time_control) {
        log::info(server_log, "connection {} asked to run time at {}x: time control is off",
                  std::to_underlying(connection), scale);
        return;
    }
    if (scale == 0 || scale > fastest) {
        log::info(server_log, "connection {} asked to run time at {}x: 1 to {} allowed",
                  std::to_underlying(connection), scale, fastest);
        return;
    }
    loop.set_scale(scale);
    log::info(server_log, "connection {} set time to {}x", std::to_underlying(connection), scale);
    clock_scale_ = scale;
    tell_clock();
}

template <server::Timer T, typename Now>
    requires std::same_as<std::invoke_result_t<Now&>, GameTime>
Result<void> Server::run(T& timer, Now /*now*/, const std::atomic<bool>& stop) {
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
        serve_players();
        hurry_clock(loop);
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
