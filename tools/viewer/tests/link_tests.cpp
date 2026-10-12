#include <hog/server/probe_service.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/viewer/link.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace hog;
using namespace std::chrono_literals;
using hog::viewer::Link;

namespace {

constexpr int level = 1;
constexpr std::uint32_t cells = 24;
constexpr Link::Timing quick{.fetch_every = 20ms, .retry_after = 20ms, .patience = 5s};

probe::Welcome welcome() {
    return probe::Welcome{.version = probe::protocol_version,
                          .world_id = 9,
                          .level = level,
                          .planet_radius = 1.0,
                          .layers = {probe::LayerInfo{.name = "a", .unit = ""},
                                     probe::LayerInfo{.name = "b", .unit = ""}}};
}

// Ground at the depth surveyed, which is as high as it is deep.
probe::Ground surveyed(const probe::Survey& survey, jobs::JobSystem& /*jobs*/) {
    const std::size_t samples = std::size_t{survey.width} * survey.height;
    return probe::Ground{.survey = survey,
                         .elevation = std::vector<float>(samples, survey.depth),
                         .biome = std::vector<std::uint8_t>(samples, 0),
                         .rock = {},
                         .deposit = {}};
}

// A probe service answering from its own thread, as a server's tick thread would: every layer
// asked for, filled with how many fetches it has answered.
class Server {
public:
    explicit Server(std::uint16_t port = 0) {
        auto started = server::ProbeService::start(port, welcome(), surveyed);
        REQUIRE(started);
        service_ = std::move(*started);
        thread_ = std::thread{[this] {
            while (!stop_.load()) {
                service_->serve(
                    [this](const probe::Fetch& fetch) {
                        const auto answered = static_cast<float>(++answered_);
                        probe::Layers layers{.time = GameTime{std::chrono::seconds{answered_}},
                                             .clock = clock_,
                                             .layers = {}};
                        for (const std::string& name : fetch.names) {
                            layers.layers.push_back(probe::Layer{
                                .name = name, .values = std::vector<float>(cells, answered)});
                        }
                        return layers;
                    },
                    [this](const probe::Control& control) {
                        if (control.action == probe::Action::Speed && control.amount == 0) {
                            return probe::Controlled{.clock = clock_, .error = "too slow"};
                        }
                        if (control.action == probe::Action::Speed) {
                            clock_.scale = control.amount;
                        }
                        clock_.paused = control.action == probe::Action::Pause ||
                                        (clock_.paused && control.action != probe::Action::Resume);
                        return probe::Controlled{.clock = clock_, .error = {}};
                    },
                    [this](const probe::Inspect& inspect) {
                        return probe::Inspection{
                            .inspect = inspect,
                            .time = GameTime{std::chrono::seconds{answered_}},
                            .cell = 1,
                            .values = std::vector<float>(welcome().layers.size(),
                                                         static_cast<float>(answered_)),
                            .column = {}};
                    });
                std::this_thread::sleep_for(1ms);
            }
        }};
    }
    Server(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(const Server&) = delete;
    Server& operator=(Server&&) = delete;
    ~Server() {
        stop_.store(true);
        thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const { return service_->port(); }

private:
    std::unique_ptr<server::ProbeService> service_;
    std::atomic<bool> stop_ = false;
    int answered_ = 0;
    probe::Clock clock_;
    std::thread thread_;
};

// Waits up to a few seconds for something to come true.
bool eventually(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

} // namespace

TEST_CASE("cells per grid level", "[viewer][link]") {
    CHECK(viewer::cells_at(level) == cells);
    CHECK(viewer::cells_at(0) == 6U);
    CHECK(viewer::cells_at(5) == 6'144U);
    CHECK(viewer::cells_at(sim::CellGrid::max_level) ==
          sim::CellGrid{sim::CellGrid::max_level}.size());
    CHECK_FALSE(viewer::cells_at(-1));
    CHECK_FALSE(viewer::cells_at(sim::CellGrid::max_level + 1));
}

TEST_CASE("a link fetches the layers it wants, again and again", "[viewer][link]") {
    const Server server;
    Link link{"127.0.0.1", server.port(), quick};
    link.want({"a"});
    REQUIRE(eventually([&] { return link.frames() >= 3; }));
    CHECK(link.state().status == Link::Status::Connected);
    const auto frame = link.frame();
    REQUIRE(frame != nullptr);
    CHECK(frame->welcome() == welcome());
    CHECK(frame->layer("a").size() == cells);
    CHECK(frame->layer("b").empty());

    link.want({"b"});
    CHECK(eventually([&] { return !link.frame()->layer("b").empty(); }));
}

TEST_CASE("a link sends clock controls ahead of the next fetch", "[viewer][link]") {
    const Server server;
    // Slow fetches, so only a control hurries the next one.
    Link link{"127.0.0.1", server.port(),
              Link::Timing{.fetch_every = 1h, .retry_after = 10ms, .patience = 5s}};
    REQUIRE(eventually([&] { return link.frames() >= 1; }));
    const auto clock = [&] {
        const auto frame = link.frame();
        return frame != nullptr ? frame->clock() : probe::Clock{};
    };
    link.control(probe::Control{.action = probe::Action::Pause, .amount = 0});
    CHECK(eventually([&] { return clock().paused; }));
    link.control(probe::Control{.action = probe::Action::Speed, .amount = 0});
    CHECK(eventually([&] { return link.control_error() == "too slow"; }));
    link.control(probe::Control{.action = probe::Action::Speed, .amount = 60});
    CHECK(eventually([&] { return clock() == probe::Clock{.paused = true, .scale = 60}; }));
    CHECK(link.control_error().empty());
}

TEST_CASE("a link waits for a server and finds it again", "[viewer][link]") {
    std::uint16_t port = 0;
    {
        const Server first;
        port = first.port();
    }
    Link link{"127.0.0.1", port, quick};
    link.want({"a"});
    REQUIRE(eventually([&] { return link.state().status == Link::Status::Lost; }));
    CHECK_FALSE(link.state().message.empty());
    {
        const Server server{port};
        REQUIRE(eventually([&] { return link.frames() >= 1; }));
        CHECK(link.state().status == Link::Status::Connected);
    }
    CHECK(eventually([&] { return link.state().status != Link::Status::Connected; }));
    // The last frame stays until there's a newer one.
    CHECK(link.frame() != nullptr);
}

TEST_CASE("a refused link says why", "[viewer][link]") {
    const Server server;
    std::vector<std::unique_ptr<Link>> links;
    for (std::size_t index = 0; index <= server::ProbeService::most_tools; ++index) {
        links.push_back(std::make_unique<Link>("127.0.0.1", server.port(), quick));
    }
    CHECK(eventually([&] {
        return std::ranges::any_of(
            links, [](const auto& link) { return link->state().status == Link::Status::Refused; });
    }));
}

TEST_CASE("a link stops promptly, even while waiting on a server", "[viewer][link]") {
    const auto silent = probe::Listener::open(0);
    REQUIRE(silent);
    auto link = std::make_unique<Link>(
        "127.0.0.1", silent->port(),
        Link::Timing{.fetch_every = 1s, .retry_after = 10s, .patience = 10s});
    std::this_thread::sleep_for(50ms);
    const auto stopping = std::chrono::steady_clock::now();
    link.reset();
    CHECK(std::chrono::steady_clock::now() - stopping < 1s);
}

TEST_CASE("a link surveys the ground without waiting for the next fetch", "[viewer][link]") {
    const Server server;
    Link link{"127.0.0.1", server.port(),
              Link::Timing{.fetch_every = 1h, .retry_after = 10ms, .patience = 5s}};
    REQUIRE(eventually([&] { return link.frames() >= 1; }));
    CHECK(link.ground() == nullptr);
    const std::uint64_t before = link.grounds();
    probe::Survey survey{.width = 4, .height = 2, .depth = 30.0F};
    link.survey(survey);
    REQUIRE(eventually([&] { return link.ground() != nullptr; }));
    CHECK(link.grounds() > before);
    CHECK(link.ground()->survey == survey);
    CHECK(link.ground()->elevation == std::vector<float>(8, 30.0F));
    CHECK(link.state().status == Link::Status::Connected);

    survey.depth = 60.0F;
    link.survey(survey);
    CHECK(eventually([&] { return link.ground()->survey.depth == 60.0F; }));
}

TEST_CASE("a refused survey says why", "[viewer][link]") {
    const Server server;
    Link link{"127.0.0.1", server.port(),
              Link::Timing{.fetch_every = 1h, .retry_after = 1h, .patience = 5s}};
    REQUIRE(eventually([&] { return link.frames() >= 1; }));
    link.survey(probe::Survey{.width = 0, .height = 0});
    REQUIRE(eventually([&] { return link.state().status == Link::Status::Refused; }));
    CHECK_FALSE(link.state().message.empty());
}
