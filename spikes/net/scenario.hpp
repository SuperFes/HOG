#pragma once

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spike {

using Clock = std::chrono::steady_clock;

// The gate's traffic: movement up at 30 Hz, a snapshot down at 20 Hz, a chat line a second.
inline constexpr std::size_t movement_size = 64;
inline constexpr std::size_t snapshot_size = 400;
inline constexpr std::size_t chat_size = 32;
inline constexpr auto movement_every = std::chrono::microseconds{33'333};
inline constexpr auto snapshot_every = std::chrono::milliseconds{50};
inline constexpr auto chat_every = std::chrono::seconds{1};
inline constexpr auto poll_every = std::chrono::milliseconds{1};

enum class Channel : std::uint8_t { unreliable, reliable };
enum class Kind : std::uint8_t { movement, snapshot, chat };

struct Header {
    Kind kind;
    std::uint32_t sequence;
    std::int64_t sent_ns; // steady clock, shared by every process on the machine
};
inline constexpr std::size_t header_size = 1 + 4 + 8;

inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

inline std::vector<std::byte> encode(Kind kind, std::uint32_t sequence, std::size_t size) {
    std::vector<std::byte> bytes(size);
    const std::int64_t sent = now_ns();
    bytes[0] = static_cast<std::byte>(kind);
    std::memcpy(&bytes[1], &sequence, 4);
    std::memcpy(&bytes[5], &sent, 8);
    return bytes;
}

inline std::optional<Header> decode(std::span<const std::byte> bytes) {
    if (bytes.size() < header_size) {
        return std::nullopt;
    }
    Header header{};
    header.kind = static_cast<Kind>(bytes[0]);
    std::memcpy(&header.sequence, &bytes[1], 4);
    std::memcpy(&header.sent_ns, &bytes[5], 8);
    return header;
}

inline std::size_t size_of(Kind kind) {
    switch (kind) {
    case Kind::movement:
        return movement_size;
    case Kind::snapshot:
        return snapshot_size;
    case Kind::chat:
        return chat_size;
    }
    return 0;
}

class Samples {
public:
    void add(double value) { values_.push_back(value); }
    [[nodiscard]] std::size_t count() const { return values_.size(); }
    [[nodiscard]] double percentile(double fraction) {
        if (values_.empty()) {
            return 0.0;
        }
        std::ranges::sort(values_);
        const auto at =
            static_cast<std::size_t>(fraction * static_cast<double>(values_.size() - 1));
        return values_[at];
    }

private:
    std::vector<double> values_;
};

inline double milliseconds_since(std::int64_t sent_ns) {
    return static_cast<double>(now_ns() - sent_ns) / 1e6;
}

// Bytes through loopback, in the network namespace the scenario runs in, IP headers and all.
inline std::uint64_t loopback_bytes() {
    std::FILE* file = std::fopen("/proc/net/dev", "r");
    if (file == nullptr) {
        return 0;
    }
    std::array<char, 512> line{};
    std::uint64_t bytes = 0;
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        const std::string_view text{line.data()};
        const auto at = text.find("lo:");
        if (at != std::string_view::npos) {
            bytes = std::strtoull(line.data() + at + 3, nullptr, 10);
        }
    }
    std::fclose(file);
    return bytes;
}

inline double cpu_seconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](timeval time) {
        return static_cast<double>(time.tv_sec) + (static_cast<double>(time.tv_usec) / 1e6);
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

struct Arguments {
    bool server = false;
    int clients = 0;
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    double seconds = 0.0;
};

// server PORT SECONDS | clients N HOST PORT SECONDS
inline Arguments parse(int argc, char** argv) {
    const std::span arguments{argv, static_cast<std::size_t>(argc)};
    Arguments parsed;
    if (argc == 4 && std::string_view{arguments[1]} == "server") {
        parsed.server = true;
        parsed.port = static_cast<std::uint16_t>(std::atoi(arguments[2]));
        parsed.seconds = std::atof(arguments[3]);
    } else if (argc == 6 && std::string_view{arguments[1]} == "clients") {
        parsed.clients = std::atoi(arguments[2]);
        parsed.host = arguments[3];
        parsed.port = static_cast<std::uint16_t>(std::atoi(arguments[4]));
        parsed.seconds = std::atof(arguments[5]);
    } else {
        std::fputs("usage: server PORT SECONDS | clients N HOST PORT SECONDS\n", stderr);
        std::exit(2);
    }
    return parsed;
}

} // namespace spike
