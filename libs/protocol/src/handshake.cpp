#include <hog/core/assert.hpp>
#include <hog/protocol/handshake.hpp>

#include <format>
#include <utility>
#include <variant>

namespace hog::protocol {

namespace {

Answer refuse(Refusal reason, std::string detail) {
    std::string logged = detail;
    return {.reply = Refused{.reason = reason, .version = version, .detail = std::move(detail)},
            .close = true,
            .reason = std::move(logged)};
}

} // namespace

Answer Handshake::received(std::span<const std::byte> bytes) {
    HOG_ASSERT(!welcomed_, "shaking hands with a client already welcomed");
    const auto message = decode(bytes);
    if (!message) {
        return refuse(Refusal::Unexpected,
                      std::format("an unreadable first message: {}", message.error().message));
    }
    const auto* hello = std::get_if<Hello>(&*message);
    if (hello == nullptr) {
        return refuse(Refusal::Unexpected, "something other than hello first");
    }
    if (hello->version != version) {
        return refuse(Refusal::Version, std::format("the client speaks protocol {} and this "
                                                    "server {}",
                                                    hello->version, version));
    }
    welcomed_ = true;
    return {.reply = Welcome{}, .close = false, .reason = {}};
}

std::optional<Answer> Handshake::waited(MonotonicClock::time_point now) const {
    if (welcomed_ || now < deadline_) {
        return std::nullopt;
    }
    return refuse(Refusal::Silent, std::format("no hello within {}", hello_within));
}

} // namespace hog::protocol
