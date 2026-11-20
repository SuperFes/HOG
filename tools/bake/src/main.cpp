#include <hog/bake/bake.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/baked/filling.hpp>
#include <hog/baked/generating.hpp>
#include <hog/baked/wants_table.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/globe/planet.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view usage =
    "usage: hog-bake --store DIR (EVERYWHERE | --at LAT,LON [--across METRES] | both | --serve)\n"
    "                [--planet FILE] [--threads N] [--map-gb N]\n"
    "       EVERYWHERE is one of --coarse METRES, --depth LEVEL or --band BAND\n"
    "Bakes the world's grid into the store at DIR ahead of play: over the whole planet down\n"
    "through a band, the one holding the first level whose nodes are --coarse metres across\n"
    "or less, the one holding level --depth, or band --band itself; and within --across metres\n"
    "(1000 by default) about --at down to the finest. Regions already\n"
    "held as this generator makes them are kept, so a stopped bake carries on. With --serve,\n"
    "fills what a server's players wait on instead, as its wants table beside the store asks,\n"
    "until stopped. The planet is config/planet.toml where that's found, else the default.\n";

struct Options {
    hog::globe::Planet planet;
    std::optional<std::filesystem::path> store;
    std::uint32_t threads = 0;
    std::uint64_t map_gb = 256;
    std::optional<double> coarse;
    std::optional<std::uint32_t> depth;
    std::optional<std::uint32_t> band;
    std::optional<std::pair<double, double>> at; // degrees
    double across = 1000.0;
    bool serve = false;
};

// Set from a signal handler, which may only touch lock-free atomics, so it has to be global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void stop_serving(int /*signal*/) {
    stopping.store(true, std::memory_order_relaxed);
}

// Fills what the wants table beside the store asks for until stopped.
int serve(const hog::globe::Generator& generator, hog::baked::ChunkStore& store,
          std::uint32_t threads) {
    auto wants = hog::baked::WantsTable::open(hog::baked::WantsTable::beside(store.directory()));
    if (!wants) {
        std::cerr << std::format("hog-bake: {}\n", wants.error());
        return EXIT_FAILURE;
    }
    hog::jobs::JobSystem jobs{threads};
    hog::baked::Generating generating{generator, store, *wants, jobs};
    for (const int signal : {SIGINT, SIGTERM}) {
        if (std::signal(signal, stop_serving) == SIG_ERR) {
            std::cerr << std::format("hog-bake: can't handle signal {}\n", signal);
        }
    }
    std::cerr << std::format("serving the wants for {}\n", store.directory().string());
    bool busy = false;
    while (!stopping) {
        const auto stepped = generating.step(busy);
        if (!stepped) {
            std::cerr << std::format("hog-bake: {}\n", stepped.error());
        }
        busy = stepped.value_or(false);
        if (!busy) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    return EXIT_SUCCESS;
}

std::optional<double> number(std::string_view text) {
    const std::string part{text};
    char* end = nullptr;
    const double value = std::strtod(part.c_str(), &end);
    if (part.empty() || end != std::to_address(part.end()) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// Each option and its value; false on anything it doesn't know.
bool apply(Options& options, std::string_view name, std::string_view value) {
    if (name == "--planet") {
        const auto loaded = hog::globe::load_planet(value);
        if (!loaded) {
            std::cerr << std::format("hog-bake: {}\n", loaded.error());
        }
        options.planet = loaded.value_or(options.planet);
        return loaded.has_value();
    }
    if (name == "--store") {
        options.store = value;
        return true;
    }
    if (name == "--at") {
        const std::size_t comma = value.find(',');
        const auto latitude = number(value.substr(0, comma));
        const auto longitude =
            comma == std::string_view::npos ? std::nullopt : number(value.substr(comma + 1));
        if (!latitude || !longitude) {
            return false;
        }
        options.at = {*latitude, *longitude};
        return true;
    }
    const auto given = number(value);
    if (!given || *given < 0.0) {
        return false;
    }
    if (name == "--threads") {
        options.threads = static_cast<std::uint32_t>(*given);
    } else if (name == "--map-gb" && *given >= 1.0) {
        options.map_gb = static_cast<std::uint64_t>(*given);
    } else if (name == "--coarse" && *given > 0.0) {
        options.coarse = given;
    } else if (name == "--depth" && *given == std::floor(*given) && *given <= 255.0) {
        options.depth = static_cast<std::uint32_t>(*given);
    } else if (name == "--band" && *given == std::floor(*given) && *given <= 255.0) {
        options.band = static_cast<std::uint32_t>(*given);
    } else if (name == "--across" && *given > 0.0) {
        options.across = *given;
    } else {
        return false;
    }
    return true;
}

hog::DVec3 direction_of(double latitude, double longitude) {
    constexpr double degrees = std::numbers::pi / 180.0;
    const double lat = latitude * degrees;
    const double lon = longitude * degrees;
    return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

// The band baked everywhere, from whichever of --coarse, --depth and --band was given.
std::expected<std::optional<std::uint8_t>, std::string>
band_asked(const Options& options, const hog::grid::Grid& grid, const hog::baked::Bands& bands) {
    if (options.coarse) {
        return bands.band_of(hog::bake::depth_of(grid, *options.coarse));
    }
    if (options.depth) {
        if (*options.depth > grid.depth()) {
            return std::unexpected{std::format("--depth goes to {} at most", grid.depth())};
        }
        return bands.band_of(static_cast<std::uint8_t>(*options.depth));
    }
    if (options.band) {
        if (*options.band >= bands.count()) {
            return std::unexpected{std::format("--band goes to {} at most", bands.count() - 1U)};
        }
        return static_cast<std::uint8_t>(*options.band);
    }
    return std::nullopt;
}

// The options given, over the planet config where that's found.
std::expected<Options, std::string> parse(std::span<const std::string_view> arguments) {
    Options options;
    if (std::filesystem::exists("config/planet.toml")) {
        if (const auto loaded = hog::globe::load_planet("config/planet.toml")) {
            options.planet = *loaded;
        }
    }
    for (std::size_t k = 0; k < arguments.size();) {
        if (arguments[k] == "--serve") {
            options.serve = true;
            ++k;
            continue;
        }
        if (k + 1 >= arguments.size()) {
            return std::unexpected{std::format("{} needs a value", arguments[k])};
        }
        if (!apply(options, arguments[k], arguments[k + 1])) {
            return std::unexpected{std::format("can't take {} {}", arguments[k], arguments[k + 1])};
        }
        k += 2;
    }
    return options;
}

int run(std::span<const std::string_view> arguments) {
    const auto refuse = [](std::string_view why) {
        std::cerr << std::format("hog-bake: {}\n\n{}", why, usage);
        return EXIT_FAILURE;
    };
    const auto parsed = parse(arguments);
    if (!parsed) {
        return refuse(parsed.error());
    }
    const Options& options = *parsed;
    if (!options.store) {
        return refuse("--store is required");
    }
    const int everywhere = static_cast<int>(options.coarse.has_value()) +
                           static_cast<int>(options.depth.has_value()) +
                           static_cast<int>(options.band.has_value());
    if (everywhere > 1) {
        return refuse("give one of --coarse, --depth and --band");
    }
    if (everywhere == 0 && !options.at && !options.serve) {
        return refuse("give --coarse, --depth or --band, --at, or both, to say what to bake, or "
                      "--serve");
    }
    if (options.serve && (everywhere > 0 || options.at)) {
        return refuse("--serve bakes nothing ahead");
    }
    const auto formed = std::chrono::steady_clock::now();
    const hog::globe::Generator generator{options.planet, [](std::string_view stage) {
                                              std::cerr << std::format("forming: {}\n", stage);
                                          }};
    std::cerr << std::format(
        "formed in {:.1f} s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - formed).count());
    auto store = hog::baked::ChunkStore::open(*options.store, hog::baked::baking_of(generator),
                                              {.map_bytes = options.map_gb << 30U,
                                               .readers = 1024,
                                               .read_only = false,
                                               .band_height = 3,
                                               .budget = std::size_t{32} << 10U});
    if (!store) {
        std::cerr << std::format("hog-bake: {}\n", store.error());
        return EXIT_FAILURE;
    }
    if (options.serve) {
        return serve(generator, *store, options.threads);
    }
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::baked::Bands& bands = store->bands();
    hog::bake::Reach reach;
    if (auto band = band_asked(options, grid, bands)) {
        reach.band = *band;
    } else {
        return refuse(band.error());
    }
    if (options.at) {
        const hog::DVec3 direction = direction_of(options.at->first, options.at->second);
        reach.around =
            direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
        reach.across = options.across;
    }
    if (reach.band) {
        const std::uint8_t foot = bands.foot(*reach.band);
        std::cerr << std::format(
            "everywhere through band {} of {}, down to level {} of {}, nodes {:.3g} m across\n",
            *reach.band, bands.count(), foot, grid.depth(), grid.size(foot));
    }
    hog::jobs::JobSystem jobs{options.threads};
    const auto started = std::chrono::steady_clock::now();
    // The rate over the last few seconds, as the whole run's average hides how it goes now.
    auto lately = started;
    std::size_t done_lately = 0;
    const auto baked = hog::bake::bake(
        generator, *store, jobs, reach,
        {.batch = std::size_t{4} * jobs.thread_count(), .fill_levels = 6},
        [&](const hog::bake::Progressed& at) {
            const auto now = std::chrono::steady_clock::now();
            const double since = std::chrono::duration<double>(now - lately).count();
            if (since >= 2.0 || at.waiting == 0) {
                const double rate =
                    static_cast<double>(at.done - done_lately) / std::max(since, 1e-3);
                lately = now;
                done_lately = at.done;
                std::cerr << std::format("{:5.1f}% {} regions done, {:.1f} a second lately, {} "
                                         "found waiting, {:.0f} s\n",
                                         100.0 * at.share, at.done, rate, at.waiting,
                                         std::chrono::duration<double>(now - started).count());
            }
        });
    if (!baked) {
        std::cerr << std::format("hog-bake: {}\n", baked.error());
        return EXIT_FAILURE;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto megabytes = [](std::size_t bytes) { return static_cast<double>(bytes) / 1e6; };
    std::cout << std::format(
        "{} regions filled in {:.1f} s, {} already held; trees: shape {:.1f} MB, material {:.1f} "
        "MB, water {:.1f} MB, woods {:.1f} MB\n",
        baked->filled, seconds, baked->kept, megabytes(baked->shape_bytes),
        megabytes(baked->material_bytes), megabytes(baked->water_bytes),
        megabytes(baked->woods_bytes));
    if (const auto stats = store->stats()) {
        std::cout << std::format("store: {} chunks, {:.1f} MB\n", stats->chunks,
                                 megabytes(stats->bytes));
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span raw{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(raw.begin() + 1, raw.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-bake: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
