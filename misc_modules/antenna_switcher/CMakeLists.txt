cmake_minimum_required(VERSION 3.13)
project(antenna_switcher)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

# --- cpp-antenna-switcher-client (pinned release tarball) ---
include(FetchContent)

# Keep the dependency tree minimal: disable the library's (and esphome's) tests/CLI/docs.
# As of v0.3.0 the client + esphome-api are self-contained (only header-only asio
# is fetched) — no protobuf / abseil / libsodium — so no extra dep hacks are needed.
set(ANTENNA_SWITCHER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ANTENNA_SWITCHER_BUILD_CLI      OFF CACHE BOOL "" FORCE)
set(ANTENNA_SWITCHER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ANTENNA_SWITCHER_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(ESPHOME_API_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(ESPHOME_API_BUILD_CLI           OFF CACHE BOOL "" FORCE)

# The static client (+ esphome) gets linked into our SHARED module → needs PIC (Linux).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

FetchContent_Declare(antenna-switcher-client
    URL      https://github.com/aurimasniekis/cpp-antenna-switcher-client/archive/refs/tags/v0.4.0.tar.gz
    URL_HASH SHA256=dbf6c0f63d40be301b700362a58599a1911d054beac6b8ce47e44dcfbb75857c
)
FetchContent_MakeAvailable(antenna-switcher-client)

target_link_libraries(antenna_switcher PRIVATE antenna_switcher::client)