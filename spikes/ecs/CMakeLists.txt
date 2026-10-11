# EnTT against flecs on the server's access patterns. Not part of the normal build:
#   cmake --preset release -DHOG_BUILD_SPIKES=ON && cmake --build --preset release --target spikes
CPMAddPackage(
    NAME EnTT
    GITHUB_REPOSITORY skypjack/entt
    VERSION 4.0.0
    DOWNLOAD_ONLY YES)

CPMAddPackage(
    NAME flecs
    GITHUB_REPOSITORY SanderMertens/flecs
    VERSION 4.1.6
    DOWNLOAD_ONLY YES)

add_library(entt INTERFACE)
target_include_directories(entt SYSTEM INTERFACE "${EnTT_SOURCE_DIR}/src")

add_library(flecs STATIC "${flecs_SOURCE_DIR}/distr/flecs.c")
target_include_directories(flecs SYSTEM PUBLIC "${flecs_SOURCE_DIR}/distr")
target_compile_definitions(flecs PUBLIC flecs_STATIC)

foreach(library entt flecs)
    add_executable(ecs_${library}_bench ${library}_bench.cpp)
    target_link_libraries(ecs_${library}_bench PRIVATE ${library} hog::jobs Catch2::Catch2WithMain)
    hog_target_defaults(ecs_${library}_bench)
endforeach()

add_custom_target(spikes DEPENDS ecs_entt_bench ecs_flecs_bench)
