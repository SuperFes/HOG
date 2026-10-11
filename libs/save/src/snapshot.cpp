#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/save/snapshot.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace hog::save {

namespace {

constexpr std::array<std::byte, 8> magic{std::byte{'H'}, std::byte{'O'}, std::byte{'G'},
                                         std::byte{'S'}, std::byte{'N'}, std::byte{'A'},
                                         std::byte{'P'}, std::byte{0}};

constexpr std::size_t checksum_size = sizeof(std::uint64_t);

template <typename T>
[[nodiscard]] std::unexpected<Error> fail(const Result<T>& failed) {
    return make_error(ErrorCode::Parse, std::format("snapshot: {}", failed.error().message));
}

} // namespace

const Section* Snapshot::find(SectionId id) const noexcept {
    const auto found = std::ranges::find(sections, id, &Section::id);
    return found == sections.end() ? nullptr : &*found;
}

void Snapshot::add(Section section) {
    HOG_ASSERT(find(section.id) == nullptr, "a snapshot holds one section per id");
    sections.push_back(std::move(section));
}

std::vector<std::byte> encode(const Snapshot& snapshot) {
    ByteWriter writer;
    writer.write_bytes(magic);
    writer.write(format_version);
    writer.write(snapshot.world);
    writer.write(static_cast<std::uint64_t>(snapshot.time.time_since_epoch().count()));
    writer.write(snapshot.tick.value());
    writer.write(static_cast<std::uint32_t>(snapshot.sections.size()));
    for (const Section& section : snapshot.sections) {
        writer.write(section.id);
        writer.write(section.version);
        writer.write(static_cast<std::uint64_t>(section.bytes.size()));
        writer.write_bytes(section.bytes);
    }
    writer.write(hash_bytes(writer.bytes()));
    return std::move(writer).take();
}

Result<Snapshot> decode(std::span<const std::byte> bytes) {
    if (bytes.size() < magic.size() || !std::ranges::equal(bytes.first(magic.size()), magic)) {
        return make_error(ErrorCode::Parse, "not a snapshot");
    }
    ByteReader header{bytes.subspan(magic.size())};
    const auto version = header.read<std::uint16_t>();
    if (!version) {
        return fail(version);
    }
    if (*version > format_version) {
        return make_error(ErrorCode::Unsupported, std::format("snapshot format {} is newer than {}",
                                                              *version, format_version));
    }

    if (bytes.size() < checksum_size) {
        return make_error(ErrorCode::Parse, "snapshot: missing checksum");
    }
    const auto body = bytes.first(bytes.size() - checksum_size);
    ByteReader trailer{bytes.last(checksum_size)};
    if (trailer.read<std::uint64_t>() != hash_bytes(body)) {
        return make_error(ErrorCode::Parse, "snapshot: checksum mismatch");
    }

    ByteReader reader{body.subspan(magic.size() + sizeof(std::uint16_t))};
    const auto world = reader.read<std::uint64_t>();
    const auto time = reader.read<std::uint64_t>();
    const auto tick = reader.read<std::uint64_t>();
    const auto count = reader.read<std::uint32_t>();
    if (!world || !time || !tick || !count) {
        return make_error(ErrorCode::Parse, "snapshot: truncated header");
    }

    Snapshot snapshot{
        .world = *world,
        .time = GameTime{GameDuration{static_cast<GameDuration::rep>(*time)}},
        .tick = Tick{*tick},
        .sections = {},
    };
    for (std::uint32_t i = 0; i < *count; ++i) {
        const auto id = reader.read<std::uint32_t>();
        const auto section_version = reader.read<std::uint16_t>();
        const auto size = reader.read<std::uint64_t>();
        if (!id || !section_version || !size) {
            return make_error(ErrorCode::Parse, "snapshot: truncated section header");
        }
        if (*size > reader.remaining()) {
            return make_error(
                ErrorCode::Parse,
                std::format("snapshot: section of {} bytes, {} left", *size, reader.remaining()));
        }
        if (snapshot.find(*id) != nullptr) {
            return make_error(ErrorCode::Parse,
                              std::format("snapshot: section {:#010x} appears twice", *id));
        }
        const auto section_bytes = reader.read_bytes(static_cast<std::size_t>(*size));
        if (!section_bytes) {
            return fail(section_bytes);
        }
        snapshot.sections.push_back(Section{
            .id = *id,
            .version = *section_version,
            .bytes = {section_bytes->begin(), section_bytes->end()},
        });
    }
    if (const auto done = reader.finish(); !done) {
        return fail(done);
    }
    return snapshot;
}

} // namespace hog::save
