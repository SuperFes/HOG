#include <hog/app/probe_layers.hpp>
#include <hog/app/server.hpp>
#include <hog/probe/channel.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using hog::GameTime;
using hog::MonotonicClock;
using hog::app::Server;

namespace {

constexpr GameTime start{
    std::chrono::sys_days{std::chrono::year{2026} / 10 / 11}.time_since_epoch()};

// Real time that moves only when the loop sleeps; asks the server to stop after some sleeps,
// which at one tick per sleep is that many ticks plus the one in progress.
class StoppingTimer {
public:
    StoppingTimer(std::atomic<bool>& stop, int sleeps) : stop_(&stop), sleeps_(sleeps) {}

    [[nodiscard]] MonotonicClock::time_point now() const { return time_; }
    void sleep_until(MonotonicClock::time_point when) {
        time_ = std::max(time_, when);
        if (--sleeps_ == 0) {
            stop_->store(true);
        }
    }

private:
    std::atomic<bool>* stop_;
    int sleeps_;
    MonotonicClock::time_point time_;
};

// A small planet and a world file in a fresh directory.
class World {
public:
    World() { write_planet(1); }

    void write_planet(std::uint64_t seed) const {
        std::ofstream{directory_.path() / "planet.toml"} << "[planet]\nseed = " << seed
                                                         << "\nradius_km = 115\n";
    }

    [[nodiscard]] std::filesystem::path write_ecology(std::string_view text) const {
        return write("ecology.toml", text);
    }

    [[nodiscard]] std::filesystem::path write_vegetation(std::string_view text) const {
        return write("forest.toml", text);
    }

    [[nodiscard]] hog::server::ServerConfig config() const {
        hog::server::ServerConfig config;
        config.world = directory_.path() / "var" / "world.db";
        config.planet = directory_.path() / "planet.toml";
        config.metrics_interval = 0s;
        return config;
    }

private:
    [[nodiscard]] std::filesystem::path write(std::string_view name, std::string_view text) const {
        const auto path = directory_.path() / name;
        std::ofstream{path} << text;
        return path;
    }

    hog::testing::TemporaryDirectory directory_;
};

std::unique_ptr<Server> opened(const hog::server::ServerConfig& config, GameTime now = start) {
    auto server = Server::open(config, now);
    REQUIRE(server);
    return std::move(*server);
}

std::uint64_t counter(const Server& server, std::string_view name) {
    for (const auto& entry : server.metrics().entries()) {
        if (entry.name == name) {
            return std::get<std::unique_ptr<hog::metrics::Counter>>(entry.metric)->value();
        }
    }
    FAIL("no counter " << name);
    return 0;
}

double gauge(const Server& server, std::string_view name) {
    for (const auto& entry : server.metrics().entries()) {
        if (entry.name == name) {
            return std::get<std::unique_ptr<hog::metrics::Gauge>>(entry.metric)->value();
        }
    }
    FAIL("no gauge " << name);
    return 0.0;
}

// Runs the server for `ticks` live ticks.
void run(Server& server, int ticks) {
    std::atomic<bool> stop{false};
    StoppingTimer timer{stop, ticks - 1};
    REQUIRE(server.run(timer, [] { return start; }, stop));
}

} // namespace

TEST_CASE("a new world starts now and keeps its entities", "[app][server]") {
    const World world;
    {
        const auto server = opened(world.config());
        CHECK(server->tick() == hog::Tick{0});
        CHECK(server->time() == start);
        server->persistence().create().set<hog::server::Position>(
            {.at = hog::WorldPosition{1.0, 115'000.0, 2.0}});
        server->persistence().create();
        run(*server, 10);
        CHECK(server->tick() == hog::Tick{10});
        CHECK(server->time() == start + 500ms);
        CHECK(counter(*server, "hog_ticks_total") == 10);
        CHECK(counter(*server, "hog_snapshots_written_total") == 1);
    }

    const auto reopened = opened(world.config(), start + 1h);
    CHECK(reopened->tick() == hog::Tick{10});
    CHECK(reopened->time() == start + 500ms);
    CHECK(reopened->persistence().size() == 2);
    const auto first = reopened->persistence().find(1);
    REQUIRE(first);
    CHECK(first.value_or(flecs::entity{}).get<hog::server::Position>().at ==
          hog::WorldPosition{1.0, 115'000.0, 2.0});
}

TEST_CASE("a reopened world catches up to now", "[app][server]") {
    const World world;
    auto config = world.config();
    config.catch_up_ticks = 100;
    REQUIRE(opened(config)->save());

    const GameTime now = start + 24h;
    {
        const auto server = opened(config, now);
        const auto stats = server->catch_up([&] { return now; });
        CHECK(stats.ticks > 0);
        CHECK(stats.ticks <= 2 * config.catch_up_ticks);
        CHECK(server->time() <= now);
        CHECK(now - server->time() < 50ms);
        CHECK(counter(*server, "hog_catch_up_ticks_total") == stats.ticks);
        CHECK(now - server->weather().time() < config.weather.step);
        CHECK(counter(*server, "hog_weather_steps_total") > 0);

        // Already at now, there's nothing to do.
        CHECK(server->catch_up([&] { return now; }).ticks == 0);
    }
    // Catching up saved where it got to.
    CHECK(now - opened(config, now)->time() < 50ms);
}

TEST_CASE("startup time is caught up without saving", "[app][server]") {
    const World world;
    {
        const auto server = opened(world.config());
        CHECK(server->catch_up([] { return GameTime{start + 120ms}; }).ticks == 2);
        CHECK(server->tick() == hog::Tick{2});
    }
    CHECK(opened(world.config())->tick() == hog::Tick{0});
}

TEST_CASE("weather is kept with the world", "[app][server]") {
    const World world;
    auto config = world.config();
    config.catch_up_ticks = 100;
    REQUIRE(opened(config)->save());

    const GameTime later = start + 6h;
    hog::sim::WeatherFields before;
    int level = 0;
    {
        const auto server = opened(config, later);
        server->catch_up([&] { return later; });
        before = server->weather().fields();
        level = server->weather().grid().level();
        CHECK(server->weather().time() > start);
    }
    CHECK(opened(config, later)->weather().fields() == before);

    // Saved weather doesn't fit a coarser grid, so it starts over.
    config.weather.cell_size *= 4.0;
    const auto coarser = opened(config, later);
    CHECK(coarser->weather().grid().level() == level - 2);
    CHECK(coarser->weather().fields().size() == before.size() / 16);
}

TEST_CASE("wildlife lives with the weather and is kept with the world", "[app][server]") {
    const World world;
    auto config = world.config();
    config.catch_up_ticks = 100;
    CHECK(opened(config)->wildlife() == nullptr);

    config.wildlife.ecology = world.write_ecology(R"(
[forage]
capacity = { grassland = 100000, savanna = 100000, "temperate forest" = 50000 }

[[species]]
name = "deer"
diet = { forage = 1 }
habitat = { grassland = 1, savanna = 1, temperate_forest = 0.5 }
)");
    REQUIRE(opened(config)->save());
    const GameTime later = start + 72h;
    hog::sim::WildlifeFields before;
    {
        const auto server = opened(config, later);
        REQUIRE(server->wildlife() != nullptr);
        CHECK(server->wildlife()->total(0) > 0.0);
        server->catch_up([&] { return later; });
        CHECK(server->wildlife()->time() == later);
        CHECK(counter(*server, "hog_wildlife_steps_total") == 3);
        CHECK(server->wildlife()->grid().level() == server->weather().grid().level());
        before = server->wildlife()->fields();
    }
    CHECK(opened(config, later)->wildlife()->fields() == before);

    config.wildlife.ecology = world.write_ecology("[[species]]\nname = 3\n");
    const auto broken = Server::open(config, later);
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.find("ecology.toml") != std::string::npos);
}

TEST_CASE("species dying out are counted", "[app][server]") {
    const World world;
    auto config = world.config();
    config.catch_up_ticks = 100;
    // With nothing to eat, it starves on the first day.
    config.wildlife.ecology = world.write_ecology(R"(
[[species]]
name = "dodo"
habitat = { grassland = 1, savanna = 1, temperate_forest = 1 }
starving = 1
)");
    REQUIRE(opened(config)->save());
    const GameTime later = start + 48h;
    {
        const auto server = opened(config, later);
        CHECK(gauge(*server, "hog_wildlife_extinct_species") == 0.0);
        server->catch_up([&] { return later; });
        CHECK(server->wildlife()->presence(0).extinct);
        CHECK(gauge(*server, "hog_wildlife_extinct_species") == 1.0);
        CHECK(counter(*server, "hog_wildlife_extinctions_total") == 1);
        CHECK(counter(*server, "hog_wildlife_local_extinctions_total") > 0);
        CHECK(counter(*server, "hog_wildlife_returns_total") == 0);
        REQUIRE(server->save());
    }
    CHECK(gauge(*opened(config, later), "hog_wildlife_extinct_species") == 1.0);
}

TEST_CASE("the forest grows with the weather and is kept with the world", "[app][server]") {
    const World world;
    auto config = world.config();
    config.catch_up_ticks = 100;
    CHECK(opened(config)->forest() == nullptr);

    config.forest.vegetation = world.write_vegetation(R"(
[growth]
capacity = { grassland = 0.5, savanna = 3, temperate_forest = 20, boreal_forest = 10 }
)");
    REQUIRE(opened(config)->save());
    const GameTime later = start + 72h;
    hog::sim::ForestFields before;
    {
        const auto server = opened(config, later);
        REQUIRE(server->forest() != nullptr);
        CHECK(gauge(*server, "hog_forest_biomass_tonnes") > 0.0);
        server->catch_up([&] { return later; });
        CHECK(server->forest()->time() == later);
        CHECK(counter(*server, "hog_forest_steps_total") == 72);
        CHECK(server->forest()->grid().level() == server->weather().grid().level());
        CHECK(gauge(*server, "hog_forest_biomass_tonnes") == server->forest()->total() / 1e3);
        CHECK(gauge(*server, "hog_forest_burning_cells") ==
              static_cast<double>(server->forest()->burning()));
        before = server->forest()->fields();
    }
    CHECK(opened(config, later)->forest()->fields() == before);

    config.forest.vegetation = world.write_vegetation("[fire]\nsmoke = 1\n");
    const auto broken = Server::open(config, later);
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.find("forest.toml") != std::string::npos);
}

TEST_CASE("the world's layers are laid out for debug tools", "[app][server]") {
    const World world;
    auto config = world.config();
    config.forest.vegetation = world.write_vegetation("[growth]\ncapacity = { grassland = 0.5 }\n");
    const auto server = opened(config);
    const hog::app::LayerSources sources{.planet = server->planet(),
                                         .weather = server->weather(),
                                         .wildlife = server->wildlife(),
                                         .forest = server->forest()};
    const auto catalogue = hog::app::layer_catalogue(sources);
    const auto named = [&](std::string_view name) {
        return std::ranges::find(catalogue, name, &hog::probe::LayerInfo::name) != catalogue.end();
    };
    CHECK(named("planet/biome"));
    CHECK(named("weather/temperature"));
    CHECK(named("weather/wind_north"));
    CHECK(named("forest/biomass"));
    CHECK_FALSE(named("wildlife/forage"));

    const auto layers = hog::app::fetch_layers(
        sources, hog::probe::Fetch{.names = {"forest/biomass", "nothing", "weather/temperature"}},
        start);
    CHECK(layers.time == start);
    REQUIRE(layers.layers.size() == 2);
    // In catalogue order, whatever order they were asked for in.
    CHECK(layers.layers[0].name == "weather/temperature");
    CHECK(layers.layers[0].values == server->weather().fields().temperature);
    CHECK(layers.layers[1].values == server->forest()->fields().biomass);

    const auto wind = hog::app::fetch_layers(
        sources, hog::probe::Fetch{.names = {"weather/wind_east", "planet/area"}}, start);
    REQUIRE(wind.layers.size() == 2);
    CHECK(wind.layers[0].name == "planet/area");
    CHECK(std::ranges::all_of(wind.layers[0].values, [](float area) { return area > 0.0F; }));
    const auto& winds = server->weather().fields().wind;
    std::vector<float> east(winds.size());
    std::ranges::transform(winds, east.begin(), [](const hog::Vec2& blowing) { return blowing.x; });
    CHECK(wind.layers[1].values == east);

    // Precipitation goes out as hourly rates.
    const auto rain =
        hog::app::fetch_layers(sources, hog::probe::Fetch{.names = {"weather/rain"}}, start);
    REQUIRE(rain.layers.size() == 1);
    const double steps_per_hour = std::chrono::duration<double>(1h) /
                                  std::chrono::duration<double>(server->weather().config().step);
    const auto& fallen = server->weather().fields().rain;
    REQUIRE(rain.layers[0].values.size() == fallen.size());
    for (std::size_t cell = 0; cell < fallen.size(); cell += 97) {
        CHECK(rain.layers[0].values[cell] ==
              static_cast<float>(static_cast<double>(fallen[cell]) * steps_per_hour));
    }
}

TEST_CASE("debug tools fetch layers from a running server", "[app][server]") {
    const World world;
    auto config = world.config();
    CHECK(opened(config)->probe() == nullptr);

    config.debug.port = 0;
    const auto server = opened(config);
    REQUIRE(server->probe() != nullptr);
    auto socket = hog::probe::Socket::connect("127.0.0.1", server->probe()->port());
    REQUIRE(socket);
    hog::probe::Channel tool{std::move(*socket)};
    REQUIRE(tool.send(hog::probe::Hello{}));
    auto welcome = tool.receive(5s);
    REQUIRE(welcome);
    REQUIRE(welcome->has_value());
    const hog::probe::Message greeting = welcome->value_or(hog::probe::Message{});
    const auto* welcomed = std::get_if<hog::probe::Welcome>(&greeting);
    REQUIRE(welcomed != nullptr);
    CHECK(welcomed->world_id == server->world_id());
    CHECK(welcomed->level == server->weather().grid().level());
    CHECK_FALSE(welcomed->layers.empty());

    std::atomic<bool> answered = false;
    std::optional<hog::probe::Message> answer;
    std::thread asking{[&] {
        REQUIRE(tool.send(hog::probe::Fetch{.names = {"weather/pressure"}}));
        auto received = tool.receive(10s);
        if (received && received->has_value()) {
            answer = **received;
        }
        answered.store(true);
    }};
    // Fetches are answered as the world ticks.
    GameTime now = start;
    while (!answered.load()) {
        now += 1s;
        server->catch_up([now] { return now; });
        std::this_thread::sleep_for(1ms);
    }
    asking.join();
    REQUIRE(answer);
    const hog::probe::Message reply = answer.value_or(hog::probe::Message{});
    const auto* layers = std::get_if<hog::probe::Layers>(&reply);
    REQUIRE(layers != nullptr);
    CHECK(layers->time > start);
    REQUIRE(layers->layers.size() == 1);
    CHECK(layers->layers[0].values.size() == server->weather().grid().size());

    // A port already taken leaves the server running without one.
    config.debug.port = server->probe()->port();
    CHECK(opened(config)->probe() == nullptr);
}

namespace {

// A debug tool's side of a conversation, recording rather than checking, as it runs on its own
// thread.
class Ask {
public:
    explicit Ask(hog::probe::Channel& channel) : channel_(&channel) {}

    std::optional<hog::probe::Message> answer(const hog::probe::Message& message) {
        if (!channel_->send(message)) {
            return std::nullopt;
        }
        auto received = channel_->receive(10s);
        if (!received || !received->has_value()) {
            return std::nullopt;
        }
        return *received;
    }

    hog::probe::Controlled control(hog::probe::Action action, std::uint32_t amount = 0) {
        const auto said = answer(hog::probe::Control{.action = action, .amount = amount});
        const auto* controlled = said ? std::get_if<hog::probe::Controlled>(&*said) : nullptr;
        return controlled != nullptr ? *controlled
                                     : hog::probe::Controlled{.clock = {}, .error = "no answer"};
    }

    // Empty, at the epoch, if there was no answer.
    hog::probe::Layers fetch() {
        const auto said = answer(hog::probe::Fetch{});
        const auto* layers = said ? std::get_if<hog::probe::Layers>(&*said) : nullptr;
        return layers != nullptr ? *layers : hog::probe::Layers{};
    }

private:
    hog::probe::Channel* channel_;
};

// Runs the server at real pace here, as only the thread that opened it may, while a tool runs
// script(ask) on another thread; stops once the script is done.
template <typename Script>
void steered(Server& server, Script script) {
    std::atomic<bool> stop = false;
    std::thread tool{[&] {
        if (auto socket = hog::probe::Socket::connect("127.0.0.1", server.probe()->port())) {
            hog::probe::Channel channel{std::move(*socket)};
            Ask ask{channel};
            const auto welcome = ask.answer(hog::probe::Hello{});
            if (welcome && std::holds_alternative<hog::probe::Welcome>(*welcome)) {
                script(ask);
            }
        }
        stop.store(true);
    }};
    hog::server::SteadyTimer timer;
    const auto ran = server.run(timer, [] { return start + 1h; }, stop);
    tool.join();
    CHECK(ran);
}

} // namespace

TEST_CASE("a new world can start at a chosen moment", "[app][server]") {
    const World world;
    auto config = world.config();
    config.debug.start = start + std::chrono::days{365};
    CHECK(opened(config)->time() == start + std::chrono::days{365});
}

TEST_CASE("debug tools can't steer the clock unless allowed", "[app][server]") {
    const World world;
    auto config = world.config();
    config.debug.port = 0;
    const auto server = opened(config);
    hog::probe::Controlled refused;
    hog::probe::Layers layers;
    steered(*server, [&](Ask& ask) {
        refused = ask.control(hog::probe::Action::Pause);
        layers = ask.fetch();
    });
    CHECK_FALSE(refused.error.empty());
    CHECK_FALSE(refused.clock.paused);
    CHECK(layers.time >= start);
    CHECK_FALSE(layers.clock.paused);
}

TEST_CASE("debug tools pause, step, speed up and rejoin real time", "[app][server]") {
    const World world;
    auto config = world.config();
    config.debug.port = 0;
    config.debug.time_control = true;
    const auto server = opened(config);
    using hog::probe::Action;
    using hog::probe::Clock;
    using hog::probe::Controlled;

    std::vector<Controlled> refused;
    Controlled sped;
    Controlled paused;
    hog::probe::Layers first;
    hog::probe::Layers again;
    hog::probe::Layers stepped;
    hog::probe::Layers still;
    Controlled rejoined;
    GameTime reached;
    Controlled ahead;
    steered(*server, [&](Ask& ask) {
        refused.push_back(ask.control(Action::Step, 1));
        refused.push_back(ask.control(Action::Speed, 0));
        refused.push_back(ask.control(Action::Speed, Server::fastest + 1));
        sped = ask.control(Action::Speed, 600);
        paused = ask.control(Action::Pause);
        first = ask.fetch();
        again = ask.fetch();
        refused.push_back(ask.control(Action::Step, 0));
        std::ignore = ask.control(Action::Step, 3);
        // Tools are still answered while it steps or catches up.
        const auto deadline = MonotonicClock::now() + 10s;
        do {
            stepped = ask.fetch();
        } while (stepped.time < first.time + 90s && MonotonicClock::now() < deadline);
        still = ask.fetch();
        rejoined = ask.control(Action::RealTime);
        do {
            reached = ask.fetch().time;
        } while (reached < start + 1h - 1s && MonotonicClock::now() < deadline);
        // Past the real time the test gives it, there's nothing to rejoin.
        std::ignore = ask.control(Action::Speed, Server::fastest);
        GameTime later = reached;
        while (later <= start + 1h && MonotonicClock::now() < deadline) {
            later = ask.fetch().time;
        }
        ahead = ask.control(Action::RealTime);
    });

    for (const Controlled& refusal : refused) {
        CHECK_FALSE(refusal.error.empty());
    }
    CHECK(sped == Controlled{.clock = Clock{.paused = false, .scale = 600}, .error = {}});
    CHECK(paused == Controlled{.clock = Clock{.paused = true, .scale = 600}, .error = {}});
    CHECK(first.time >= start);
    CHECK(first.clock == Clock{.paused = true, .scale = 600});
    CHECK(again.time == first.time);
    // Each tick at 20 Hz and 600x is 30 game seconds.
    CHECK(stepped.time == first.time + 90s);
    CHECK(still.time == stepped.time);
    CHECK(rejoined == Controlled{.clock = Clock{.paused = false, .scale = 1}, .error = {}});
    CHECK(reached > start + 1h - 1s);
    CHECK(reached < start + 1h + 10s);
    CHECK_FALSE(ahead.error.empty());
    CHECK(ahead.clock.scale == Server::fastest);
}

TEST_CASE("snapshots are taken every save interval", "[app][server]") {
    const World world;
    auto config = world.config();
    config.save_interval = 100ms;
    const auto server = opened(config);
    run(*server, 10);
    // Five periodic snapshots over 500 ms of game time, then one on stopping. A newer snapshot
    // may replace one still waiting to be written.
    CHECK(counter(*server, "hog_snapshots_written_total") +
              counter(*server, "hog_snapshots_replaced_total") ==
          6);
    CHECK(counter(*server, "hog_snapshots_failed_total") == 0);
}

TEST_CASE("metrics are logged every interval", "[app][server]") {
    const World world;
    auto config = world.config();
    config.metrics_interval = 1s;
    const auto server = opened(config);
    hog::app::metrics_log.set_threshold(hog::log::Level::Info);

    std::vector<std::string> lines;
    class Capture final : public hog::log::Sink {
    public:
        explicit Capture(std::vector<std::string>& lines) : lines_(&lines) {}
        void write(const hog::log::Record& record) override {
            if (record.category == "metrics") {
                lines_->push_back(hog::log::format_line(record));
            }
        }

    private:
        std::vector<std::string>* lines_;
    };
    const auto sink = hog::log::add_sink(std::make_unique<Capture>(lines));
    run(*server, 50);
    hog::log::remove_sink(sink);

    // At 1 s and 2 s of real time, then on stopping.
    REQUIRE(lines.size() == 3);
    CHECK(lines.back().find(" hog_ticks_total=50 ") != std::string::npos);
    CHECK(lines.back().find(" hog_snapshots_written_total=1 ") != std::string::npos);
}

TEST_CASE("another world's snapshots are refused", "[app][server]") {
    const World world;
    REQUIRE(opened(world.config())->save());
    world.write_planet(2);
    const auto server = Server::open(world.config(), start);
    REQUIRE_FALSE(server);
    CHECK(server.error().code == hog::ErrorCode::InvalidArgument);
}

TEST_CASE("a server reports what it can't open", "[app][server]") {
    const World world;
    auto config = world.config();
    config.planet = config.planet.parent_path() / "missing.toml";
    const auto missing = Server::open(config, start);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == hog::ErrorCode::NotFound);

    config = world.config();
    config.world = config.planet;
    const auto not_a_database = Server::open(config, start);
    REQUIRE_FALSE(not_a_database);
    CHECK(not_a_database.error().message.starts_with(config.world.string() + ": "));
}
