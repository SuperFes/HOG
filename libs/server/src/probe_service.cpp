#include <hog/core/time.hpp>
#include <hog/server/probe_service.hpp>

#include <chrono>
#include <format>
#include <variant>

namespace hog::server {

namespace {

using namespace std::chrono_literals;

// How often waiting threads look up to see whether the service is stopping.
constexpr std::chrono::milliseconds glance = 100ms;
// For a tool to say hello once connected.
constexpr std::chrono::milliseconds greeting = 5s;

} // namespace

Result<std::unique_ptr<ProbeService>> ProbeService::start(std::uint16_t port,
                                                          probe::Welcome welcome) {
    auto listener = probe::Listener::open(port);
    if (!listener) {
        return std::unexpected(std::move(listener.error()));
    }
    return std::make_unique<ProbeService>(Started{}, std::move(*listener), std::move(welcome));
}

ProbeService::ProbeService(Started /*unused*/, probe::Listener listener, probe::Welcome welcome)
    : listener_(std::move(listener)), welcome_(std::move(welcome)), port_(listener_.port()),
      listening_([this] { listen(); }) {}

ProbeService::~ProbeService() {
    stopping_.store(true);
    listening_.join();
    const std::scoped_lock lock{tools_mutex_};
    for (Tool& tool : tools_) {
        tool.thread.join();
    }
}

std::size_t ProbeService::tools() const {
    const std::scoped_lock lock{tools_mutex_};
    std::size_t talking = 0;
    for (const Tool& tool : tools_) {
        talking += tool.done.load() ? 0U : 1U;
    }
    return talking;
}

void ProbeService::listen() {
    while (!stopping_.load()) {
        reap();
        auto pending = listener_.pending(glance);
        if (!pending || !*pending) {
            continue;
        }
        auto socket = listener_.accept();
        if (!socket) {
            continue;
        }
        probe::Channel channel{std::move(*socket)};
        if (tools() >= most_tools) {
            std::ignore = channel.send(
                probe::Refused{.reason = std::format("already talking to {} tools", most_tools)});
            continue;
        }
        const std::scoped_lock lock{tools_mutex_};
        Tool& tool = tools_.emplace_back();
        tool.thread = std::thread{[this, &tool, channel = std::move(channel)] mutable {
            talk(std::move(channel));
            tool.done.store(true);
        }};
    }
}

void ProbeService::reap() {
    const std::scoped_lock lock{tools_mutex_};
    std::erase_if(tools_, [](Tool& tool) {
        if (!tool.done.load()) {
            return false;
        }
        tool.thread.join();
        return true;
    });
}

std::optional<probe::Message> ProbeService::first_message(probe::Channel& channel) const {
    const MonotonicClock::time_point deadline = MonotonicClock::now() + greeting;
    while (!stopping_.load() && MonotonicClock::now() < deadline) {
        auto message = channel.receive(glance);
        if (!message) {
            return std::nullopt;
        }
        if (message->has_value()) {
            return std::move(*message);
        }
    }
    return std::nullopt;
}

void ProbeService::talk(probe::Channel channel) {
    const std::optional<probe::Message> hello = first_message(channel);
    if (!hello) {
        return;
    }
    const auto* said = std::get_if<probe::Hello>(&*hello);
    if (said == nullptr) {
        std::ignore = channel.send(probe::Refused{.reason = "expected hello first"});
        return;
    }
    if (said->version != probe::protocol_version) {
        std::ignore = channel.send(
            probe::Refused{.reason = std::format("speaks protocol {}, not {}",
                                                 probe::protocol_version, said->version)});
        return;
    }
    if (!channel.send(welcome_)) {
        return;
    }
    while (!stopping_.load()) {
        auto message = channel.receive(glance);
        if (!message) {
            return;
        }
        if (!message->has_value()) {
            continue;
        }
        auto* fetch = std::get_if<probe::Fetch>(&**message);
        if (fetch == nullptr) {
            std::ignore = channel.send(probe::Refused{.reason = "expected a fetch"});
            return;
        }
        std::optional<probe::Layers> layers = ask(std::move(*fetch));
        if (!layers || !channel.send(*layers)) {
            return;
        }
    }
}

std::optional<probe::Layers> ProbeService::ask(probe::Fetch fetch) {
    std::future<probe::Layers> answer;
    {
        const std::scoped_lock lock{waiting_mutex_};
        Waiting& waiting = waiting_.emplace_back(Waiting{.fetch = std::move(fetch), .answer = {}});
        answer = waiting.answer.get_future();
        asked_.store(true, std::memory_order_release);
    }
    while (answer.wait_for(glance) != std::future_status::ready) {
        if (stopping_.load()) {
            return std::nullopt;
        }
    }
    return answer.get();
}

} // namespace hog::server
