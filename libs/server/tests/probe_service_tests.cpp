#include <hog/probe/channel.hpp>
#include <hog/server/probe_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
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
                  service.serve([](const Fetch& fetch) {
                      Layers layers{
                          .time = GameTime{std::chrono::seconds{7}}, .clock = {}, .layers = {}};
                      for (const std::string& name : fetch.names) {
                          layers.layers.push_back(Layer{.name = name, .values = {1.0F, 2.0F}});
                      }
                      return layers;
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
