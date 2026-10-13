#include <hog/probe/channel.hpp>
#include <hog/server/probe_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace hog;
using namespace hog::probe;
using namespace hog::server;
using namespace std::chrono_literals;

namespace {

Welcome sample_welcome() {
    return Welcome{.version = protocol_version,
                   .world_id = 42,
                   .level = 3,
                   .planet_radius = 1000.0,
                   .layers = {LayerInfo{.name = "a", .unit = "m"}}};
}

std::unique_ptr<ProbeService> started() {
    auto service = ProbeService::start(0, sample_welcome());
    REQUIRE(service);
    return std::move(*service);
}

Channel connected(const ProbeService& service) {
    auto socket = Socket::connect("127.0.0.1", service.port());
    REQUIRE(socket);
    return Channel{std::move(*socket)};
}

Message received(Channel& channel) {
    auto message = channel.receive(5s);
    REQUIRE(message);
    REQUIRE(message->has_value());
    return message->value_or(Message{});
}

Channel welcomed(const ProbeService& service) {
    Channel channel = connected(service);
    REQUIRE(channel.send(Hello{}));
    CHECK(received(channel) == Message{sample_welcome()});
    return channel;
}

// Serves fetches from this thread, as the tick thread does, until stopped.
class Ticking {
public:
    explicit Ticking(ProbeService& service)
        : thread_([this, &service] {
              while (!stop_.load()) {
                  service.serve(
                      [](const Fetch& fetch) {
                          Layers layers{
                              .time = GameTime{std::chrono::seconds{7}}, .clock = {}, .layers = {}};
                          for (const std::string& name : fetch.names) {
                              layers.layers.push_back(Layer{.name = name, .values = {1.0F, 2.0F}});
                          }
                          return layers;
                      },
                      [](const Control& control) {
                          return Controlled{.clock =
                                                Clock{.paused = control.action == Action::Pause,
                                                      .scale = control.amount},
                                            .error = {}};
                      },
                      [](const Inspect& inspect) {
                          return Inspection{.inspect = inspect,
                                            .time = GameTime{std::chrono::seconds{7}},
                                            .cell = 3,
                                            .values = {1.0F},
                                            .column = {}};
                      });
                  std::this_thread::sleep_for(1ms);
              }
          }) {}
    Ticking(const Ticking&) = delete;
    Ticking(Ticking&&) = delete;
    Ticking& operator=(const Ticking&) = delete;
    Ticking& operator=(Ticking&&) = delete;
    ~Ticking() {
        stop_.store(true);
        thread_.join();
    }

private:
    std::atomic<bool> stop_ = false;
    std::thread thread_;
};

} // namespace

TEST_CASE("a tool is welcomed and its fetches answered between ticks", "[server][probe]") {
    const auto service = started();
    Channel tool = welcomed(*service);
    const Ticking ticking{*service};
    REQUIRE(tool.send(Fetch{.names = {"a", "b"}}));
    const Message answer = received(tool);
    REQUIRE(std::holds_alternative<Layers>(answer));
    const auto& layers = std::get<Layers>(answer);
    CHECK(layers.time == GameTime{std::chrono::seconds{7}});
    REQUIRE(layers.layers.size() == 2);
    CHECK(layers.layers[1] == Layer{.name = "b", .values = {1.0F, 2.0F}});
}

TEST_CASE("a tool's controls are answered between ticks", "[server][probe]") {
    const auto service = started();
    Channel tool = welcomed(*service);
    const Ticking ticking{*service};
    REQUIRE(tool.send(Control{.action = Action::Pause, .amount = 5}));
    CHECK(received(tool) ==
          Message{Controlled{.clock = Clock{.paused = true, .scale = 5}, .error = {}}});
}

TEST_CASE("a tool's inspections are answered between ticks", "[server][probe]") {
    const auto service = started();
    Channel tool = welcomed(*service);
    const Inspect inspect{.latitude = 12.5, .longitude = -170.0};
    REQUIRE(tool.send(inspect));
    auto early = tool.receive(200ms);
    REQUIRE(early);
    CHECK_FALSE(early->has_value());
    const Ticking ticking{*service};
    const Message answer = received(tool);
    REQUIRE(std::holds_alternative<Inspection>(answer));
    CHECK(std::get<Inspection>(answer).inspect == inspect);
    CHECK(std::get<Inspection>(answer).cell == 3);
}

TEST_CASE("an inspection off the planet is refused", "[server][probe]") {
    const auto service = started();
    Channel tool = welcomed(*service);
    const Ticking ticking{*service};
    REQUIRE(tool.send(Inspect{.latitude = 91.0, .longitude = 0.0}));
    const Message answer = received(tool);
    REQUIRE(std::holds_alternative<Refused>(answer));
    CHECK(std::get<Refused>(answer).reason.contains("planet"));
    CHECK_FALSE(tool.receive(5s));
}

TEST_CASE("a tool may only fetch and control once welcomed", "[server][probe]") {
    const auto service = started();
    Channel tool = welcomed(*service);
    REQUIRE(tool.send(Hello{}));
    CHECK(std::holds_alternative<Refused>(received(tool)));
    CHECK_FALSE(tool.receive(5s));
}

TEST_CASE("a fetch waits for the next tick", "[server][probe]") {
    const auto service = started();
    Channel tool = welcomed(*service);
    REQUIRE(tool.send(Fetch{.names = {"a"}}));
    auto early = tool.receive(200ms);
    REQUIRE(early);
    CHECK_FALSE(early->has_value());
    const Ticking ticking{*service};
    CHECK(std::holds_alternative<Layers>(received(tool)));
}

TEST_CASE("a tool on another protocol is refused", "[server][probe]") {
    const auto service = started();
    Channel tool = connected(*service);
    REQUIRE(tool.send(Hello{.version = protocol_version + 1}));
    const Message answer = received(tool);
    REQUIRE(std::holds_alternative<Refused>(answer));
    CHECK_FALSE(std::get<Refused>(answer).reason.empty());
    CHECK_FALSE(tool.receive(5s));
}

TEST_CASE("a tool must say hello first", "[server][probe]") {
    const auto service = started();
    Channel tool = connected(*service);
    REQUIRE(tool.send(Fetch{}));
    CHECK(std::holds_alternative<Refused>(received(tool)));
    CHECK_FALSE(tool.receive(5s));
}

TEST_CASE("tools past the most at once are refused", "[server][probe]") {
    const auto service = started();
    std::vector<Channel> tools;
    tools.reserve(ProbeService::most_tools);
    for (std::size_t index = 0; index < ProbeService::most_tools; ++index) {
        tools.push_back(welcomed(*service));
    }
    CHECK(service->tools() == ProbeService::most_tools);
    Channel extra = connected(*service);
    CHECK(std::holds_alternative<Refused>(received(extra)));

    // Once one leaves, another can take its place.
    tools.pop_back();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (service->tools() == ProbeService::most_tools &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(service->tools() == ProbeService::most_tools - 1);
    tools.push_back(welcomed(*service));
}

TEST_CASE("stopping hangs up on tools, even mid-fetch", "[server][probe]") {
    auto service = started();
    Channel idle = welcomed(*service);
    Channel fetching = welcomed(*service);
    Channel silent = connected(*service);
    REQUIRE(fetching.send(Fetch{.names = {"a"}}));
    std::this_thread::sleep_for(50ms);
    const auto stopping = std::chrono::steady_clock::now();
    service.reset();
    CHECK(std::chrono::steady_clock::now() - stopping < 2s);
    CHECK_FALSE(idle.receive(5s));
    CHECK_FALSE(fetching.receive(5s));
    CHECK_FALSE(silent.receive(5s));
}

TEST_CASE("surveys are answered without waiting for a tick", "[server][probe]") {
    std::atomic<std::uint32_t> threads = 0;
    auto service = ProbeService::start(
        0, sample_welcome(), [&threads](const Survey& survey, jobs::JobSystem& jobs) {
            threads.store(jobs.thread_count());
            const std::size_t samples = std::size_t{survey.width} * survey.height;
            return Ground{.survey = survey,
                          .elevation = std::vector<float>(samples, 12.5F),
                          .biome = std::vector<std::uint8_t>(samples, 3),
                          .water_table = std::vector<float>(samples, 2.0F),
                          .river = std::vector<std::uint8_t>(samples, 0),
                          .rock = {},
                          .deposit = {},
                          .heat = {}};
        });
    REQUIRE(service);
    Channel tool = welcomed(**service);
    const Survey survey{.west = -10.0,
                        .east = 10.0,
                        .north = 5.0,
                        .south = -5.0,
                        .width = 4,
                        .height = 2,
                        .underground = false,
                        .depth = 0.0F};
    for (int twice = 0; twice < 2; ++twice) {
        REQUIRE(tool.send(survey));
        const Message answer = received(tool);
        REQUIRE(std::holds_alternative<Ground>(answer));
        CHECK(std::get<Ground>(answer).survey == survey);
        CHECK(std::get<Ground>(answer).elevation == std::vector<float>(8, 12.5F));
    }
    CHECK(threads.load() >= 1);
}

TEST_CASE("bad surveys, and surveys of a server without them, are refused", "[server][probe]") {
    const auto ground = [](const Survey& survey, jobs::JobSystem& /*jobs*/) {
        return Ground{.survey = survey,
                      .elevation = {},
                      .biome = {},
                      .water_table = {},
                      .river = {},
                      .rock = {},
                      .deposit = {},
                      .heat = {}};
    };
    auto service = ProbeService::start(0, sample_welcome(), ground);
    REQUIRE(service);
    Channel tool = welcomed(**service);
    REQUIRE(tool.send(Survey{.west = 10.0, .east = -10.0, .width = 1, .height = 1}));
    const Message answer = received(tool);
    REQUIRE(std::holds_alternative<Refused>(answer));
    CHECK(std::get<Refused>(answer).reason.contains("edges"));

    const auto plain = started();
    Channel other = welcomed(*plain);
    REQUIRE(other.send(Survey{.width = 1, .height = 1}));
    CHECK(std::holds_alternative<Refused>(received(other)));
}
