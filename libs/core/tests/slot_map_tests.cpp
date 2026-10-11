#include <hog/core/slot_map.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

struct Npc {};
struct Item {};

static_assert(!std::is_convertible_v<hog::Handle<Npc>, hog::Handle<Item>>,
              "handles with different tags must not mix");

} // namespace

TEST_CASE("a default handle is null and never contained", "[core][slot_map]") {
    const hog::SlotMap<int> map;
    const hog::SlotMap<int>::Key key;
    CHECK(key.is_null());
    CHECK_FALSE(map.contains(key));
}

TEST_CASE("inserted values can be read and written through their handle", "[core][slot_map]") {
    hog::SlotMap<std::string> map;
    const auto a = map.insert("alpha");
    const auto b = map.emplace(3, 'b');

    CHECK_FALSE(a.is_null());
    CHECK(a != b);
    CHECK(map.size() == 2);
    CHECK(map.get(a) == "alpha");
    CHECK(map.get(b) == "bbb");

    map.get(a) = "changed";
    CHECK(map.get(a) == "changed");
}

TEST_CASE("an erased handle goes stale", "[core][slot_map]") {
    hog::SlotMap<int> map;
    const auto key = map.insert(7);

    CHECK(map.erase(key));
    CHECK_FALSE(map.contains(key));
    CHECK_FALSE(map.erase(key));
    CHECK(map.empty());

    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(map.get(key), hog::testing::AssertionFailed);
}

TEST_CASE("a reused slot does not revive old handles", "[core][slot_map]") {
    hog::SlotMap<int> map;
    const auto old_key = map.insert(1);
    map.erase(old_key);
    const auto new_key = map.insert(2);

    CHECK(new_key.index() == old_key.index());
    CHECK(new_key.generation() != old_key.generation());
    CHECK_FALSE(map.contains(old_key));
    CHECK(map.get(new_key) == 2);
}

TEST_CASE("erasing from the middle keeps every other value reachable", "[core][slot_map]") {
    hog::SlotMap<int> map;
    std::vector<hog::SlotMap<int>::Key> keys;
    keys.reserve(5);
    for (int i = 0; i < 5; ++i) {
        keys.push_back(map.insert(i * 10));
    }

    map.erase(keys[1]);
    map.erase(keys[3]);

    CHECK(map.size() == 3);
    CHECK(map.get(keys[0]) == 0);
    CHECK(map.get(keys[2]) == 20);
    CHECK(map.get(keys[4]) == 40);
}

TEST_CASE("for_each visits every live value with its own key", "[core][slot_map]") {
    hog::SlotMap<int> map;
    const auto a = map.insert(1);
    const auto b = map.insert(2);
    const auto c = map.insert(3);
    map.erase(b);

    std::vector<hog::SlotMap<int>::Key> seen;
    map.for_each([&](auto key, int& value) {
        CHECK(map.get(key) == value);
        seen.push_back(key);
    });

    std::ranges::sort(seen);
    std::vector expected{a, c};
    std::ranges::sort(expected);
    CHECK(seen == expected);
    CHECK(map.values().size() == 2);
}

TEST_CASE("clear invalidates every handle", "[core][slot_map]") {
    hog::SlotMap<int> map;
    const auto a = map.insert(1);
    const auto b = map.insert(2);

    map.clear();

    CHECK(map.empty());
    CHECK_FALSE(map.contains(a));
    CHECK_FALSE(map.contains(b));
    const auto c = map.insert(3);
    CHECK(map.get(c) == 3);
}

TEST_CASE("a slot whose generation is exhausted is retired, not reused", "[core][slot_map]") {
    // An 8-bit generation runs out after 128 insert/erase cycles on one slot.
    hog::SlotMap<int, int, std::uint8_t> map;
    std::vector<hog::SlotMap<int, int, std::uint8_t>::Key> issued;
    auto key = map.insert(0);
    const auto first_index = key.index();
    while (key.index() == first_index) {
        issued.push_back(key);
        map.erase(key);
        key = map.insert(0);
    }

    CHECK(issued.size() == 128);
    CHECK(issued.back().generation() == 255);
    for (const auto old_key : issued) {
        CHECK_FALSE(map.contains(old_key));
    }
}

TEST_CASE("a throwing constructor leaves the map unchanged", "[core][slot_map]") {
    struct Fragile {
        explicit Fragile(bool fail) {
            if (fail) {
                throw std::runtime_error("construction failed");
            }
        }
    };

    hog::SlotMap<Fragile> map;
    const auto kept = map.emplace(false);
    CHECK_THROWS_AS(map.emplace(true), std::runtime_error);
    CHECK(map.size() == 1);
    CHECK(map.contains(kept));
    const auto next = map.emplace(false);
    CHECK(map.contains(next));
    CHECK(map.size() == 2);
}

TEST_CASE("random operations match a reference model", "[core][slot_map]") {
    using Map = hog::SlotMap<std::uint64_t>;
    Map map;
    std::map<Map::Key, std::uint64_t> model;
    std::vector<Map::Key> ever_issued;
    std::mt19937_64 rng(0x406'5EED);

    for (int step = 0; step < 20'000; ++step) {
        const auto op = rng() % 10;
        if (op < 5 || model.empty()) {
            const auto value = rng();
            const auto key = map.insert(value);
            REQUIRE_FALSE(model.contains(key));
            model.emplace(key, value);
            ever_issued.push_back(key);
        } else if (op < 8) {
            const auto key = ever_issued[rng() % ever_issued.size()];
            REQUIRE(map.erase(key) == (model.erase(key) == 1));
        } else if (op < 9) {
            auto it = model.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(rng() % model.size()));
            it->second = rng();
            map.get(it->first) = it->second;
        } else if (rng() % 200 == 0) {
            map.clear();
            model.clear();
        }

        REQUIRE(map.size() == model.size());
    }

    for (const auto key : ever_issued) {
        const auto it = model.find(key);
        REQUIRE(map.contains(key) == (it != model.end()));
        if (it != model.end()) {
            REQUIRE(map.get(key) == it->second);
        }
    }
}
