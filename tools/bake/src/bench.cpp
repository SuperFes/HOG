#include <hog/baked/node.hpp>
#include <hog/baked/store.hpp>
#include <hog/core/hash.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view usage =
    "usage: hog-baked-bench --store DIR [--readers N] [--seconds S] [--near METRES]\n"
    "Reads voxels from a baked store from N threads at once (200 by default) for S seconds\n"
    "(10 by default) while one writer keeps writing records back, and reports how fast and how\n"
    "long each read takes. Each reader wanders, every read within --near voxels of its last\n"
    "(64 by default), jumping to a random record now and then, as a player's lookups do.\n";

using Clock = std::chrono::steady_clock;

struct Settings {
    std::string store;
    unsigned readers = 200;
    double seconds = 10.0;
    std::uint32_t near = 64;
};

struct Shared {
    hog::baked::Store* store = nullptr;
    std::span<const hog::baked::Node> nodes;
    std::uint32_t near = 64;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> missing{0};
    std::atomic<std::uint64_t> writes{0};
};

// One reader: a walk of reads near each other, jumping to a random record now and then.
void read_on(Shared& shared, unsigned reader, std::vector<double>& latencies) {
    auto snapshot = shared.store->read();
    if (!snapshot) {
        return;
    }
    std::uint64_t n = 0;
    hog::baked::Point at{};
    std::uint64_t reads = 0;
    std::uint64_t missing = 0;
    const auto draw = [&](std::uint64_t axis, std::uint64_t below) {
        return hog::hash_values(std::uint64_t{reader}, n, axis) % below;
    };
    const auto step = [&](std::uint32_t v, std::uint64_t axis) {
        const auto by =
            static_cast<std::int64_t>(draw(axis, (2 * std::uint64_t{shared.near}) + 1)) -
            static_cast<std::int64_t>(shared.near);
        return static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, static_cast<std::int64_t>(v) + by));
    };
    while (!shared.stop.load(std::memory_order_relaxed)) {
        if (n % 4096 == 0) {
            const hog::baked::Node& node = shared.nodes[draw(0, shared.nodes.size())];
            at = {.face = node.face,
                  .x = node.x + static_cast<std::uint32_t>(draw(1, node.side())),
                  .y = node.y + static_cast<std::uint32_t>(draw(2, node.side())),
                  .z = node.z + static_cast<std::uint32_t>(draw(3, node.side()))};
        } else {
            at.x = step(at.x, 4);
            at.y = step(at.y, 5);
            at.z = step(at.z, 6);
        }
        const bool timed = n % 64 == 0;
        const auto began = timed ? Clock::now() : Clock::time_point{};
        const auto leaf = snapshot->at(at);
        if (timed) {
            latencies.push_back(
                std::chrono::duration<double, std::micro>(Clock::now() - began).count());
        }
        ++reads;
        missing += leaf ? 0U : 1U;
        ++n;
        if (n % 1024 == 0 && !snapshot->refresh()) {
            break;
        }
    }
    shared.reads += reads;
    shared.missing += missing;
}

// The writer: each batch writes a few records back as they are.
void write_on(Shared& shared, std::vector<double>& commits) {
    std::uint64_t n = 0;
    while (!shared.stop.load(std::memory_order_relaxed)) {
        const auto began = Clock::now();
        const auto done = shared.store->write([&](hog::baked::Batch& batch) -> hog::Result<void> {
            for (int k = 0; k < 8; ++k, ++n) {
                const hog::baked::Node& node =
                    shared.nodes[hog::hash_values(std::uint64_t{99}, n) % shared.nodes.size()];
                const auto record = batch.record(
                    hog::baked::Point{.face = node.face, .x = node.x, .y = node.y, .z = node.z});
                if (!record || record->node != node) {
                    continue;
                }
                const std::vector<std::byte> page{record->page.bytes().begin(),
                                                  record->page.bytes().end()};
                if (auto put = batch.put(node, page); !put) {
                    return put;
                }
            }
            return {};
        });
        if (!done) {
            std::cerr << std::format("hog-baked-bench: {}\n", done.error());
            return;
        }
        commits.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
        shared.writes += 8;
    }
}

double percentile(std::vector<double>& samples, double share) {
    if (samples.empty()) {
        return 0.0;
    }
    const auto at = static_cast<std::size_t>(share * static_cast<double>(samples.size() - 1));
    std::ranges::nth_element(samples, samples.begin() + static_cast<std::ptrdiff_t>(at));
    return samples[at];
}

int run(std::span<const std::string_view> arguments) {
    Settings settings;
    for (std::size_t k = 0; k + 1 < arguments.size(); k += 2) {
        const std::string value{arguments[k + 1]};
        if (arguments[k] == "--store") {
            settings.store = value;
        } else if (arguments[k] == "--readers") {
            settings.readers = static_cast<unsigned>(std::stoul(value));
        } else if (arguments[k] == "--seconds") {
            settings.seconds = std::stod(value);
        } else if (arguments[k] == "--near") {
            settings.near = static_cast<std::uint32_t>(std::stoul(value));
        } else {
            std::cerr << usage;
            return EXIT_FAILURE;
        }
    }
    if (settings.store.empty() || arguments.size() % 2 != 0) {
        std::cerr << usage;
        return EXIT_FAILURE;
    }
    auto store = hog::baked::Store::open(
        settings.store, {.map_bytes = std::uint64_t{256} << 30U, .readers = settings.readers + 16});
    if (!store) {
        std::cerr << std::format("hog-baked-bench: {}\n", store.error());
        return EXIT_FAILURE;
    }

    // Every record's node, to start reads from and to write back.
    std::vector<hog::baked::Node> nodes;
    {
        auto snapshot = store->read();
        if (!snapshot) {
            std::cerr << std::format("hog-baked-bench: {}\n", snapshot.error());
            return EXIT_FAILURE;
        }
        for (std::uint8_t face = 0; face < 6; ++face) {
            snapshot->each(hog::baked::Node{.face = face, .size = hog::baked::coordinate_bits},
                           [&](const hog::baked::Record& record) { nodes.push_back(record.node); });
        }
    }
    if (nodes.empty()) {
        std::cerr << "hog-baked-bench: the store holds nothing\n";
        return EXIT_FAILURE;
    }
    std::cout << std::format("{} records, {} readers, {:.0f} s\n", nodes.size(), settings.readers,
                             settings.seconds);

    Shared shared{.store = &*store, .nodes = nodes, .near = settings.near};
    std::vector<std::vector<double>> latencies(settings.readers);
    std::vector<double> commits;
    std::vector<std::thread> threads;
    threads.reserve(settings.readers + 1);
    for (unsigned r = 0; r < settings.readers; ++r) {
        threads.emplace_back([&, r] { read_on(shared, r, latencies[r]); });
    }
    threads.emplace_back([&] { write_on(shared, commits); });

    const auto started = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(settings.seconds));
    shared.stop = true;
    for (std::thread& thread : threads) {
        thread.join();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();

    std::vector<double> all;
    for (const std::vector<double>& own : latencies) {
        all.insert(all.end(), own.begin(), own.end());
    }
    std::cout << std::format("reads: {:.1f} M/s ({} in all, {} outside the bake)\n",
                             static_cast<double>(shared.reads.load()) / elapsed / 1e6,
                             shared.reads.load(), shared.missing.load());
    std::cout << std::format("read µs: p50 {:.2f}  p99 {:.2f}  p99.9 {:.2f}  max {:.1f}\n",
                             percentile(all, 0.5), percentile(all, 0.99), percentile(all, 0.999),
                             percentile(all, 1.0));
    std::cout << std::format(
        "writes: {} records in {} batches, ms per batch p50 {:.2f} p99 {:.2f}\n",
        shared.writes.load(), commits.size(), percentile(commits, 0.5), percentile(commits, 0.99));
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span raw{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(raw.begin() + 1, raw.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-baked-bench: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
