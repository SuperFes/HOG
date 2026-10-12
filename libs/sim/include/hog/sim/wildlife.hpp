#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/ecology.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace hog {
class ByteWriter;
}

namespace hog::jobs {
class JobSystem;
}

namespace hog::sim {

class Weather;

namespace detail {
struct LifeContext;
class Reader;
} // namespace detail

struct WildlifeConfig {
    int level = 5;
    // Steps fall on whole multiples of this since the epoch.
    GameDuration step = std::chrono::hours{24};
    double planet_radius = 115'000.0; // metres, for the cells' areas
    std::uint64_t seed = 0;           // where authored cycles peak first
    double most_eaten = 0.5;          // share of any food that can be eaten in a day
    double extinct_below = 1e-3;      // animals; a cell with fewer has none
    // Animals sense better ground this many cells away, valued less by `scent_fade` per cell.
    int scent_reach = 12;
    double scent_fade = 0.85;
    // Species empty out of and come back to regions the size of grid cells at this level.
    int region_level = 2;
    // Kills are remembered against their hunters, fading by half every `kill_half_life` years.
    // Events name up to `named` hunters, each with at least `named_share` of the kills remembered.
    double kill_half_life = 2.0;
    std::size_t named = 3;
    double named_share = 0.1;
    Ecology ecology{};
};

// What a cell offers wildlife.
struct Habitat {
    planet::Biome biome = planet::Biome::Grassland;
    bool ocean = false;
};

[[nodiscard]] std::vector<Habitat> habitat_for(const CellGrid& grid, const planet::World& world);

// The recent weather in each cell.
struct Conditions {
    std::span<const float> temperature;   // °C
    std::span<const float> precipitation; // mm per day
};

// Precondition: the weather's grid has the wildlife's level.
[[nodiscard]] Conditions conditions_of(const Weather& weather);

struct Population {
    std::vector<float> count; // animals in each cell
    std::vector<float> fed;   // share of their appetite met on the last step, 0 to 1

    friend bool operator==(const Population&, const Population&) = default;
};

struct WildlifeFields {
    std::vector<float> forage;           // kg per km²
    std::vector<float> carrion;          // kg per km²
    std::vector<Population> populations; // one per species, in the ecology's order

    friend bool operator==(const WildlifeFields&, const WildlifeFields&) = default;
};

// Where a species has gone from, and whether it has gone from everywhere.
struct Presence {
    // Per region, since when it has had none; empty while some live there, and for regions it
    // can't live in.
    std::vector<std::optional<GameTime>> emptied;
    std::optional<GameTime> extinct; // since when none are left anywhere, having once lived
    bool alive = false;              // some lived at the last step
    bool succeeded = false;          // its successor has come in its place

    friend bool operator==(const Presence&, const Presence&) = default;
};

// Whoever kills animals: a player, an NPC, or anything else gameplay tells apart.
using Hunter = std::uint64_t;

struct Contributor {
    Hunter hunter = 0;
    double kills = 0.0; // as remembered, faded with time

    friend bool operator==(const Contributor&, const Contributor&) = default;
};

// What happened to a species in a region, or everywhere, measured against its long-run baseline.
struct EcologyEvent {
    enum class Kind : std::uint8_t {
        Collapse,        // fell far below its baseline
        Overpopulation,  // rose far above it
        LocalExtinction, // none left in a region that held a fair number
        Return,          // back where it had died out, or anywhere after extinction
        Extinction,      // none left anywhere
    };
    static constexpr std::uint32_t everywhere = std::numeric_limits<std::uint32_t>::max();

    Kind kind = Kind::Collapse;
    std::uint32_t species = 0;
    std::uint32_t region = everywhere;
    GameTime at;
    double count = 0.0; // animals there now
    double baseline = 0.0;
    std::vector<Contributor> hunters; // most kills first

    friend bool operator==(const EcologyEvent&, const EcologyEvent&) = default;
};

[[nodiscard]] std::string_view to_string(EcologyEvent::Kind kind) noexcept;

// Populations of each species per cell, stepped together: plants grow, animals eat, are born and
// die, and wander or head for better ground. Long cycles come from predators and prey chasing
// each other and from good and bad years of weather, plus any a species is authored with.
// A region a species empties out of fills back from its neighbours, or failing that, from a
// founding group once a while has passed and no rival has taken its place. Once a species is gone
// from everywhere it's extinct: it waits to be reintroduced, or as its ecology says, a few relicts
// turn up or a successor takes its place.
// Each species keeps a slow baseline of its numbers per region, and what strays far from it, or
// dies out or comes back, is reported as an event naming the hunters with the most kills there.
class Wildlife {
public:
    // Starts at the ecology's densities with forage grown in, as of the last step at or before
    // `now`. Precondition: one habitat per cell at the configured level, a positive step.
    Wildlife(WildlifeConfig config, std::vector<Habitat> habitat, GameTime now);

    [[nodiscard]] const WildlifeConfig& config() const noexcept { return config_; }
    [[nodiscard]] const CellGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] const std::vector<Habitat>& habitat() const noexcept { return habitat_; }
    [[nodiscard]] const WildlifeFields& fields() const noexcept { return fields_; }
    // km².
    [[nodiscard]] double area(std::uint32_t cell) const { return areas_[cell]; }
    [[nodiscard]] std::uint32_t region(std::uint32_t cell) const { return regions_[cell]; }
    [[nodiscard]] std::uint32_t region_count() const noexcept { return region_count_; }
    [[nodiscard]] const Presence& presence(std::uint32_t species) const {
        return presence_[species];
    }
    // The species' long-run number of animals in the region.
    [[nodiscard]] double baseline(std::uint32_t species, std::uint32_t region) const {
        return watched_[species].regions[region].baseline;
    }
    // The kills remembered against each hunter of the species in the region, in no order.
    [[nodiscard]] std::span<const Contributor> kills(std::uint32_t species,
                                                     std::uint32_t region) const {
        return watched_[species].kills[region];
    }
    // When the last step was taken, and when the next is due.
    [[nodiscard]] GameTime time() const noexcept { return time_; }
    [[nodiscard]] GameTime next_step() const noexcept { return time_ + config_.step; }

    // Takes every step due by `now`, all under the same conditions; returns how many. Call from
    // the jobs' owning thread. Precondition: conditions for every cell.
    std::uint64_t advance_to(GameTime now, const Conditions& conditions, jobs::JobSystem& jobs);

    // Animals of a species across the planet.
    [[nodiscard]] double total(std::uint32_t species) const;
    // Removes up to `count` animals from a cell, as when they're killed or brought to life in
    // detail; returns how many there were to take.
    float take(std::uint32_t species, std::uint32_t cell, float count);
    // Puts animals back, as when they're folded back into the population or reintroduced; a
    // species brought back from extinction is alive again from the next step.
    void add(std::uint32_t species, std::uint32_t cell, float count);
    // Takes animals as `take` does, remembering them against the hunter; returns how many died.
    float kill(std::uint32_t species, std::uint32_t cell, float count, Hunter hunter);
    // Events since the last call, in the order they happened. They aren't saved.
    [[nodiscard]] std::vector<EcologyEvent> drain_events();

    // Versioned bytes for a snapshot section.
    [[nodiscard]] std::vector<std::byte> save() const;
    // Replaces the state with saved bytes, or fails and changes nothing: with Unsupported for a
    // newer version, and InvalidArgument for wildlife saved at another grid level. Species are
    // matched by name; one the save lacks keeps its state. Presence and baselines missing from an
    // older save are taken from the populations, as of the save, and no kills are remembered.
    [[nodiscard]] Result<void> load(std::uint16_t version, std::span<const std::byte> bytes);

    static constexpr std::uint16_t save_version = 3;

private:
    // A species' baseline in a region, and what has last been reported of it there.
    struct Watch {
        enum class State : std::uint8_t { Usual, Collapsed, Overrun, Gone };
        double baseline = 0.0;
        State state = State::Usual;

        friend bool operator==(const Watch&, const Watch&) = default;
    };
    // Whether a species has lived at all, lives, or has died out everywhere, as last reported.
    enum class Standing : std::uint8_t { Absent, Alive, Extinct };
    [[nodiscard]] static Standing standing(const Presence& presence);
    // A species' baselines and the kills remembered, per region. Strays from the baseline go
    // unreported until it has had a while to settle after `since`.
    struct Watched {
        GameTime since;
        std::vector<Watch> regions;
        std::vector<std::vector<Contributor>> kills;
    };
    void write_watch(ByteWriter& out, std::uint32_t species) const;
    // Whether what was read can be used: watching saved over another number of regions is read
    // past.
    [[nodiscard]] bool read_watch(detail::Reader& in, Watched& watched) const;

    void step(GameTime at, const Conditions& conditions, jobs::JobSystem& jobs);
    // Animals wander, and head for better ground they can sense.
    void move(const detail::LifeContext& context, std::uint32_t species, jobs::JobSystem& jobs);
    // Animals of each species in each region.
    [[nodiscard]] std::vector<std::vector<double>> held(const WildlifeFields& fields) const;
    // Presence as it stands, with every emptied region's wait starting at `at`.
    [[nodiscard]] Presence present(std::uint32_t species, std::span<const double> held,
                                   GameTime at) const;
    // Notes where each species has gone from and come back to, and brings founders, relicts and
    // successors where they're due.
    void recover(const detail::LifeContext& context, GameTime at);
    void track(std::uint32_t species, std::span<const double> held, GameTime at);
    void found(const detail::LifeContext& context, std::uint32_t species,
               std::vector<std::vector<double>>& held, GameTime at);
    void outlive(const detail::LifeContext& context, std::uint32_t species, GameTime at);
    // Reports what strayed from its baseline, then moves the baselines on and fades the kills.
    void report(GameTime at);
    void watch(std::uint32_t species, std::uint32_t region, double held, GameTime at);
    [[nodiscard]] Watched watching(std::span<const double> held, GameTime since) const;
    void announce(EcologyEvent::Kind kind, std::uint32_t species, std::uint32_t region, GameTime at,
                  double count, double baseline);
    // The hunters to name, of those remembered.
    [[nodiscard]] std::vector<Contributor> named(std::vector<Contributor> kills) const;
    // Whether founders could live in the region: no rival with its main food is settled there.
    [[nodiscard]] bool open_niche(std::uint32_t species, std::uint32_t region,
                                  const std::vector<std::vector<double>>& held) const;
    // The cell the species likes best, of those `where` allows; none if it likes none.
    template <typename Where>
    [[nodiscard]] std::optional<std::uint32_t> best_cell(const detail::LifeContext& context,
                                                         std::uint32_t species, Where where) const;
    // A wait between `earliest` and `latest` years, the same each time for the same reasons.
    [[nodiscard]] GameDuration wait(std::uint32_t species, std::uint64_t place, GameTime since,
                                    double earliest, double latest) const;

    WildlifeConfig config_;
    CellGrid grid_;
    std::vector<Habitat> habitat_;
    std::vector<double> areas_;
    // For each neighbour of a cell, which of its neighbours is the cell.
    std::vector<std::array<std::uint8_t, 4>> back_;
    // Per species, the year (from 0 to its cycle's length) each cell's authored boom peaks.
    std::vector<std::vector<float>> peaks_;
    std::vector<std::uint32_t> regions_; // each cell's
    std::uint32_t region_count_ = 0;
    // Per species, the regions with ground it can live on.
    std::vector<std::vector<bool>> livable_;
    WildlifeFields fields_;
    std::vector<Presence> presence_;
    std::vector<Watched> watched_;
    std::vector<Standing> standing_;
    std::vector<EcologyEvent> events_;
    GameTime time_;
    // Scratch for moving.
    std::vector<float> appeal_;
    std::vector<float> scent_;
    std::vector<float> scent_next_;
    std::vector<std::array<float, 4>> leaving_;
    std::vector<float> arriving_;
};

} // namespace hog::sim
