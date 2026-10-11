#include <hog/core/slot_map.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t count = 100'000;

struct Payload {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    std::uint32_t id = 0;
};

} // namespace

TEST_CASE("slot map against unordered_map", "[core][slot_map][benchmark]") {
    BENCHMARK("slot map: insert 100k") {
        hog::SlotMap<Payload> map;
        map.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            map.insert(Payload{.id = i});
        }
        return map.size();
    };

    BENCHMARK("unordered_map: insert 100k") {
        std::unordered_map<std::uint32_t, Payload> map;
        map.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            map.emplace(i, Payload{.id = i});
        }
        return map.size();
    };

    hog::SlotMap<Payload> slots;
    std::vector<hog::SlotMap<Payload>::Key> keys;
    std::unordered_map<std::uint32_t, Payload> table;
    keys.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        keys.push_back(slots.insert(Payload{.id = i}));
        table.emplace(i, Payload{.id = i});
    }

    BENCHMARK("slot map: look up 100k") {
        std::uint64_t sum = 0;
        for (const auto key : keys) {
            sum += slots.get(key).id;
        }
        return sum;
    };

    BENCHMARK("unordered_map: look up 100k") {
        std::uint64_t sum = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            sum += table.at(i).id;
        }
        return sum;
    };

    BENCHMARK("slot map: iterate 100k") {
        return std::accumulate(slots.values().begin(), slots.values().end(), std::uint64_t{0},
                               [](std::uint64_t sum, const Payload& p) { return sum + p.id; });
    };

    BENCHMARK("unordered_map: iterate 100k") {
        std::uint64_t sum = 0;
        for (const auto& [id, payload] : table) {
            sum += payload.id;
        }
        return sum;
    };
}
