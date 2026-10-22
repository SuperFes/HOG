#include <hog/testing/quick.hpp>

#include <cstdlib>
#include <string_view>

namespace hog::testing {

bool quick_from(const char* value) noexcept {
    return value != nullptr && *value != '\0' && std::string_view{value} != "0";
}

bool quick() noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing sets the environment while tests run
    static const bool asked = quick_from(std::getenv("HOG_QUICK_TESTS"));
    return asked;
}

} // namespace hog::testing
