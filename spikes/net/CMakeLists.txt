# Transports for the one encrypted UDP connection, run through the same impaired link. Not part
# of the normal build:
#   cmake --preset release -DHOG_BUILD_SPIKES=ON && cmake --build --preset release --target spikes
#   spikes/net/run.sh
function(net_spike name)
    add_executable(net_${name} ${name}.cpp)
    target_link_libraries(net_${name} PRIVATE ${ARGN})
    hog_target_defaults(net_${name})
    add_dependencies(spikes net_${name})
endfunction()

add_custom_target(spikes)
net_spike(baseline)

# Needs protobuf (with Abseil) and OpenSSL from the system; neither is built here.
CPMAddPackage(
    NAME GameNetworkingSockets
    URL https://github.com/ValveSoftware/GameNetworkingSockets/archive/refs/tags/v1.6.1.tar.gz
    OPTIONS "BUILD_STATIC_LIB ON" "BUILD_SHARED_LIB OFF" "ENABLE_ICE OFF" "USE_CRYPTO OpenSSL")
net_spike(gns GameNetworkingSockets::static)

# OpenSSL 3.5+ from the system here; elsewhere msquic builds it from a submodule, or uses
# Schannel on Windows.
CPMAddPackage(
    NAME msquic
    GITHUB_REPOSITORY microsoft/msquic
    GIT_TAG v2.6.3
    GIT_SHALLOW YES
    GIT_SUBMODULES "submodules/clog"
    OPTIONS "QUIC_TLS_LIB openssl" "QUIC_USE_EXTERNAL_OPENSSL ON"
            "QUIC_EMBED_GIT_HASH OFF" "QUIC_STATIC_LINK_CRT OFF" "QUIC_STATIC_LINK_PARTIAL_CRT OFF")
net_spike(msquic msquic::msquic)
target_include_directories(net_msquic SYSTEM PRIVATE "${msquic_SOURCE_DIR}/src/inc")

CPMAddPackage(
    NAME yojimbo
    URL https://github.com/mas-bandwidth/yojimbo/archive/refs/tags/v1.13.5.tar.gz)
net_spike(yojimbo yojimbo)
target_include_directories(net_yojimbo SYSTEM PRIVATE "${yojimbo_SOURCE_DIR}/netcode")
