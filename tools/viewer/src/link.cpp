#include <hog/core/time.hpp>
#include <hog/probe/channel.hpp>
#include <hog/probe/socket.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/viewer/link.hpp>

#include <format>
#include <utility>
#include <variant>

namespace hog::viewer {

namespace {

using namespace std::chrono_literals;

// How often a blocked wait looks up to see whether the link is stopping.
constexpr std::chrono::milliseconds glance = 50ms;

} // namespace

std::optional<std::uint32_t> cells_at(int level) noexcept {
    if (level < 0 || level > sim::CellGrid::max_level) {
        return std::nullopt;
    }
    return 6U << (2U * static_cast<unsigned>(level));
}

std::string_view to_string(Link::Status status) noexcept {
    switch (status) {
    case Link::Status::Connecting:
        return "connecting";
    case Link::Status::Connected:
        return "connected";
    case Link::Status::Refused:
        return "refused";
    case Link::Status::Lost:
        return "lost";
    }
    return "unknown";
}

Link::Link(std::string host, std::uint16_t port, Timing timing)
    : host_(std::move(host)), port_(port), timing_(timing), thread_([this] { run(); }) {}

Link::~Link() {
    {
        const std::scoped_lock lock{mutex_};
        stopping_.store(true);
    }
    woken_.notify_all();
    thread_.join();
}

void Link::want(std::vector<std::string> layers) {
    {
        const std::scoped_lock lock{mutex_};
        wanted_ = std::move(layers);
        changed_ = true;
    }
    woken_.notify_all();
}

Link::State Link::state() const {
    const std::scoped_lock lock{mutex_};
    return state_;
}

std::shared_ptr<const Frame> Link::frame() const {
    const std::scoped_lock lock{mutex_};
    return frame_;
}

void Link::set_state(Status status, std::string message) {
    const std::scoped_lock lock{mutex_};
    state_ = State{.status = status, .message = std::move(message)};
}

bool Link::wait(std::chrono::milliseconds most) {
    std::unique_lock lock{mutex_};
    woken_.wait_for(lock, most, [this] { return stopping_.load() || changed_; });
    changed_ = false;
    return stopping_.load();
}

void Link::run() {
    while (!stopping_.load()) {
        talk();
        if (stopping_.load()) {
            return;
        }
        // Waiting out the retry, unless stopped; wanting new layers doesn't hurry it.
        const MonotonicClock::time_point retry = MonotonicClock::now() + timing_.retry_after;
        while (!stopping_.load() && MonotonicClock::now() < retry) {
            std::unique_lock lock{mutex_};
            woken_.wait_for(lock, glance, [this] { return stopping_.load(); });
        }
    }
}

std::optional<probe::Message> Link::await(probe::Channel& channel) {
    const MonotonicClock::time_point deadline = MonotonicClock::now() + timing_.patience;
    while (!stopping_.load()) {
        if (MonotonicClock::now() >= deadline) {
            set_state(Status::Lost, "the server stopped answering");
            return std::nullopt;
        }
        auto message = channel.receive(glance);
        if (!message) {
            set_state(Status::Lost, message.error().message);
            return std::nullopt;
        }
        if (message->has_value()) {
            return std::move(*message);
        }
    }
    return std::nullopt;
}

std::optional<Link::Greeting> Link::greet(probe::Channel& channel) {
    if (auto sent = channel.send(probe::Hello{}); !sent) {
        set_state(Status::Lost, sent.error().message);
        return std::nullopt;
    }
    std::optional<probe::Message> greeting = await(channel);
    if (!greeting) {
        return std::nullopt;
    }
    if (const auto* refused = std::get_if<probe::Refused>(&*greeting)) {
        set_state(Status::Refused, refused->reason);
        return std::nullopt;
    }
    auto* welcome = std::get_if<probe::Welcome>(&*greeting);
    const std::optional<std::uint32_t> cells =
        welcome != nullptr ? cells_at(welcome->level) : std::nullopt;
    if (!cells) {
        set_state(Status::Lost, "the server didn't say hello properly");
        return std::nullopt;
    }
    return Greeting{.welcome = std::move(*welcome), .cells = *cells};
}

void Link::talk() {
    set_state(Status::Connecting);
    auto socket = probe::Socket::connect(host_, port_);
    if (!socket) {
        set_state(Status::Lost, socket.error().message);
        return;
    }
    probe::Channel channel{std::move(*socket)};
    const std::optional<Greeting> greeting = greet(channel);
    if (!greeting) {
        return;
    }
    set_state(Status::Connected);
    while (!stopping_.load()) {
        probe::Fetch fetch;
        {
            const std::scoped_lock lock{mutex_};
            fetch.names = wanted_;
        }
        if (auto sent = channel.send(fetch); !sent) {
            set_state(Status::Lost, sent.error().message);
            return;
        }
        const std::optional<probe::Message> answer = await(channel);
        if (!answer) {
            return;
        }
        const auto* layers = std::get_if<probe::Layers>(&*answer);
        if (layers == nullptr) {
            set_state(Status::Lost, "the server answered a fetch with something else");
            return;
        }
        auto frame = std::make_shared<const Frame>(greeting->welcome, *layers, greeting->cells);
        {
            const std::scoped_lock lock{mutex_};
            frame_ = std::move(frame);
        }
        frames_.fetch_add(1);
        if (wait(timing_.fetch_every)) {
            return;
        }
    }
}

} // namespace hog::viewer
