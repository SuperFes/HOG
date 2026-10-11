#include <hog/core/build_info.hpp>
#include <hog/core/version.hpp>

namespace hog {

namespace {

// Clang also defines __GNUC__, so it must be tested first.
#if defined(__clang__)
constexpr std::string_view compiler_text = "clang " __clang_version__;
#elif defined(__GNUC__)
constexpr std::string_view compiler_text = "gcc " __VERSION__;
#elif defined(_MSC_VER)
#define HOG_STRINGIFY_IMPL(x) #x
#define HOG_STRINGIFY(x) HOG_STRINGIFY_IMPL(x)
constexpr std::string_view compiler_text = "msvc " HOG_STRINGIFY(_MSC_FULL_VER);
#else
constexpr std::string_view compiler_text = "unknown";
#endif

} // namespace

BuildInfo build_info() noexcept {
    return {.version = version_text, .compiler = compiler_text, .config = HOG_BUILD_CONFIG};
}

} // namespace hog
