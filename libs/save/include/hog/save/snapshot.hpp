#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace hog::save {

// Names a section: four ASCII characters, such as "ENTS", read as a little-endian integer.
using SectionId = std::uint32_t;

[[nodiscard]] consteval SectionId section_id(std::string_view name) {
    if (name.size() != sizeof(SectionId)) {
        throw std::invalid_argument("a section id is four characters");
    }
    SectionId id = 0;
    for (const char c : name | std::views::reverse) {
        id = (id << 8U) | static_cast<unsigned char>(c);
    }
    return id;
}

// One system's state. Each system versions its own bytes and keeps decoding every version it has
// ever written, so sections change independently of each other and of the container.
struct Section {
    SectionId id = 0;
    std::uint16_t version = 0;
    std::vector<std::byte> bytes;

    friend bool operator==(const Section&, const Section&) = default;
};

// A running world's state at one tick. Terrain isn't here: it is the seed plus the change log.
struct Snapshot {
    // Identifies the world, so a snapshot is never loaded into a different one.
    std::uint64_t world = 0;
    GameTime time;
    Tick tick;
    std::vector<Section> sections;

    // The section with this id, or nullptr. Readers ignore sections they don't know, so a system
    // can be added or retired without touching the others.
    [[nodiscard]] const Section* find(SectionId id) const noexcept;

    // Precondition: no section with this id yet.
    void add(Section section);

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

// The container layout's version, separate from every section's version.
inline constexpr std::uint16_t format_version = 1;

// Fixed-width little-endian bytes, the same on every platform, ending in a checksum.
[[nodiscard]] std::vector<std::byte> encode(const Snapshot& snapshot);

// Fails on damaged bytes, duplicate sections or a newer container version than this build reads.
[[nodiscard]] Result<Snapshot> decode(std::span<const std::byte> bytes);

} // namespace hog::save
