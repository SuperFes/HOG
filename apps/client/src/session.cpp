#include <hog/client/session.hpp>
#include <hog/core/log.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>
#include <variant>

namespace hog::client {

namespace {

const log::Category session_log{"connection"};

// Lost, as a percentage, of a fraction delivered; "?" while it's unknown.
std::string lost(float delivered) {
    return delivered < 0.0F ? std::string{"?"}
                            : std::format("{:.0f}%", (1.0F - delivered) * 100.0F);
}

std::string_view name_of(Session::State state) {
    switch (state) {
    case Session::State::Connecting:
        return "connecting";
    case Session::State::Welcomed:
        return "welcomed";
    case Session::State::Refused:
        return "refused";
    case Session::State::Closed:
        return "closed";
    }
    return "?";
}

} // namespace

Result<Session> Session::connect(std::string address, std::vector<std::byte> token) {
    auto client = net::Client::connect(address);
    if (!client) {
        return std::unexpected(client.error());
    }
    log::info(session_log, "connecting to {}", address);
    return Session{std::move(*client), std::move(address), std::move(token)};
}

Session::Session(net::Client client, std::string address, std::vector<std::byte> token) noexcept
    : client_(std::move(client)), address_(std::move(address)), token_(std::move(token)),
      counting_since_(MonotonicClock::now()) {}

void Session::look(std::optional<WorldPosition> feet, MonotonicClock::time_point now) {
    client_.poll(events_);
    for (const net::Event& event : events_) {
        if (std::holds_alternative<net::Connected>(event)) {
            log::info(session_log, "connected to {}", address_);
            send(protocol::Hello{.version = protocol::version, .token = token_},
                 net::Delivery::Reliable);
        } else if (const auto* message = std::get_if<net::Message>(&event)) {
            heard(message->bytes(), now);
        } else if (const auto* lost = std::get_if<net::Disconnected>(&event)) {
            log::warn(session_log, "disconnected from {}: {}", address_, lost->reason);
            if (state_ != State::Refused) {
                state_ = State::Closed;
                reason_ = lost->reason;
            }
        }
    }
    if (state_ == State::Welcomed && feet && (!next_move_ || now >= *next_move_)) {
        send(protocol::Moved{.sequence = ++moves_,
                             .corrections = corrections_,
                             .at = protocol::to_millimetres(*feet)},
             net::Delivery::Unreliable);
        next_move_ = next_move_.value_or(now) + move_interval;
        // After a stall, carry on from now rather than send the missed ones at once.
        if (*next_move_ <= now) {
            next_move_ = now + move_interval;
        }
    }
    if (now - counting_since_ >= std::chrono::seconds{1}) {
        rate_ = counted_;
        counted_ = 0;
        counting_since_ = now;
    }
}

void Session::heard(std::span<const std::byte> bytes, MonotonicClock::time_point now) {
    const auto message = protocol::decode(bytes);
    if (!message) {
        log::warn(session_log, "{} sent something unreadable: {}", address_, message.error());
    } else if (std::holds_alternative<protocol::Welcome>(*message)) {
        log::info(session_log, "welcomed by {}", address_);
        state_ = State::Welcomed;
    } else if (const auto* refused = std::get_if<protocol::Refused>(&*message)) {
        reason_ = protocol::explain(*refused);
        log::warn(session_log, "{}", reason_);
        state_ = State::Refused;
    } else if (const auto* snapshot = std::get_if<protocol::Snapshot>(&*message)) {
        const auto applied = replica_.receive(*snapshot);
        if (!applied) {
            log::warn(session_log, "{} sent a snapshot that doesn't fit: {}", address_,
                      applied.error());
        } else if (*applied) {
            send(protocol::Ack{.tick = snapshot->tick}, net::Delivery::Unreliable);
            last_snapshot_ = now;
            ++counted_;
        }
    } else if (const auto* corrected = std::get_if<protocol::Corrected>(&*message)) {
        if (corrected->count > corrections_) {
            corrections_ = corrected->count;
            correction_ = protocol::to_world(corrected->at);
            log::debug(session_log, "{} put the player back", address_);
        }
    } else {
        log::debug(session_log, "{} sent a message out of place", address_);
    }
}

void Session::send(const protocol::Message& message, net::Delivery delivery) {
    if (auto sent = client_.send(delivery, protocol::encode(message)); !sent) {
        log::warn(session_log, "couldn't send to {}: {}", address_, sent.error());
    }
}

SessionSummary summarise(const Session& session, WorldPosition eye,
                         MonotonicClock::time_point now) {
    SessionSummary summary;
    summary.connection = std::format("{}, {}", session.address(), name_of(session.state()));
    if (!session.reason().empty()) {
        summary.connection += std::format(": {}", session.reason());
    }
    if (const auto stats = session.stats()) {
        summary.quality = std::format("ping {} ms, {} lost coming, {} going", stats->ping.count(),
                                      lost(stats->delivered_here), lost(stats->delivered_there));
        summary.traffic =
            std::format("{:.1f} kB/s in, {:.1f} out", stats->bytes_in_per_second / 1'000.0F,
                        stats->bytes_out_per_second / 1'000.0F);
    }
    if (const auto last = session.last_snapshot()) {
        summary.snapshots =
            std::format("{} snapshots a second, newest {} ms ago", session.snapshot_rate(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - *last).count());
    } else {
        summary.snapshots = "no snapshots yet";
    }
    for (const replication::EntityState& entity : session.replica().now()) {
        summary.nearby.push_back(
            {.name = entity.name, .distance = glm::length(protocol::to_world(entity.at) - eye)});
    }
    std::ranges::stable_sort(summary.nearby, {}, &SessionSummary::Seen::distance);
    return summary;
}

} // namespace hog::client
