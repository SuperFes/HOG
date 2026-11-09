#include <hog/core/hash.hpp>
#include <hog/planet/system.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace hog::planet {

namespace {

using namespace std::string_view_literals;

constexpr std::array system_keys{"air"sv,   "orbit"sv, "cycles"sv, "stars"sv,
                                 "moons"sv, "rings"sv, "planets"sv};

constexpr double above_zero = std::numeric_limits<double>::min();
constexpr double most_degrees = 360.0;
constexpr double most_au = 1.0e5;
constexpr double most_km = 1.0e9;
constexpr double most_period = 1.0e6;
constexpr double most_cycle = 1.0e9;

// Stars and planets are placed in astronomical units and Earth's years, moons in kilometres and
// the planet's days.
struct Clock {
    std::string_view distance;
    double metres;
    double most;
    std::string_view period;
};

constexpr Clock years{.distance = "distance_au",
                      .metres = astronomical_unit,
                      .most = most_au,
                      .period = "period_years"};
constexpr Clock days{
    .distance = "distance_km", .metres = 1'000.0, .most = most_km, .period = "period_days"};

// Reads one table, keeping the first failure for every table read alongside it.
class Reader {
public:
    Reader(const toml::table* table, std::string name, std::optional<Error>* error)
        : table_(table), name_(std::move(name)), error_(error) {}

    void only(std::initializer_list<std::string_view> known) {
        if (table_ == nullptr) {
            return;
        }
        for (const auto& [key, value] : *table_) {
            if (std::ranges::find(known, key.str()) == known.end()) {
                fail(std::format("unknown key {}", where(key.str())));
            }
        }
    }

    // Leaves `value` alone when the key is missing; stores the number read times `scale`.
    void number(std::string_view key, double low, double high, double& value, double scale = 1.0) {
        const toml::node* node = find(key);
        if (node == nullptr) {
            return;
        }
        std::optional<double> read;
        if (const auto* integer = node->as_integer()) {
            read = static_cast<double>(integer->get());
        } else if (const auto* floating = node->as_floating_point()) {
            read = floating->get();
        }
        // Written so NaN fails.
        const bool in_range = read && *read >= low && *read <= high;
        if (!in_range) {
            fail(std::format("{} must be a number from {} to {}", where(key), low, high));
            return;
        }
        value = *read * scale;
    }

    // Null when missing, or when it isn't a table, which fails.
    [[nodiscard]] Reader child(std::string_view key) {
        const toml::node* node = find(key);
        if (node != nullptr && !node->is_table()) {
            fail(std::format("{} must be a table", where(key)));
        }
        return Reader{node == nullptr ? nullptr : node->as_table(), where(key), error_};
    }

    // Reads each table of a list over a fresh `Body`, replacing `bodies` when the key is there.
    template <typename Body, typename Read>
    void list(std::string_view key, std::vector<Body>& bodies, Read read) {
        const toml::node* node = find(key);
        if (node == nullptr) {
            return;
        }
        const auto* array = node->as_array();
        if (array == nullptr) {
            fail(std::format("{} must be a list of tables", where(key)));
            return;
        }
        bodies.clear();
        for (std::size_t index = 0; index < array->size(); ++index) {
            const toml::node& element = *array->get(index);
            Reader entry{element.as_table(), std::format("{}[{}]", where(key), index), error_};
            if (entry.table_ == nullptr) {
                fail(std::format("{} must be a table", entry.name_));
                continue;
            }
            Body body;
            read(entry, body);
            bodies.push_back(body);
        }
    }

    [[nodiscard]] bool given() const noexcept { return table_ != nullptr; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    [[nodiscard]] std::string where(std::string_view key) const {
        return std::format("{}.{}", name_, key);
    }

    void fail(std::string message) {
        if (!*error_) {
            *error_ = Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)};
        }
    }

private:
    [[nodiscard]] const toml::node* find(std::string_view key) const {
        return table_ == nullptr ? nullptr : table_->get(key);
    }

    const toml::table* table_;
    std::string name_;
    std::optional<Error>* error_;
};

void read_orbit(Reader& orbit, Orbit& into, const Clock& clock) {
    orbit.only({clock.distance, "eccentricity", "inclination", "node", "periapsis", "longitude",
                clock.period});
    orbit.number(clock.distance, 0.0, clock.most, into.distance, clock.metres);
    orbit.number("eccentricity", 0.0, 0.99, into.eccentricity);
    orbit.number("inclination", 0.0, 180.0, into.inclination);
    orbit.number("node", -most_degrees, most_degrees, into.node);
    orbit.number("periapsis", -most_degrees, most_degrees, into.periapsis);
    orbit.number("longitude", -most_degrees, most_degrees, into.longitude);
    orbit.number(clock.period, above_zero, most_period, into.period);
}

void read_star(Reader& star, Star& into) {
    star.only({"luminosity", "temperature", "radius_km", "tide", "orbit"});
    star.number("luminosity", above_zero, 1.0e7, into.luminosity);
    star.number("temperature", 500.0, 100'000.0, into.temperature);
    star.number("radius_km", above_zero, most_km, into.radius, 1'000.0);
    star.number("tide", 0.0, 100.0, into.tide);
    Reader orbit = star.child("orbit");
    read_orbit(orbit, into.orbit, years);
}

void read_moon(Reader& moon, Moon& into) {
    moon.only({"radius_km", "albedo", "tide", "orbit"});
    moon.number("radius_km", above_zero, most_km, into.radius, 1'000.0);
    moon.number("albedo", 0.0, 1.0, into.albedo);
    moon.number("tide", 0.0, 100.0, into.tide);
    Reader orbit = moon.child("orbit");
    read_orbit(orbit, into.orbit, days);
    if (into.orbit.distance <= 0.0) {
        moon.fail(std::format("{}.orbit.distance_km must be above 0", moon.name()));
    }
}

void read_wanderer(Reader& planet, Wanderer& into) {
    planet.only({"radius_km", "albedo", "orbit"});
    planet.number("radius_km", above_zero, most_km, into.radius, 1'000.0);
    planet.number("albedo", 0.0, 1.0, into.albedo);
    Reader orbit = planet.child("orbit");
    read_orbit(orbit, into.orbit, years);
    if (into.radius <= 0.0 || into.orbit.distance <= 0.0) {
        planet.fail(std::format("{} needs radius_km and orbit.distance_au", planet.name()));
    }
}

void read_air(Reader& air, Air& into) {
    air.only({"pressure_kpa", "oxygen", "carbon_dioxide_ppm"});
    air.number("pressure_kpa", above_zero, 1.0e5, into.pressure, 1'000.0);
    air.number("oxygen", 0.0, 1.0, into.oxygen);
    air.number("carbon_dioxide_ppm", 0.0, 1.0e6, into.carbon_dioxide, 1.0e-6);
    if (into.oxygen + into.carbon_dioxide > 1.0) {
        air.fail(
            std::format("{}: oxygen and carbon dioxide come to more than the air", air.name()));
    }
}

void read_rings(Reader& rings, std::optional<Rings>& into) {
    if (!rings.given()) {
        return;
    }
    rings.only({"inner", "outer", "opacity", "albedo"});
    Rings read;
    rings.number("inner", 1.0, 1'000.0, read.inner);
    rings.number("outer", 1.0, 1'000.0, read.outer);
    rings.number("opacity", 0.0, 1.0, read.opacity);
    rings.number("albedo", 0.0, 1.0, read.albedo);
    if (read.outer <= read.inner) {
        rings.fail(std::format("{}.outer must be beyond inner", rings.name()));
    }
    into = read;
}

void read_cycle(Reader& swing, Cycle& into, double most) {
    swing.only({"size", "period_years", "phase"});
    swing.number("size", 0.0, most, into.size);
    swing.number("period_years", 1.0, most_cycle, into.period);
    swing.number("phase", -most_degrees, most_degrees, into.phase);
    if (into.period <= 0.0) {
        swing.fail(std::format("{} needs period_years", swing.name()));
    }
}

void read_cycles(Reader& cycles, Cycles& into) {
    cycles.only({"tilt", "eccentricity", "precession_years"});
    cycles.list("tilt", into.tilt,
                [](Reader& swing, Cycle& read) { read_cycle(swing, read, 90.0); });
    cycles.list("eccentricity", into.eccentricity,
                [](Reader& swing, Cycle& read) { read_cycle(swing, read, 0.5); });
    cycles.number("precession_years", -most_cycle, most_cycle, into.precession);
}

void add(std::uint64_t& id, double value) {
    id = hash_combine(id, std::bit_cast<std::uint64_t>(value));
}

void add(std::uint64_t& id, const Orbit& orbit) {
    for (const double value : {orbit.distance, orbit.eccentricity, orbit.inclination, orbit.node,
                               orbit.periapsis, orbit.longitude, orbit.period}) {
        add(id, value);
    }
}

} // namespace

std::vector<Wanderer> earths_planets() {
    // JPL's approximate elements and mean radii, with geometric albedos.
    const auto planet = [](double radius_km, double albedo, Orbit orbit) {
        orbit.distance *= astronomical_unit;
        return Wanderer{.radius = radius_km * 1'000.0, .albedo = albedo, .orbit = orbit};
    };
    return {
        planet(2'439.7, 0.142,
               {.distance = 0.387'099'27,
                .eccentricity = 0.205'635'93,
                .inclination = 7.004'979'02,
                .node = 48.330'765'93,
                .periapsis = 77.457'796'28,
                .longitude = 252.250'323'50,
                .period = 0.240'846'7}),
        planet(6'051.8, 0.689,
               {.distance = 0.723'335'66,
                .eccentricity = 0.006'776'72,
                .inclination = 3.394'676'05,
                .node = 76.679'842'55,
                .periapsis = 131.602'467'18,
                .longitude = 181.979'099'50,
                .period = 0.615'197'26}),
        planet(3'389.5, 0.170,
               {.distance = 1.523'710'34,
                .eccentricity = 0.093'394'10,
                .inclination = 1.849'691'42,
                .node = 49.559'538'91,
                .periapsis = -23.943'629'59,
                .longitude = -4.553'432'05,
                .period = 1.880'847'6}),
        planet(69'911.0, 0.538,
               {.distance = 5.202'887'00,
                .eccentricity = 0.048'386'24,
                .inclination = 1.304'396'95,
                .node = 100.473'909'09,
                .periapsis = 14.728'479'83,
                .longitude = 34.396'440'51,
                .period = 11.862'615}),
        planet(58'232.0, 0.499,
               {.distance = 9.536'675'94,
                .eccentricity = 0.053'861'79,
                .inclination = 2.485'991'87,
                .node = 113.662'424'48,
                .periapsis = 92.598'878'31,
                .longitude = 49.954'244'23,
                .period = 29.447'498}),
    };
}

std::uint64_t system_id(const System& system) noexcept {
    std::uint64_t id = hash_values(system.stars.size(), system.moons.size(), system.planets.size(),
                                   system.rings.has_value());
    for (const Star& star : system.stars) {
        for (const double value : {star.luminosity, star.temperature, star.radius, star.tide}) {
            add(id, value);
        }
        add(id, star.orbit);
    }
    add(id, system.orbit);
    for (const Moon& moon : system.moons) {
        for (const double value : {moon.radius, moon.albedo, moon.tide}) {
            add(id, value);
        }
        add(id, moon.orbit);
    }
    if (system.rings) {
        for (const double value : {system.rings->inner, system.rings->outer, system.rings->opacity,
                                   system.rings->albedo}) {
            add(id, value);
        }
    }
    for (const Wanderer& planet : system.planets) {
        add(id, planet.radius);
        add(id, planet.albedo);
        add(id, planet.orbit);
    }
    for (const double value : {system.air.pressure, system.air.oxygen, system.air.carbon_dioxide}) {
        add(id, value);
    }
    id = hash_values(id, system.cycles.tilt.size(), system.cycles.eccentricity.size());
    for (const auto* swings : {&system.cycles.tilt, &system.cycles.eccentricity}) {
        for (const Cycle& swing : *swings) {
            add(id, swing.size);
            add(id, swing.period);
            add(id, swing.phase);
        }
    }
    add(id, system.cycles.precession);
    return id;
}

bool system_key(std::string_view key) noexcept {
    return std::ranges::find(system_keys, key) != system_keys.end();
}

Result<System> parse_system(std::string_view text) {
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& failed = parsed.error();
        return make_error(ErrorCode::Parse, std::format("line {}: {}", failed.source().begin.line,
                                                        failed.description()));
    }
    const toml::table* planet = parsed.table()["planet"].as_table();
    if (planet == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "missing [planet] table");
    }
    System system;
    std::optional<Error> error;
    Reader top{planet, "planet", &error};

    Reader air = top.child("air");
    read_air(air, system.air);
    Reader orbit = top.child("orbit");
    read_orbit(orbit, system.orbit, years);
    if (system.orbit.distance <= 0.0) {
        top.fail("planet.orbit.distance_au must be above 0");
    }
    Reader rings = top.child("rings");
    read_rings(rings, system.rings);
    Reader cycles = top.child("cycles");
    read_cycles(cycles, system.cycles);

    top.list("stars", system.stars, read_star);
    top.list("moons", system.moons, read_moon);
    top.list("planets", system.planets, read_wanderer);
    if (system.stars.empty()) {
        top.fail("planet.stars needs at least one star");
    }

    if (error) {
        return std::unexpected(*error);
    }
    return system;
}

namespace {

void write_double(ByteWriter& out, double value) {
    out.write(std::bit_cast<std::uint64_t>(value));
}

bool read_double(ByteReader& in, double& into) {
    const auto bits = in.read<std::uint64_t>();
    if (!bits) {
        return false;
    }
    into = std::bit_cast<double>(*bits);
    return true;
}

void write_orbit(ByteWriter& out, const Orbit& orbit) {
    for (const double value : {orbit.distance, orbit.eccentricity, orbit.inclination, orbit.node,
                               orbit.periapsis, orbit.longitude, orbit.period}) {
        write_double(out, value);
    }
}

bool read_orbit(ByteReader& in, Orbit& orbit) {
    return read_double(in, orbit.distance) && read_double(in, orbit.eccentricity) &&
           read_double(in, orbit.inclination) && read_double(in, orbit.node) &&
           read_double(in, orbit.periapsis) && read_double(in, orbit.longitude) &&
           read_double(in, orbit.period);
}

// Lists are kept short: no system has more bodies than this.
constexpr std::uint64_t most_listed = 1'000;

template <typename Item, typename Write>
void write_list(ByteWriter& out, const std::vector<Item>& items, Write write) {
    out.write_varint(items.size());
    for (const Item& item : items) {
        write(item);
    }
}

template <typename Item, typename Read>
bool read_list(ByteReader& in, std::vector<Item>& items, Read read) {
    const auto count = in.read_varint();
    if (!count || *count > most_listed) {
        return false;
    }
    items.assign(static_cast<std::size_t>(*count), Item{});
    return std::ranges::all_of(items, read);
}

} // namespace

void write_system(ByteWriter& out, const System& system) {
    write_list(out, system.stars, [&](const Star& star) {
        for (const double value : {star.luminosity, star.temperature, star.radius, star.tide}) {
            write_double(out, value);
        }
        write_orbit(out, star.orbit);
    });
    write_orbit(out, system.orbit);
    const auto cycle = [&](const Cycle& swing) {
        for (const double value : {swing.size, swing.period, swing.phase}) {
            write_double(out, value);
        }
    };
    write_list(out, system.cycles.tilt, cycle);
    write_list(out, system.cycles.eccentricity, cycle);
    write_double(out, system.cycles.precession);
    write_list(out, system.moons, [&](const Moon& moon) {
        for (const double value : {moon.radius, moon.albedo, moon.tide}) {
            write_double(out, value);
        }
        write_orbit(out, moon.orbit);
    });
    out.write(static_cast<std::uint8_t>(system.rings.has_value() ? 1 : 0));
    if (system.rings) {
        for (const double value : {system.rings->inner, system.rings->outer, system.rings->opacity,
                                   system.rings->albedo}) {
            write_double(out, value);
        }
    }
    write_list(out, system.planets, [&](const Wanderer& planet) {
        write_double(out, planet.radius);
        write_double(out, planet.albedo);
        write_orbit(out, planet.orbit);
    });
    for (const double value : {system.air.pressure, system.air.oxygen, system.air.carbon_dioxide}) {
        write_double(out, value);
    }
}

bool read_system(ByteReader& in, System& system) {
    const auto cycle = [&](Cycle& swing) {
        return read_double(in, swing.size) && read_double(in, swing.period) &&
               read_double(in, swing.phase);
    };
    const bool listed =
        read_list(in, system.stars,
                  [&](Star& star) {
                      return read_double(in, star.luminosity) &&
                             read_double(in, star.temperature) && read_double(in, star.radius) &&
                             read_double(in, star.tide) && read_orbit(in, star.orbit);
                  }) &&
        read_orbit(in, system.orbit) && read_list(in, system.cycles.tilt, cycle) &&
        read_list(in, system.cycles.eccentricity, cycle) &&
        read_double(in, system.cycles.precession) && read_list(in, system.moons, [&](Moon& moon) {
            return read_double(in, moon.radius) && read_double(in, moon.albedo) &&
                   read_double(in, moon.tide) && read_orbit(in, moon.orbit);
        });
    const auto ringed = in.read<std::uint8_t>();
    if (!listed || !ringed || *ringed > 1) {
        return false;
    }
    system.rings.reset();
    if (*ringed == 1) {
        Rings rings;
        if (!read_double(in, rings.inner) || !read_double(in, rings.outer) ||
            !read_double(in, rings.opacity) || !read_double(in, rings.albedo)) {
            return false;
        }
        system.rings = rings;
    }
    return read_list(in, system.planets,
                     [&](Wanderer& planet) {
                         return read_double(in, planet.radius) && read_double(in, planet.albedo) &&
                                read_orbit(in, planet.orbit);
                     }) &&
           read_double(in, system.air.pressure) && read_double(in, system.air.oxygen) &&
           read_double(in, system.air.carbon_dioxide);
}

} // namespace hog::planet
