# Contributing

## Building

Needs CMake 3.28+, Ninja, and Clang 19+ (the default and primary toolchain), GCC 14+ or MSVC 2022 17.10+. Dependencies are fetched by [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) into `.cache/cpm` on first configure. On Linux the debug viewer's SDL3 wants X11 or Wayland headers (on Ubuntu: `libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libegl1-mesa-dev`); without them it builds without a window system, and `-DHOG_BUILD_VIEWER=OFF` skips it.

```sh
cmake --workflow --preset debug        # configure, build and run all tests
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

| Preset | Purpose |
| --- | --- |
| `debug` | Day-to-day development |
| `release` | Optimized with debug info |
| `asan` | Address and undefined-behaviour sanitizers |
| `tsan` | Thread sanitizer |
| `ci`, `ci-asan`, `ci-tsan` | As above with warnings as errors |

Benchmarks live in each library's `bench/` directory as Catch2 `BENCHMARK`s, built into separate `<lib>_bench` executables that ctest never runs. Run them on a release build: `cmake --build --preset release --target benchmarks`.

World generation is pinned by golden hashes in `libs/planet/tests/golden/tiles.txt`, which every CI platform must reproduce. After an intended change to generated output, rewrite the file with `hog_planet_tests "[.update-golden]"` and commit it with the change.

Run a development server with `build/debug/apps/server/hog-server` from the repository root. It reads `config/server.toml`, keeps the world and its logs under `var/`, catches up to now on start, and saves when stopped with Ctrl-C. Watch it with `build/debug/tools/viewer/hog-viewer`, which connects to the `[debug]` port (7777 in the dev config, loopback only) and draws the weather, plants and wildlife on a map of the planet: wheel to zoom, drag to pan, double-click for the whole planet. `--help` lists its options.

Presets use `clang++`; pick another compiler with `-DCMAKE_CXX_COMPILER=g++` (or `cl` on Windows). `-DHOG_USE_LIBCXX=ON` builds against libc++, as macOS does; CI's Linux Clang jobs use it. Libraries live in `libs/<name>/` with `include/hog/<name>/`, `src/` and `tests/`; each registers its tests with `hog_add_tests()`. Executables keep their logic in a tested library beside a thin `main.cpp`: `tools/` for developer tools, `apps/` for what ships.

## Code rules

- **C++23.** Use C++26 library hardening as compilers ship it; until then the hardened standard-library modes are on in every build.
- **No owning raw pointers.** Objects refer to each other through generational handles (`hog::Handle`), not pointers. Raw pointers and `new`/`delete` appear only at the boundary with external C libraries, wrapped immediately.
- **Static dispatch by default.** Use templates, `std::variant` or plain functions for sets known at compile time. Use a virtual interface only when the call site genuinely cannot know the concrete type (a heterogeneous container, a set that is open at runtime), and never per element in a hot loop.
- **Errors are values.** Fallible functions return `hog::Result<T>` (`std::expected`). Exceptions never cross a library boundary.
- **Determinism where it matters.** World generation and simulation must give bit-identical results on every platform and compiler: integer or fixed-point maths, stable hashing, no unordered iteration.
- **Tests for everything.** Every library change comes with unit tests. Refactor for testability rather than skip a test.
- **Build in small steps.** Land stubs that compile and pass tests, then fill them in, so every regression can be traced through history.
- **Comments say why, not what.** Short, about the code, never about the task in progress.

## Naming

`clang-tidy` enforces these:

| Kind | Style | Example |
| --- | --- | --- |
| Namespaces, functions, variables | `lower_case` | `hog::slot_count()` |
| Types, enums, enum values, concepts, template parameters | `CamelCase` | `SlotMap`, `CellId` |
| Private and protected members | `lower_case_` | `generation_` |
| Macros | `HOG_UPPER_CASE` | `HOG_ASSERT` |

Headers use `#pragma once`.

## Formatting and linting

`.clang-format` and `.clang-tidy` at the repo root are the authority; CI checks both. Run `clang-format -i` on changed files before committing.

## Commits and reviews

- Short, imperative, conventional commits: `feat(core): add slot map`, `fix(planet): wrap neighbor across face edge`, `docs: ...`, `build: ...`, `ci: ...`, `test: ...`.
- Add a body only for context the diff can't show.
- Each pull request is reviewed by one other person and must pass CI.
- Set your git identity per repo (`git config user.name` / `user.email` inside the clone).
