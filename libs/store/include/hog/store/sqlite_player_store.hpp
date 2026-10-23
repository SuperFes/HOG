#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/store/database.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace hog::store {

// Where a player was when they last left, and how: on foot or flying, and looking which way.
struct PlayerPlace {
    I64Vec3 at{0}; // millimetres from the planet's centre
    bool flying = false;
    std::uint16_t heading = 0; // 65536ths of a turn from north towards east
    std::int16_t pitch = 0;    // 65536ths of a turn up from the horizon

    friend bool operator==(const PlayerPlace&, const PlayerPlace&) = default;
};

// Each account's player's place, kept in a world's SQLite file between visits.
class SqlitePlayerStore {
public:
    // Opens the database at `path`, creating it if missing. The path ":memory:" opens a private
    // database that lasts as long as the store.
    [[nodiscard]] static Result<SqlitePlayerStore> open(const std::filesystem::path& path);

    // Keeps `place` for `account`, in place of any kept before.
    Result<void> keep(std::uint64_t account, const PlayerPlace& place);
    // None for an account whose player has never left.
    [[nodiscard]] Result<std::optional<PlayerPlace>> find(std::uint64_t account) const;

private:
    explicit SqlitePlayerStore(Database database);

    Database database_;
};

} // namespace hog::store
