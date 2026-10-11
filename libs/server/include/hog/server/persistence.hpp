#pragma once

#include <hog/core/bytes.hpp>
#include <hog/core/result.hpp>
#include <hog/save/snapshot.hpp>
#include <hog/server/spatial_tracking.hpp>

#include <flecs.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::server {

// An entity's id across saves. flecs ids are reused and differ between runs, so saved entities
// carry their own; Persistence assigns them.
struct PersistentId {
    std::uint64_t value = 0;
};

// How a component is saved. Specialize with:
//   static constexpr save::SectionId section;  // never reused once released
//   static constexpr std::uint16_t version;    // raised whenever the bytes change
//   static void encode(const T&, ByteWriter&);
//   static Result<T> decode(std::uint16_t version, ByteReader&);  // every version ever written
template <typename T>
struct Persist;

template <typename T>
concept Persistable = requires(const T& value, ByteWriter& writer, ByteReader& reader) {
    { Persist<T>::section } -> std::convertible_to<save::SectionId>;
    { Persist<T>::version } -> std::convertible_to<std::uint16_t>;
    Persist<T>::encode(value, writer);
    { Persist<T>::decode(std::uint16_t{}, reader) } -> std::same_as<Result<T>>;
};

template <>
struct Persist<Position> {
    static constexpr save::SectionId section = save::section_id("POSN");
    static constexpr std::uint16_t version = 1;
    static void encode(const Position& position, ByteWriter& writer);
    static Result<Position> decode(std::uint16_t version, ByteReader& reader);
};

// Saves and restores a world's persistent entities: an "ENTS" section listing their ids, and a
// section per registered component. Components added later are simply absent from older
// snapshots, and sections of components no longer registered are ignored.
class Persistence {
public:
    static constexpr save::SectionId entities_section = save::section_id("ENTS");
    static constexpr std::uint16_t entities_version = 1;

    // The world must outlive this.
    explicit Persistence(flecs::world& world);
    ~Persistence();
    Persistence(const Persistence&) = delete;
    Persistence(Persistence&&) = delete;
    Persistence& operator=(const Persistence&) = delete;
    Persistence& operator=(Persistence&&) = delete;

    // Saves T with every persistent entity that has one. Precondition: no registered component
    // shares its section.
    template <Persistable T>
    void persist() {
        add_codec(Codec{
            .section = Persist<T>::section,
            .encode = &encode_component<T>,
            .decode = &decode_component<T>,
        });
    }

    // A new entity that will be saved.
    flecs::entity create();

    // The live entity with this id.
    [[nodiscard]] std::optional<flecs::entity> find(std::uint64_t id) const;
    [[nodiscard]] std::size_t size() const noexcept { return links_.size(); }

    // Adds the entities section and one section per registered component.
    void store(save::Snapshot& snapshot) const;

    // Recreates the snapshot's entities with their components and returns how many there were.
    // Changes nothing on failure. Precondition: no persistent entities exist yet.
    Result<std::size_t> restore(const save::Snapshot& snapshot);

private:
    // Decoding applies components only when `apply` is set, so a restore can check everything
    // first.
    struct Codec {
        save::SectionId section = 0;
        save::Section (*encode)(const Persistence&) = nullptr;
        Result<void> (*decode)(Persistence&, const save::Section&, bool apply) = nullptr;
    };

    void add_codec(Codec codec);
    void link(std::uint64_t id, flecs::entity entity);

    // Ids in ascending order, so snapshots of the same state are byte-for-byte equal.
    [[nodiscard]] std::vector<std::uint64_t> sorted_ids() const;
    [[nodiscard]] bool staged(std::uint64_t id) const;

    template <Persistable T>
    static save::Section encode_component(const Persistence& self) {
        ByteWriter writer;
        std::size_t count = 0;
        for (const std::uint64_t id : self.sorted_ids()) {
            if (const T* component = self.entity(id).template try_get<T>()) {
                writer.write(id);
                Persist<T>::encode(*component, writer);
                ++count;
            }
        }
        ByteWriter section;
        section.write(static_cast<std::uint64_t>(count));
        section.write_bytes(writer.bytes());
        return save::Section{.id = Persist<T>::section,
                             .version = Persist<T>::version,
                             .bytes = std::move(section).take()};
    }

    template <Persistable T>
    static Result<void> decode_component(Persistence& self, const save::Section& section,
                                         bool apply) {
        if (section.version > Persist<T>::version) {
            return newer_section(section);
        }
        ByteReader reader{section.bytes};
        auto count = reader.read<std::uint64_t>();
        if (!count) {
            return bad_section(section, count.error());
        }
        for (std::uint64_t i = 0; i < *count; ++i) {
            auto id = reader.read<std::uint64_t>();
            if (!id) {
                return bad_section(section, id.error());
            }
            if (!self.staged(*id)) {
                return unknown_entity(section, *id);
            }
            auto component = Persist<T>::decode(section.version, reader);
            if (!component) {
                return bad_section(section, component.error());
            }
            if (apply) {
                self.entity(*id).template set<T>(*std::move(component));
            }
        }
        if (auto done = reader.finish(); !done) {
            return bad_section(section, done.error());
        }
        return {};
    }

    [[nodiscard]] flecs::entity entity(std::uint64_t id) const;
    [[nodiscard]] static std::unexpected<Error> newer_section(const save::Section& section);
    [[nodiscard]] static std::unexpected<Error> bad_section(const save::Section& section,
                                                            const Error& error);
    [[nodiscard]] static std::unexpected<Error> unknown_entity(const save::Section& section,
                                                               std::uint64_t id);

    flecs::world* world_;
    flecs::entity removed_;
    std::vector<Codec> codecs_;
    std::unordered_map<std::uint64_t, flecs::entity_t> links_;
    // Ids a restore has checked, in ascending order.
    std::vector<std::uint64_t> staged_;
    std::uint64_t next_ = 1;
};

} // namespace hog::server
