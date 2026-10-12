#pragma once

#include <hog/core/result.hpp>
#include <hog/probe/channel.hpp>
#include <hog/probe/messages.hpp>
#include <hog/probe/socket.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace hog::server {

// Answers debug tools on a loopback port while the server runs. Each tool talks on its own
// thread, but its fetches wait for the tick thread to answer them between ticks, so nothing else
// ever reads the world while it changes.
class ProbeService {
    // Lets start() construct through make_unique while nothing else can.
    struct Started {
        explicit Started() = default;
    };

public:
    // Tools connected at once; any more are refused.
    static constexpr std::size_t most_tools = 4;

    // Listens on `port`, or any free port for 0, offering what `welcome` describes.
    [[nodiscard]] static Result<std::unique_ptr<ProbeService>> start(std::uint16_t port,
                                                                     probe::Welcome welcome);

    ProbeService(Started /*unused*/, probe::Listener listener, probe::Welcome welcome);
    ProbeService(const ProbeService&) = delete;
    ProbeService(ProbeService&&) = delete;
    ProbeService& operator=(const ProbeService&) = delete;
    ProbeService& operator=(ProbeService&&) = delete;
    // Hangs up on every tool and waits for their threads.
    ~ProbeService();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::size_t tools() const;

    // Answers the fetches waiting, each with build(fetch).
    template <typename Build>
        requires std::same_as<std::invoke_result_t<Build&, const probe::Fetch&>, probe::Layers>
    void serve(Build build);

private:
    struct Waiting {
        probe::Fetch fetch;
        std::promise<probe::Layers> answer;
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
    // The answer to a fetch, or none if the service stopped first.
    [[nodiscard]] std::optional<probe::Layers> ask(probe::Fetch fetch);

    probe::Listener listener_;
    probe::Welcome welcome_;
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

template <typename Build>
    requires std::same_as<std::invoke_result_t<Build&, const probe::Fetch&>, probe::Layers>
void ProbeService::serve(Build build) {
    if (!asked_.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<Waiting> waiting;
    {
        const std::scoped_lock lock{waiting_mutex_};
        waiting.swap(waiting_);
        asked_.store(false, std::memory_order_release);
    }
    for (Waiting& fetch : waiting) {
        fetch.answer.set_value(build(std::as_const(fetch.fetch)));
    }
}

} // namespace hog::server
