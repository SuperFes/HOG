#include <hog/core/assert.hpp>
#include <hog/server/persistence.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <utility>

namespace hog::server {

namespace {

void write_double(ByteWriter& writer, double value) {
    writer.write(std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] Result<double> read_double(ByteReader& reader) {
    auto bits = reader.read<std::uint64_t>();
    if (!bits) {
        return std::unexpected(std::move(bits.error()));
    }
    return std::bit_cast<double>(*bits);
}

[[nodiscard]] std::unexpected<Error> bad_entities(const Error& error) {
    return make_error(ErrorCode::Parse, std::format("entities section: {}", error.message));
}

struct SavedIds {
    std::uint64_t next = 0;
    // Ascending, which also rules out duplicates.
    std::vector<std::uint64_t> ids;
};

[[nodiscard]] Result<SavedIds> read_ids(const save::Section& entities) {
    ByteReader reader{entities.bytes};
    const auto next = reader.read<std::uint64_t>();
    const auto count = reader.read<std::uint64_t>();
    if (!next || !count) {
        return make_error(ErrorCode::Parse, "entities section: truncated header");
    }
    if (*count > reader.remaining() / sizeof(std::uint64_t)) {
        return make_error(ErrorCode::Parse, std::format("entities section: {} ids in {} bytes",
                                                        *count, reader.remaining()));
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(static_cast<std::size_t>(*count));
    for (std::uint64_t i = 0; i < *count; ++i) {
        const auto id = reader.read<std::uint64_t>();
        if (!id) {
            return bad_entities(id.error());
        }
        if (*id == 0 || (!ids.empty() && *id <= ids.back())) {
            return make_error(ErrorCode::Parse,
                              std::format("entities section: id {} out of order", *id));
        }
        ids.push_back(*id);
    }
    if (const auto done = reader.finish(); !done) {
        return bad_entities(done.error());
    }
    return SavedIds{.next = *next, .ids = std::move(ids)};
}

} // namespace

void Persist<Position>::encode(const Position& position, ByteWriter& writer) {
    const DVec3 metres = position.at.metres();
    write_double(writer, metres.x);
    write_double(writer, metres.y);
    write_double(writer, metres.z);
}

Result<Position> Persist<Position>::decode(std::uint16_t /*version*/, ByteReader& reader) {
    const auto x = read_double(reader);
    const auto y = read_double(reader);
    const auto z = read_double(reader);
    if (!x || !y || !z) {
        return make_error(ErrorCode::Parse, "truncated position");
    }
    return Position{.at = WorldPosition{*x, *y, *z}};
}

Persistence::Persistence(flecs::world& world)
    : world_{&world},
      removed_{
          world.observer<const PersistentId>()
              .event(flecs::OnRemove)
              .each([this](flecs::entity, const PersistentId& id) { links_.erase(id.value); })} {}

Persistence::~Persistence() {
    removed_.destruct();
}

void Persistence::add_codec(Codec codec) {
    HOG_ASSERT(codec.section != entities_section &&
                   std::ranges::none_of(
                       codecs_, [&](const Codec& other) { return other.section == codec.section; }),
               "each persistent component needs its own section");
    codecs_.push_back(codec);
}

flecs::entity Persistence::create() {
    const std::uint64_t id = next_++;
    const flecs::entity entity = world_->entity().set<PersistentId>({id});
    link(id, entity);
    return entity;
}

void Persistence::link(std::uint64_t id, flecs::entity entity) {
    const bool inserted = links_.emplace(id, entity.id()).second;
    HOG_ASSERT(inserted, "persistent ids are unique");
}

std::optional<flecs::entity> Persistence::find(std::uint64_t id) const {
    const auto found = links_.find(id);
    if (found == links_.end()) {
        return std::nullopt;
    }
    return entity(id);
}

flecs::entity Persistence::entity(std::uint64_t id) const {
    return world_->entity(links_.at(id));
}

std::vector<std::uint64_t> Persistence::sorted_ids() const {
    std::vector<std::uint64_t> ids;
    ids.reserve(links_.size());
    for (const auto& [id, entity] : links_) {
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    return ids;
}

bool Persistence::staged(std::uint64_t id) const {
    return std::ranges::binary_search(staged_, id);
}

void Persistence::store(save::Snapshot& snapshot) const {
    const std::vector<std::uint64_t> ids = sorted_ids();
    ByteWriter writer;
    writer.write(next_);
    writer.write(static_cast<std::uint64_t>(ids.size()));
    for (const std::uint64_t id : ids) {
        writer.write(id);
    }
    snapshot.add(save::Section{
        .id = entities_section, .version = entities_version, .bytes = std::move(writer).take()});
    for (const Codec& codec : codecs_) {
        snapshot.add(codec.encode(*this));
    }
}

Result<std::size_t> Persistence::restore(const save::Snapshot& snapshot) {
    HOG_ASSERT(links_.empty(), "restoring into a world that already has persistent entities");
    const save::Section* entities = snapshot.find(entities_section);
    if (entities == nullptr) {
        for (const Codec& codec : codecs_) {
            if (snapshot.find(codec.section) != nullptr) {
                return make_error(
                    ErrorCode::Parse,
                    std::format("section {:#010x} without an entities section", codec.section));
            }
        }
        return 0;
    }
    if (entities->version > entities_version) {
        return newer_section(*entities);
    }

    auto ids = read_ids(*entities);
    if (!ids) {
        return std::unexpected(std::move(ids.error()));
    }
    const std::uint64_t next = ids->next;

    staged_ = std::move(ids->ids);
    for (const Codec& codec : codecs_) {
        if (const save::Section* section = snapshot.find(codec.section)) {
            if (auto checked = codec.decode(*this, *section, false); !checked) {
                staged_.clear();
                return std::unexpected(std::move(checked.error()));
            }
        }
    }

    for (const std::uint64_t id : staged_) {
        link(id, world_->entity().set<PersistentId>({id}));
    }
    for (const Codec& codec : codecs_) {
        if (const save::Section* section = snapshot.find(codec.section)) {
            const auto applied = codec.decode(*this, *section, true);
            HOG_ASSERT(applied.has_value(), "a checked section decodes the same way twice");
        }
    }
    next_ = staged_.empty() ? next : std::max(next, staged_.back() + 1);
    const std::size_t restored = staged_.size();
    staged_.clear();
    return restored;
}

std::unexpected<Error> Persistence::newer_section(const save::Section& section) {
    return make_error(ErrorCode::Unsupported,
                      std::format("section {:#010x} version {} is newer than this build reads",
                                  section.id, section.version));
}

std::unexpected<Error> Persistence::bad_section(const save::Section& section, const Error& error) {
    return make_error(ErrorCode::Parse,
                      std::format("section {:#010x}: {}", section.id, error.message));
}

std::unexpected<Error> Persistence::unknown_entity(const save::Section& section, std::uint64_t id) {
    return make_error(
        ErrorCode::Parse,
        std::format("section {:#010x} names entity {}, which isn't saved", section.id, id));
}

} // namespace hog::server
