#include <hog/client/session.hpp>
#include <hog/core/log.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
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

Result<Session> Session::connect(std::string address, std::vector<std::byte> token,
                                 std::optional<std::uint64_t> world) {
    auto client = net::Client::connect(address);
    if (!client) {
        return std::unexpected(client.error());
    }
    log::info(session_log, "connecting to {}", address);
    Session session{std::move(*client), std::move(address), std::move(token)};
    session.world_ = world;
    return session;
}

Session::Session(net::Client client, std::string address, std::vector<std::byte> token) noexcept
    : client_(std::move(client)), address_(std::move(address)), token_(std::move(token)),
      counting_since_(MonotonicClock::now()) {}

void Session::look(std::optional<Whereabouts> player, MonotonicClock::time_point now) {
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
    if (state_ == State::Welcomed && player && (!next_move_ || now >= *next_move_)) {
        send(protocol::Moved{.sequence = ++moves_,
                             .corrections = corrections_,
                             .at = protocol::to_millimetres(player->at),
                             .stance = player->stance,
                             .facing = player->facing},
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
    auto message = protocol::decode(bytes);
    if (!message && message.error().code == ErrorCode::Unsupported) {
        // A kind from a newer server: skipped, as the rest still make sense.
        log::debug(session_log, "{} sent something this client doesn't know: {}", address_,
                   message.error());
    } else if (!message) {
        log::warn(session_log, "{} sent something unreadable: {}", address_, message.error());
    } else if (const auto* welcome = std::get_if<protocol::Welcome>(&*message)) {
        if (world_ && welcome->world != 0 && welcome->world != *world_) {
            reason_ =
                std::format("the server runs planet {:016x}, not the {:016x} this client makes",
                            welcome->world, *world_);
            log::warn(session_log, "{}", reason_);
            state_ = State::Refused;
            return;
        }
        log::info(session_log, "welcomed by {}", address_);
        planet_ = welcome->planet;
        ground_origin_ = welcome->ground;
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
            timeline_.add(snapshot->tick, now, replica_.now());
            last_snapshot_ = now;
            ++counted_;
        }
    } else if (const auto* corrected = std::get_if<protocol::Corrected>(&*message)) {
        if (corrected->count > corrections_) {
            corrections_ = corrected->count;
            correction_ = protocol::to_world(corrected->at);
            log::debug(session_log, "{} put the player back", address_);
        }
    } else if (const auto* placed = std::get_if<protocol::Placed>(&*message)) {
        placed_ = *placed;
    } else if (auto* weather = std::get_if<protocol::Weather>(&*message)) {
        weather_ = std::move(*weather);
    } else if (auto* said = std::get_if<protocol::Said>(&*message)) {
        chat_.push_back({.from = std::move(said->from), .text = std::move(said->text)});
    } else if (auto* tiles = std::get_if<protocol::Tiles>(&*message)) {
        tiles_.insert(tiles_.end(), std::make_move_iterator(tiles->tiles.begin()),
                      std::make_move_iterator(tiles->tiles.end()));
    } else if (const auto* grains = std::get_if<protocol::Grains>(&*message)) {
        grains_.receive(*grains, now);
    } else if (const auto* clock = std::get_if<protocol::Clock>(&*message)) {
        if (clock->scale != world_rate_) {
            log::info(session_log, "{} runs the world at {}x", address_, clock->scale);
            world_rate_ = clock->scale;
        }
    } else {
        log::debug(session_log, "{} sent a message out of place", address_);
    }
}

void Session::say(std::string_view text) {
    if (state_ != State::Welcomed) {
        return;
    }
    std::string tidied;
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (value >= 0x20U && value != 0x7FU) {
            tidied.push_back(character);
        }
    }
    while (!tidied.empty() && tidied.back() == ' ') {
        tidied.pop_back();
    }
    tidied.erase(0, tidied.find_first_not_of(' '));
    if (tidied.size() > protocol::most_said) {
        tidied.resize(protocol::most_said);
        // A line cut short mustn't end mid-way through a UTF-8 character.
        while (!tidied.empty() && (static_cast<unsigned char>(tidied.back()) & 0xC0U) == 0x80U) {
            tidied.pop_back();
        }
    }
    if (tidied.empty()) {
        return;
    }
    send(protocol::Said{.from = {}, .text = std::move(tidied)}, net::Delivery::Reliable);
}

void Session::hurry(std::uint32_t scale) {
    if (state_ != State::Welcomed || scale == 0) {
        return;
    }
    send(protocol::Hurry{.scale = scale}, net::Delivery::Reliable);
}

namespace {

// A ball's radius as sent, in whole millimetres; none if it couldn't be.
std::optional<std::uint32_t> sendable(double radius) {
    const double millimetres = std::round(radius * 1'000.0);
    if (!(millimetres >= 1.0) || millimetres > protocol::most_edit) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(millimetres);
}

} // namespace

void Session::dig(WorldPosition centre, double radius) {
    const std::optional<std::uint32_t> width = sendable(radius);
    if (state_ != State::Welcomed || !width) {
        return;
    }
    send(protocol::Dug{.centre = protocol::to_millimetres(centre), .radius = *width},
         net::Delivery::Reliable);
}

void Session::build(WorldPosition centre, double radius, std::uint8_t material) {
    const std::optional<std::uint32_t> width = sendable(radius);
    if (state_ != State::Welcomed || !width) {
        return;
    }
    send(protocol::Built{.centre = protocol::to_millimetres(centre),
                         .radius = *width,
                         .material = material},
         net::Delivery::Reliable);
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
    if (session.world_rate() != 1) {
        summary.clock = std::format("world time {}x", session.world_rate());
    }
    if (const auto last = session.last_snapshot()) {
        summary.snapshots =
            std::format("{} snapshots a second, newest {} ms ago", session.snapshot_rate(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - *last).count());
    } else {
        summary.snapshots = "no snapshots yet";
    }
    for (const replication::Shown& other : session.others(now)) {
        summary.nearby.push_back(
            {.name = std::string{other.name}, .distance = glm::length(other.at - eye)});
    }
    std::ranges::stable_sort(summary.nearby, {}, &SessionSummary::Seen::distance);
    return summary;
}

} // namespace hog::client
