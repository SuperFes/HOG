#pragma once

// Marks for the Tracy profiler. They compile to nothing unless the build defines HOG_PROFILE, and
// even then gather nothing until a profiler connects. Names must be string literals.

#ifdef HOG_PROFILE
#include <tracy/Tracy.hpp>
#endif

namespace hog::profile {

#ifdef HOG_PROFILE
inline constexpr bool enabled = true;
#else
inline constexpr bool enabled = false;
#endif

} // namespace hog::profile

#ifdef HOG_PROFILE
// Times the rest of the enclosing scope.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): declares a scoped zone at the caller's line
#define HOG_ZONE(name) ZoneScopedN(name)
// Labels the enclosing zone with text that changes from call to call, such as a pass's name.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): reaches the zone HOG_ZONE declared
#define HOG_ZONE_TEXT(text) ZoneText((text).data(), (text).size())
// Ends a frame; the profiler shows the time between marks.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the profiler's frame mark is a macro
#define HOG_FRAME() FrameMark
// One sample of a value graphed over time.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the profiler keys plots by the literal's address
#define HOG_PLOT(name, value) TracyPlot(name, value)
// Names the calling thread in the profiler.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): kept beside the other marks
#define HOG_THREAD(name) tracy::SetThreadName(name)
#else
// NOLINTBEGIN(cppcoreguidelines-macro-usage): no-op twins of the marks above
#define HOG_ZONE(name) static_cast<void>(0)
#define HOG_ZONE_TEXT(text) static_cast<void>(0)
#define HOG_FRAME() static_cast<void>(0)
#define HOG_PLOT(name, value) static_cast<void>(0)
#define HOG_THREAD(name) static_cast<void>(0)
// NOLINTEND(cppcoreguidelines-macro-usage)
#endif
