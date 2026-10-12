#pragma once

#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/probe/channel.hpp>
#include <hog/probe/messages.hpp>
#include <hog/probe/socket.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace hog::server {

// Answers debug tools on a loopback port while the server runs. Each tool talks on its own
// thread, but its fetches, controls and inspections wait for the tick thread to answer them between
// ticks, so nothing else ever touches the world while it changes. Surveys read only what never
// changes, so the tool's thread answers them itself, with its own jobs, rather than hold up a tick.
class ProbeService {
    // Lets start() construct through make_unique while nothing else can.
    struct Started {
        explicit Started() = default;
    };

public:
    // Answers a checked survey; empty when the server takes none.
    using Surveyor = std::function<probe::Ground(const probe::Survey&, jobs::JobSystem&)>;

    // Tools connected at once; any more are refused.
    static constexpr std::size_t most_tools = 4;

    // Listens on `port`, or any free port for 0, offering what `welcome` describes.
    [[nodiscard]] static Result<std::unique_ptr<ProbeService>>
    start(std::uint16_t port, probe::Welcome welcome, Surveyor surveyor = {});

    ProbeService(Started /*unused*/, probe::Listener listener, probe::Welcome welcome,
                 Surveyor surveyor);
    ProbeService(const ProbeService&) = delete;
    ProbeService(ProbeService&&) = delete;
    ProbeService& operator=(const ProbeService&) = delete;
    ProbeService& operator=(ProbeService&&) = delete;
    // Hangs up on every tool and waits for their threads.
    ~ProbeService();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::size_t tools() const;

    // Answers what is waiting: each fetch with build(fetch), each control with steer(control),
    // and each checked inspection with look(inspect).
    template <typename Build, typename Steer, typename Look>
        requires std::same_as<std::invoke_result_t<Build&, const probe::Fetch&>, probe::Layers> &&
                 std::same_as<std::invoke_result_t<Steer&, const probe::Control&>,
                              probe::Controlled> &&
                 std::same_as<std::invoke_result_t<Look&, const probe::Inspect&>, probe::Inspection>
    void serve(Build build, Steer steer, Look look);

private:
    using Request = std::variant<probe::Fetch, probe::Control, probe::Inspect>;
    struct Waiting {
        Request request;
        std::promise<probe::Message> answer;
    };
    struct Tool {
        std::thread thread;
        std::atomic<bool> done = false;
    };

    void listen();
    // Joins the threads of tools that have gone.
    void reap();
    // What a tool says first, waiting a while for it; none if it went quiet or away.
    [[nodiscard]] std::optional<probe::Message> first_message(probe::Channel& channel) const;
    void talk(probe::Channel channel);
    // Answers a survey on the tool's thread, starting its jobs on the first; false to hang up.
    [[nodiscard]] bool survey(probe::Channel& channel, const probe::Survey& survey,
                              std::optional<jobs::JobSystem>& jobs) const;
    // The tick thread's answer, or none if the service stopped first.
    [[nodiscard]] std::optional<probe::Message> ask(Request request);

    probe::Listener listener_;
    probe::Welcome welcome_;
    Surveyor surveyor_;
    std::uint16_t port_;
    std::atomic<bool> stopping_ = false;
    std::atomic<bool> asked_ = false;
    std::mutex waiting_mutex_;
    std::vector<Waiting> waiting_;
    mutable std::mutex tools_mutex_;
    std::list<Tool> tools_;
    // Last, so it starts after everything it uses exists.
    std::thread listening_;
};

template <typename Build, typename Steer, typename Look>
    requires std::same_as<std::invoke_result_t<Build&, const probe::Fetch&>, probe::Layers> &&
             std::same_as<std::invoke_result_t<Steer&, const probe::Control&>, probe::Controlled> &&
             std::same_as<std::invoke_result_t<Look&, const probe::Inspect&>, probe::Inspection>
void ProbeService::serve(Build build, Steer steer, Look look) {
    if (!asked_.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<Waiting> waiting;
    {
        const std::scoped_lock lock{waiting_mutex_};
        waiting.swap(waiting_);
        asked_.store(false, std::memory_order_release);
    }
    for (Waiting& asked : waiting) {
        asked.answer.set_value(std::visit(
            [&](const auto& request) -> probe::Message {
                using Asked = std::decay_t<decltype(request)>;
                if constexpr (std::same_as<Asked, probe::Fetch>) {
                    return build(request);
                } else if constexpr (std::same_as<Asked, probe::Control>) {
                    return steer(request);
                } else {
                    return look(request);
                }
            },
            std::as_const(asked.request)));
    }
}

} // namespace hog::server
