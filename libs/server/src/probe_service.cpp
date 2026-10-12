#include <hog/core/time.hpp>
#include <hog/server/probe_service.hpp>
#include <hog/server/survey.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <variant>

namespace hog::server {

namespace {

using namespace std::chrono_literals;

// How often waiting threads look up to see whether the service is stopping.
constexpr std::chrono::milliseconds glance = 100ms;
// For a tool to say hello once connected.
constexpr std::chrono::milliseconds greeting = 5s;

// Half the machine, leaving the rest to the world's own jobs.
std::uint32_t survey_threads() noexcept {
    return std::max(1U, std::thread::hardware_concurrency() / 2);
}

} // namespace

Result<std::unique_ptr<ProbeService>>
ProbeService::start(std::uint16_t port, probe::Welcome welcome, Surveyor surveyor) {
    auto listener = probe::Listener::open(port);
    if (!listener) {
        return std::unexpected(std::move(listener.error()));
    }
    return std::make_unique<ProbeService>(Started{}, std::move(*listener), std::move(welcome),
                                          std::move(surveyor));
}

ProbeService::ProbeService(Started /*unused*/, probe::Listener listener, probe::Welcome welcome,
                           Surveyor surveyor)
    : listener_(std::move(listener)), welcome_(std::move(welcome)), surveyor_(std::move(surveyor)),
      port_(listener_.port()), listening_([this] { listen(); }) {}

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
    std::optional<jobs::JobSystem> jobs;
    while (!stopping_.load()) {
        auto message = channel.receive(glance);
        if (!message) {
            return;
        }
        if (!message->has_value()) {
            continue;
        }
        if (const auto* asked = std::get_if<probe::Survey>(&**message)) {
            if (!survey(channel, *asked, jobs)) {
                return;
            }
            continue;
        }
        std::optional<Request> request;
        if (auto* fetch = std::get_if<probe::Fetch>(&**message)) {
            request = std::move(*fetch);
        } else if (const auto* control = std::get_if<probe::Control>(&**message)) {
            request = *control;
        } else {
            std::ignore =
                channel.send(probe::Refused{.reason = "expected a fetch, control or survey"});
            return;
        }
        const std::optional<probe::Message> answer = ask(std::move(*request));
        if (!answer || !channel.send(*answer)) {
            return;
        }
    }
}

bool ProbeService::survey(probe::Channel& channel, const probe::Survey& survey,
                          std::optional<jobs::JobSystem>& jobs) const {
    if (!surveyor_) {
        std::ignore = channel.send(probe::Refused{.reason = "this server takes no surveys"});
        return false;
    }
    if (auto checked = check_survey(survey); !checked) {
        std::ignore = channel.send(probe::Refused{.reason = checked.error().message});
        return false;
    }
    if (!jobs) {
        jobs.emplace(survey_threads());
    }
    return channel.send(surveyor_(survey, *jobs)).has_value();
}

std::optional<probe::Message> ProbeService::ask(Request request) {
    std::future<probe::Message> answer;
    {
        const std::scoped_lock lock{waiting_mutex_};
        Waiting& waiting =
            waiting_.emplace_back(Waiting{.request = std::move(request), .answer = {}});
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
