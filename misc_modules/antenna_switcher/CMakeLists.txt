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
    URL      https://github.com/aurimasniekis/cpp-antenna-switcher-client/archive/refs/tags/v0.3.0.tar.gz
    URL_HASH SHA256=44d8f05d091bedf79ccd6f1d643f943749963902fb3ade4c98ce821977fd463b
)
FetchContent_MakeAvailable(antenna-switcher-client)

target_link_libraries(antenna_switcher PRIVATE antenna_switcher::client)