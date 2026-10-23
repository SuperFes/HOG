#include <hog/core/flat_map.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("a flat map starts empty", "[core][flat_map]") {
    const hog::FlatMap<int, std::string> map;
    CHECK(map.empty());
    CHECK(map.size() == 0);
    CHECK(map.begin() == map.end());
    CHECK_FALSE(map.contains(3));
    CHECK(map.find(3) == map.end());
    CHECK(map.lower_bound(3) == map.end());
}

TEST_CASE("a flat map keeps its entries in key order", "[core][flat_map]") {
    hog::FlatMap<int, std::string> map;
    for (const int key : {5, 1, 9, 3, 7}) {
        CHECK(map.emplace(key, std::to_string(key)).second);
    }
    std::vector<std::pair<int, std::string>> entries(map.begin(), map.end());
    CHECK(entries == std::vector<std::pair<int, std::string>>{
                         {1, "1"}, {3, "3"}, {5, "5"}, {7, "7"}, {9, "9"}});
    CHECK(map.lower_bound(4)->first == 5);
    CHECK(map.lower_bound(5)->first == 5);
    CHECK(map.lower_bound(10) == map.end());
}

TEST_CASE("a flat map keeps the first value emplaced for a key", "[core][flat_map]") {
    hog::FlatMap<int, std::string> map;
    CHECK(map.emplace(2, "first").second);
    const auto [at, added] = map.emplace(2, "second");
    CHECK_FALSE(added);
    CHECK(at->second == "first");
    CHECK(map.size() == 1);
}

TEST_CASE("a flat map's values change in place", "[core][flat_map]") {
    hog::FlatMap<int, int> map;
    (void)map.emplace(4, 40);
    map.at(4) += 2;
    map.find(4)->second += 1;
    CHECK(map.at(4) == 43);
}

TEST_CASE("a flat map erases by key, saying whether it did", "[core][flat_map]") {
    hog::FlatMap<int, int> map;
    for (const int key : {1, 2, 3}) {
        (void)map.emplace(key, key * 10);
    }
    CHECK(map.erase(2) == 1);
    CHECK(map.erase(2) == 0);
    CHECK_FALSE(map.contains(2));
    CHECK(map.at(1) == 10);
    CHECK(map.at(3) == 30);
    CHECK(map.size() == 2);
}

TEST_CASE("a flat map refuses a key it doesn't hold", "[core][flat_map]") {
    const hog::testing::AssertionGuard guard;
    hog::FlatMap<int, int> map;
    (void)map.emplace(1, 1);
    CHECK_THROWS_AS((void)map.at(2), hog::testing::AssertionFailed);
}

TEST_CASE("a flat map answers as std::map does", "[core][flat_map]") {
    std::mt19937 random{7};
    std::uniform_int_distribution<int> keys{0, 200};
    hog::FlatMap<int, int> flat;
    std::map<int, int> tree;
    for (int step = 0; step < 2'000; ++step) {
        const int key = keys(random);
        if (step % 3 == 0) {
            CHECK(flat.erase(key) == tree.erase(key));
        } else {
            CHECK(flat.emplace(key, step).second == tree.emplace(key, step).second);
        }
        const int probe = keys(random);
        CHECK(flat.contains(probe) == tree.contains(probe));
        const auto flat_at = flat.lower_bound(probe);
        const auto tree_at = tree.lower_bound(probe);
        REQUIRE((flat_at == flat.end()) == (tree_at == tree.end()));
        if (flat_at != flat.end()) {
            CHECK(*flat_at == *tree_at);
        }
    }
    CHECK(std::vector<std::pair<int, int>>(flat.begin(), flat.end()) ==
          std::vector<std::pair<int, int>>(tree.begin(), tree.end()));
}
