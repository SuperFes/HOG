#include <hog/server/server_config.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

using hog::ErrorCode;
using hog::server::parse_server_config;
using namespace std::chrono_literals;

namespace {

std::filesystem::path base() {
    return "/srv/hog/config";
}

constexpr std::string_view minimal = R"(
[server]
world = "world.db"
planet = "planet.toml"
)";

std::string with(std::string_view extra) {
    return std::string{minimal} + std::string{extra};
}

} // namespace

TEST_CASE("a minimal server config takes the defaults", "[server][config]") {
    const auto config = parse_server_config(minimal, base());
    REQUIRE(config);
    CHECK(config->world == base() / "world.db");
    CHECK(config->planet == base() / "planet.toml");
    CHECK(config->tick_rate == hog::server::idle_rate);
    CHECK(config->max_burst == 5);
    CHECK(config->save_interval == 5min);
    CHECK(config->keep_snapshots == 3);
    CHECK(config->catch_up_ticks == 2'000);
    CHECK(config->log.level == hog::log::Level::Info);
    CHECK(config->log.categories.empty());
    CHECK_FALSE(config->log.file);
    CHECK(config->metrics_interval == 10s);
    CHECK(config->weather.cell_size == 6'000.0);
    CHECK(config->weather.step == 10min);
    CHECK(config->weather.day == 24h);
    CHECK_FALSE(config->weather.earth_pace);
    CHECK_FALSE(config->wildlife.ecology);
    CHECK(config->wildlife.step == 24h);
    CHECK_FALSE(config->forest.vegetation);
    CHECK(config->forest.step == 1h);
    CHECK_FALSE(config->forest.burned_years_ago);
    CHECK_FALSE(config->net.port);
    CHECK_FALSE(config->net.login_key);
    CHECK(config->net.simulate.off());
    CHECK(config->net.view.radius == 500.0);
    CHECK(config->net.view.budget == 1'200);
    CHECK_FALSE(config->debug.port);
    CHECK_FALSE(config->debug.time_control);
    CHECK_FALSE(config->debug.start);
}

TEST_CASE("every server setting can be given", "[server][config]") {
    const auto config = parse_server_config(R"(
[server]
world = "../var/world.db"
planet = "/etc/hog/planet.toml"
tick_rate = 30
max_burst = 2

[save]
interval_minutes = 0.5
keep = 7
catch_up_ticks = 500

[log]
level = "debug"
file = "../var/logs/server.jsonl"
categories = { save = "trace", net = "off" }

[metrics]
interval_seconds = 0

[weather]
cell_km = 2.5
step_minutes = 5
day_hours = 4
earth_pace = true

[wildlife]
ecology = "ecology.toml"
step_hours = 6

[forest]
vegetation = "forest.toml"
step_hours = 0.5
burned_years_ago = 8

[net]
port = 27100
login_key = "../var/login.pub"
simulate = "lag=75,loss=2"
view_m = 250.5
snapshot_bytes = 900

[debug]
port = 7777
time_control = true
start = 2030-06-01T12:00:00+02:00
)",
                                            base());
    REQUIRE(config);
    CHECK(config->world == std::filesystem::path{"/srv/hog/var/world.db"});
    CHECK(config->planet == std::filesystem::path{"/etc/hog/planet.toml"});
    CHECK(config->tick_rate == 30);
    CHECK(config->max_burst == 2);
    CHECK(config->save_interval == 30s);
    CHECK(config->keep_snapshots == 7);
    CHECK(config->catch_up_ticks == 500);
    CHECK(config->log.level == hog::log::Level::Debug);
    REQUIRE(config->log.categories.size() == 2);
    using Override = std::pair<std::string, hog::log::Level>;
    const auto has = [&](const Override& override) {
        return std::ranges::find(config->log.categories, override) != config->log.categories.end();
    };
    CHECK(has(Override{"save", hog::log::Level::Trace}));
    CHECK(has(Override{"net", hog::log::Level::Off}));
    CHECK(config->log.file == std::filesystem::path{"/srv/hog/var/logs/server.jsonl"});
    CHECK(config->metrics_interval == 0s);
    CHECK(config->weather.cell_size == 2'500.0);
    CHECK(config->weather.step == 5min);
    CHECK(config->weather.day == 4h);
    CHECK(config->weather.earth_pace);
    CHECK(config->wildlife.ecology == base() / "ecology.toml");
    CHECK(config->wildlife.step == 6h);
    CHECK(config->forest.vegetation == base() / "forest.toml");
    CHECK(config->forest.step == 30min);
    CHECK(config->forest.burned_years_ago == 8.0);
    CHECK(config->net.port == 27100);
    CHECK(config->net.login_key == (base() / "../var/login.pub").lexically_normal());
    CHECK(config->net.simulate == hog::net::Simulation{.lag = 75ms, .loss = 2});
    CHECK(config->net.view.radius == 250.5);
    CHECK(config->net.view.budget == 900);
    CHECK(config->debug.port == 7777);
    CHECK(config->debug.time_control);
    CHECK(config->debug.start ==
          hog::to_game_time(std::chrono::sys_days{std::chrono::year{2030} / 6 / 1} + 10h));
}

TEST_CASE("bad server configs are rejected", "[server][config]") {
    const auto [text, code] = GENERATE(table<std::string, ErrorCode>({
        {"[server", ErrorCode::Parse},
        {"", ErrorCode::InvalidArgument},
        {"[server]\nworld = \"w.db\"", ErrorCode::InvalidArgument},
        {"[server]\nworld = \"\"\nplanet = \"p.toml\"", ErrorCode::InvalidArgument},
        {"[server]\nworld = 1\nplanet = \"p.toml\"", ErrorCode::InvalidArgument},
        {with("[sever]\n"), ErrorCode::InvalidArgument},
        {with("[save]\nkeeps = 3\n"), ErrorCode::InvalidArgument},
        {"wat = 1\n" + with(""), ErrorCode::InvalidArgument},
        {with("tick_rate = 0\n"), ErrorCode::InvalidArgument},
        {with("tick_rate = 61\n"), ErrorCode::InvalidArgument},
        {with("tick_rate = \"fast\"\n"), ErrorCode::InvalidArgument},
        {with("max_burst = 0\n"), ErrorCode::InvalidArgument},
        {with("[save]\ninterval_minutes = 0\n"), ErrorCode::InvalidArgument},
        {with("[save]\ninterval_minutes = -1\n"), ErrorCode::InvalidArgument},
        {with("[save]\ninterval_minutes = nan\n"), ErrorCode::InvalidArgument},
        {with("[save]\ninterval_minutes = inf\n"), ErrorCode::InvalidArgument},
        {with("[save]\ninterval_minutes = 1e-12\n"), ErrorCode::InvalidArgument},
        {with("[save]\ninterval_minutes = 1441\n"), ErrorCode::InvalidArgument},
        {with("[save]\ninterval_minutes = \"5\"\n"), ErrorCode::InvalidArgument},
        {with("[save]\nkeep = 0\n"), ErrorCode::InvalidArgument},
        {with("[save]\ncatch_up_ticks = 0\n"), ErrorCode::InvalidArgument},
        {with("[log]\nlevel = \"loud\"\n"), ErrorCode::InvalidArgument},
        {with("[log]\nlevel = 3\n"), ErrorCode::InvalidArgument},
        {with("[log]\ncategories = { save = 1 }\n"), ErrorCode::InvalidArgument},
        {with("[log]\ncategories = \"save\"\n"), ErrorCode::InvalidArgument},
        {with("[metrics]\ninterval_seconds = -1\n"), ErrorCode::InvalidArgument},
        {with("[metrics]\ninterval_seconds = 1.5\n"), ErrorCode::InvalidArgument},
        {with("[weather]\ncell_km = 0\n"), ErrorCode::InvalidArgument},
        {with("[weather]\ncell_km = nan\n"), ErrorCode::InvalidArgument},
        {with("[weather]\nstep_minutes = 0\n"), ErrorCode::InvalidArgument},
        {with("[weather]\nstep_minutes = 1441\n"), ErrorCode::InvalidArgument},
        {with("[weather]\nday_hours = -4\n"), ErrorCode::InvalidArgument},
        {with("[weather]\nday_hours = inf\n"), ErrorCode::InvalidArgument},
        {with("[weather]\nwind = 3\n"), ErrorCode::InvalidArgument},
        {with("[wildlife]\necology = 3\n"), ErrorCode::InvalidArgument},
        {with("[wildlife]\nstep_hours = 0\n"), ErrorCode::InvalidArgument},
        {with("[wildlife]\nspecies = \"a.toml\"\n"), ErrorCode::InvalidArgument},
        {with("[forest]\nvegetation = 3\n"), ErrorCode::InvalidArgument},
        {with("[forest]\nstep_hours = 25\n"), ErrorCode::InvalidArgument},
        {with("[forest]\nburned_years_ago = -1\n"), ErrorCode::InvalidArgument},
        {with("[forest]\nburned_years_ago = 1001\n"), ErrorCode::InvalidArgument},
        {with("[forest]\nfire = \"a.toml\"\n"), ErrorCode::InvalidArgument},
        {with("[net]\nport = 0\n"), ErrorCode::InvalidArgument},
        {with("[net]\nport = 65536\n"), ErrorCode::InvalidArgument},
        {with("[net]\naddress = \"0.0.0.0\"\n"), ErrorCode::InvalidArgument},
        {with("[net]\nlogin_key = 3\n"), ErrorCode::InvalidArgument},
        {with("[net]\nsimulate = \"lag=-5\"\n"), ErrorCode::InvalidArgument},
        {with("[net]\nsimulate = 5\n"), ErrorCode::InvalidArgument},
        {with("[net]\nview_m = 0\n"), ErrorCode::InvalidArgument},
        {with("[net]\nview_m = 10001\n"), ErrorCode::InvalidArgument},
        {with("[net]\nview_m = nan\n"), ErrorCode::InvalidArgument},
        {with("[net]\nview_m = \"far\"\n"), ErrorCode::InvalidArgument},
        {with("[net]\nsnapshot_bytes = 63\n"), ErrorCode::InvalidArgument},
        {with("[net]\nsnapshot_bytes = 65537\n"), ErrorCode::InvalidArgument},
        {with("[net]\nsnapshot_bytes = 1200.5\n"), ErrorCode::InvalidArgument},
        {with("[debug]\nport = 0\n"), ErrorCode::InvalidArgument},
        {with("[debug]\nport = 65536\n"), ErrorCode::InvalidArgument},
        {with("[debug]\nport = \"7777\"\n"), ErrorCode::InvalidArgument},
        {with("[debug]\nhost = \"0.0.0.0\"\n"), ErrorCode::InvalidArgument},
        {with("[debug]\ntime_control = \"yes\"\n"), ErrorCode::InvalidArgument},
        {with("[debug]\nstart = 2030-06-01T12:00:00\n"), ErrorCode::InvalidArgument},
        {with("[debug]\nstart = 2030-06-01\n"), ErrorCode::InvalidArgument},
    }));
    CAPTURE(text);
    const auto config = parse_server_config(text, base());
    REQUIRE_FALSE(config);
    CHECK(config.error().code == code);
}

TEST_CASE("errors name the setting", "[server][config]") {
    const auto config = parse_server_config(with("[save]\nkeep = 0\n"), base());
    REQUIRE_FALSE(config);
    CHECK(config.error().message == "save.keep must be an integer from 1 to 1000");
}

TEST_CASE("server configs load relative to their file", "[server][config]") {
    const hog::testing::TemporaryDirectory directory;
    const auto path = directory.path() / "server.toml";
    std::ofstream{path} << minimal;
    const auto config = hog::server::load_server_config(path);
    REQUIRE(config);
    CHECK(config->world == directory.path() / "world.db");

    std::ofstream{path} << "[server]\n";
    const auto broken = hog::server::load_server_config(path);
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.starts_with(path.string() + ": "));

    const auto missing = hog::server::load_server_config(directory.path() / "missing.toml");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("the development server config loads", "[server][config]") {
    const auto config = hog::server::load_server_config(HOG_SOURCE_DIR "/config/server.toml");
    REQUIRE(config);
    CHECK(config->planet.filename() == "planet.toml");
    CHECK(std::filesystem::exists(config->planet));
    CHECK(std::filesystem::exists(config->wildlife.ecology.value_or("")));
    CHECK(std::filesystem::exists(config->forest.vegetation.value_or("")));
    CHECK(config->net.port);
    CHECK(config->debug.port);
}
