# HOG

A 3D MMORPG and engine built around one seamless, round planet where players, NPCs and creatures share a living world that changes permanently.

The world is generated deterministically and grown the same on every machine: terrain, weather, seas, grass, bushes and anchored trees, with a day-night sky and clouds simulated server-side. The client flies over it today; the simulation, creatures and play are being built up phase by phase ([roadmap](docs/roadmap.md)).

## What builds

| Target | What it is |
| --- | --- |
| `apps/client/hog-client` | The game client: flies over the planet (Vulkan, SDL3) |
| `apps/server/hog-server` | The world server: weather, plants, wildlife, saves under `var/` |
| `apps/login/hog-login` | The login service issuing connect tokens |
| `apps/bot/hog-bot` | A headless client for load and soak testing |
| `tools/viewer/hog-viewer` | A map viewer watching a running server's debug port |
| `tools/mapgen/hog-mapgen` | Renders planet maps to images |

## Dependencies

Host toolchain:

- **CMake 3.28+** and **Ninja**
- **Clang 19+** (default and primary), **GCC 14+**, or **MSVC 2022 17.10+** — C++23
- **Git** and network access on first configure: every third-party library is pinned and fetched by [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) into `.cache/cpm`, including the pinned [Slang](https://github.com/shader-slang/slang) compiler that builds the shaders to SPIR-V (`-DHOG_SLANGC=` points at your own)
- **Linux:** OpenSSL development headers (GameNetworkingSockets' crypto; Windows uses BCrypt), and for the windowed apps X11 or Wayland headers — on Ubuntu: `libssl-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libegl1-mesa-dev`. Without a window system the apps still build headless; `-DHOG_BUILD_VIEWER=OFF -DHOG_BUILD_CLIENT=OFF` skips them entirely.
- **ccache** is picked up automatically when present

Nothing Vulkan needs installing to build: headers are pinned and functions are loaded at run time through volk. Running the client (or the render tests) takes a Vulkan driver; CI and headless test runs get by on llvmpipe.

Everything else is fetched and built in-tree, pinned: glm, xxHash, enkiTS, toml++, SQLite, flecs, Jolt Physics (double precision), stb, Abseil, protobuf, GameNetworkingSockets, Monocypher, SDL3, Dear ImGui, Vulkan-Headers, volk, VulkanMemoryAllocator, Catch2.

## Building

```sh
cmake --workflow --preset debug    # configure, build and run all tests
```

Or step by step:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

| Preset | Purpose |
| --- | --- |
| `debug` | Day-to-day development |
| `release` | Optimized with debug info (`RelWithDebInfo`) |
| `asan` | Address and undefined-behaviour sanitizers |
| `tsan` | Thread sanitizer |
| `ci`, `ci-asan`, `ci-tsan` | As above with warnings as errors |

Binaries land under `build/<preset>/`, e.g. `build/release/apps/client/hog-client`. Presets use `clang++`; pick another compiler with `-DCMAKE_CXX_COMPILER=g++` (or `cl` on Windows).

## Running

From the repository root:

- `build/debug/apps/server/hog-server` — reads `config/server.toml`, keeps the world under `var/`, saves on Ctrl-C.
- `build/debug/tools/viewer/hog-viewer` — watches the server's debug port and draws weather, plants and wildlife on a planet map.
- `build/debug/apps/client/hog-client` — builds the world from `config/planet.toml` and starts flying over the starting islands: WASD to fly, Space and C for up and down, hold the right mouse button to look, wheel for speed.

Each takes `--help`. See [CONTRIBUTING.md](CONTRIBUTING.md) for the full developer workflow, code rules and golden-hash discipline.

## Docs

- [Design and technical plan](docs/plan.md): vision, world, rendering, characters, simulation, networking, moderation, stack, licensing
- [Roadmap](docs/roadmap.md): phases 0–8, the gate that ends each one, and task lists

## License

[MIT](LICENSE).
