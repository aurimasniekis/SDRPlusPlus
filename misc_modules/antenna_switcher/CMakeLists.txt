cmake_minimum_required(VERSION 3.13)
project(antenna_switcher)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

# --- cpp-antenna-switcher-client (pinned release tarball) ---
include(FetchContent)

# Keep the dependency tree minimal: disable the library's (and esphome's) tests/CLI/docs
set(ANTENNA_SWITCHER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ANTENNA_SWITCHER_BUILD_CLI      OFF CACHE BOOL "" FORCE)
set(ANTENNA_SWITCHER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ANTENNA_SWITCHER_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(ESPHOME_API_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(ESPHOME_API_BUILD_CLI           OFF CACHE BOOL "" FORCE)
# Noise PSK support must stay ON (Options.noise_psk); libsodium is vendored.
set(ESPHOME_API_WITH_NOISE          ON  CACHE BOOL "" FORCE)

# Don't pull in zlib: protobuf only needs it for gzip streams (unused here), and
# a from-source protobuf build links it as @rpath/libz.1.dylib, which the macOS
# bundler skips (system lib) — leaving the plugin unable to load at runtime.
set(protobuf_WITH_ZLIB              OFF CACHE BOOL "" FORCE)

# Static deps (protobuf/abseil/sodium) get linked into our SHARED module → need PIC (Linux).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

FetchContent_Declare(antenna-switcher-client
    URL      https://github.com/aurimasniekis/cpp-antenna-switcher-client/archive/refs/tags/v0.2.0.tar.gz
    URL_HASH SHA256=01f8b7d684f6bf379e31363fbea7b3bbb6024fec35c2cddf24b740e332a2c5ba
)
FetchContent_MakeAvailable(antenna-switcher-client)

target_link_libraries(antenna_switcher PRIVATE antenna_switcher::client)