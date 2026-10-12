#pragma once

#include <hog/probe/messages.hpp>
#include <hog/viewer/frame.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hog::probe {
class Channel;
} // namespace hog::probe

namespace hog::viewer {

// Keeps a tool talking to a server from its own thread: connecting, saying hello, then fetching
// the layers it wants every so often, and connecting again whenever the server goes away. Clock
// controls go ahead of the next fetch, then the newest survey asked for; the point inspected, if
// any, is inspected again after each fetch.
class Link {
public:
    enum class Status : std::uint8_t {
        Connecting,
        Connected,
        Refused, // the server wouldn't talk; it's asked again later
        Lost,    // no server, or it went away; it's tried again later
    };

    struct State {
        Status status = Status::Connecting;
        std::string message; // why, when refused or lost
    };

    struct Timing {
        std::chrono::milliseconds fetch_every{1'000};
        std::chrono::milliseconds retry_after{1'000};
        // For a server to answer; it answers between ticks, which can be slow while catching up.
        std::chrono::milliseconds patience{30'000};
    };

    Link(std::string host, std::uint16_t port, Timing timing);
    Link(const Link&) = delete;
    Link(Link&&) = delete;
    Link& operator=(const Link&) = delete;
    Link& operator=(Link&&) = delete;
    ~Link();

    // The layers to fetch from now on, starting at once.
    void want(std::vector<std::string> layers);
    // Asks the server to change its clock, then fetches at once to show it. Dropped if the
    // server goes away first.
    void control(probe::Control control);

    // Asks for the ground under part of the planet, replacing any survey not yet sent.
    void survey(probe::Survey survey);

    // The point to inspect from now on, at once and after every fetch; none to stop.
    void inspect(std::optional<probe::Inspect> point);

    [[nodiscard]] State state() const;
    // Why the server didn't change its clock the last time it was asked, or empty if it did.
    [[nodiscard]] std::string control_error() const;
    // The newest frame, or null before the first.
    [[nodiscard]] std::shared_ptr<const Frame> frame() const;
    // Frames received so far, to tell when there's a new one.
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_.load(); }
    // The newest ground surveyed from the server talked to now, or null before the first.
    [[nodiscard]] std::shared_ptr<const probe::Ground> ground() const;
    [[nodiscard]] std::uint64_t grounds() const noexcept { return grounds_.load(); }
    // The newest inspection of the point inspected now, or null before the first.
    [[nodiscard]] std::shared_ptr<const probe::Inspection> inspection() const;

private:
    struct Greeting {
        probe::Welcome welcome;
        std::uint32_t cells = 0;
    };

    void run();
    // Talks to one server until it goes away or the link stops.
    void talk();
    // Says hello and hears what the server offers; none if it refused or went away.
    [[nodiscard]] std::optional<Greeting> greet(probe::Channel& channel);
    // The next message, waiting as long as a server may take; none if it went away or the link
    // is stopping.
    [[nodiscard]] std::optional<probe::Message> await(probe::Channel& channel);
    // Sends the controls waiting; false if the server went away.
    [[nodiscard]] bool send_controls(probe::Channel& channel);
    // Sends the survey waiting, if any; false if the server went away or refused it.
    [[nodiscard]] bool send_survey(probe::Channel& channel);
    // Inspects the point wanted, if any; false if the server went away or refused it.
    [[nodiscard]] bool send_inspect(probe::Channel& channel);
    // Waits until a fetch is due, the wanted layers change, or the link stops; true if stopping.
    bool wait(std::chrono::milliseconds most);
    void set_state(Status status, std::string message = {});

    std::string host_;
    std::uint16_t port_;
    Timing timing_;
    std::atomic<bool> stopping_ = false;
    std::atomic<std::uint64_t> frames_ = 0;
    std::atomic<std::uint64_t> grounds_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable woken_;
    State state_;
    std::vector<std::string> wanted_;
    std::vector<probe::Control> controls_;
    std::optional<probe::Survey> survey_;
    std::shared_ptr<const probe::Ground> ground_;
    std::optional<probe::Inspect> inspect_;
    std::shared_ptr<const probe::Inspection> inspection_;
    std::string control_error_;
    bool changed_ = false;
    std::shared_ptr<const Frame> frame_;
    // Last, so it starts after everything it uses exists.
    std::thread thread_;
};

[[nodiscard]] std::string_view to_string(Link::Status status) noexcept;

// Cells on a grid level, or none for a level no grid has.
[[nodiscard]] std::optional<std::uint32_t> cells_at(int level) noexcept;

} // namespace hog::viewer
