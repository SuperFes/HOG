#include <hog/core/time.hpp>
#include <hog/probe/channel.hpp>

#include <span>
#include <utility>

namespace hog::probe {

namespace {

constexpr std::size_t read_size = std::size_t{64} << 10U;

} // namespace

Channel::Channel(Socket socket) : socket_(std::move(socket)), buffer_(read_size) {}

Result<void> Channel::send(const Message& message) {
    return socket_.send(encode(message));
}

Result<std::optional<Message>> Channel::receive(std::chrono::milliseconds timeout) {
    const MonotonicClock::time_point deadline = MonotonicClock::now() + timeout;
    for (;;) {
        auto next = frames_.next();
        if (!next || next->has_value()) {
            return next;
        }
        const auto left =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - MonotonicClock::now());
        auto ready = socket_.readable(std::max(left, std::chrono::milliseconds::zero()));
        if (!ready) {
            return std::unexpected(std::move(ready.error()));
        }
        if (!*ready) {
            return std::nullopt;
        }
        auto got = socket_.receive(buffer_);
        if (!got) {
            return std::unexpected(std::move(got.error()));
        }
        if (*got == 0) {
            return make_error(ErrorCode::Io, "the peer closed the connection");
        }
        frames_.feed(std::span{buffer_}.first(*got));
    }
}

} // namespace hog::probe
