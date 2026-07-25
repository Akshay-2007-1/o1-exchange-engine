cmake_minimum_required(VERSION 3.15)
project(ExchangeEngine)

# ── Homebrew Paths (For libsodium and other local packages) ──
if(APPLE)
    include_directories(/opt/homebrew/include /usr/local/include)
    link_directories(/opt/homebrew/lib /usr/local/lib)
endif()
# ─────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Default to an optimized build if the caller didn't ask for something else -
# a plain `cmake ..` would otherwise compile at -O0, which badly misrepresents
# the CURRENT-vs-LEGACY benchmark story this project is built around.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

# -O3 (CMake's own Release default for GCC/Clang) plus -march=native: this
# project always builds and runs on the same machine (local dev, the Docker
# image built on the Azure VM itself, and CI's own runner), so targeting the
# exact CPU is safe and lets __builtin_clzll/ctzll in the bitmap scan lower to
# native lzcnt/tzcnt instead of the portable fallback. Drop -march=native if
# that build-on-target assumption ever changes (e.g. shipping a prebuilt
# binary to a different machine than it was compiled on).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native)
endif()

option(FETCH_GTEST "Fetch GoogleTest from GitHub" ON)
option(FETCH_JSON "Fetch nlohmann/json from GitHub" ON)
option(FETCH_BENCHMARK "Fetch Google Benchmark from GitHub" ON)

# ── Google Test ───────────────────────────
if(FETCH_GTEST)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
      DOWNLOAD_EXTRACT_TIMESTAMP true
    )
    FetchContent_MakeAvailable(googletest)
else()
    find_package(GTest REQUIRED)
endif()

# ── nlohmann/json ─────────────────────────
if(FETCH_JSON)
    include(FetchContent)
    FetchContent_Declare(
      nlohmann_json
      URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
      DOWNLOAD_EXTRACT_TIMESTAMP true
    )
    FetchContent_MakeAvailable(nlohmann_json)
else()
    find_package(nlohmann_json REQUIRED)
endif()

# ── Google Benchmark ──────────────────────
if(FETCH_BENCHMARK)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googlebenchmark
      URL https://github.com/google/benchmark/archive/refs/tags/v1.8.4.zip
      DOWNLOAD_EXTRACT_TIMESTAMP true
    )
    FetchContent_MakeAvailable(googlebenchmark)
else()
    find_package(benchmark REQUIRED)
endif()

# ── Boost ─────────────────────────────────
if(APPLE)
    # Appending instead of overwriting just in case
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/boost")
    find_package(Boost 1.74 REQUIRED)
    set(BOOST_LINK_LIB Boost::headers)
else()
    find_package(Boost 1.74 REQUIRED COMPONENTS system)
    set(BOOST_LINK_LIB Boost::system)
endif()

# ── Main executable ────────────────────────
add_executable(engine src/main.cpp)
target_include_directories(engine PRIVATE include)
find_package(SQLite3 REQUIRED)
target_link_libraries(engine ${BOOST_LINK_LIB} pthread nlohmann_json::nlohmann_json sqlite3 sodium)

# ── Test executable ────────────────────────
add_executable(tests
    tests/test_orderbook.cpp
    tests/test_orderbook_legacy_parity.cpp
    tests/test_database.cpp
    tests/test_market_maker_judge.cpp
    tests/test_game_manager.cpp
)
target_include_directories(tests PRIVATE include)
target_link_libraries(tests GTest::gtest_main nlohmann_json::nlohmann_json sqlite3 sodium)

enable_testing()
add_test(NAME EngineTests COMMAND tests)

# ── Benchmark executable ───────────────────
add_executable(benchmarks benchmark/benchmark_orderbook.cpp)
target_include_directories(benchmarks PRIVATE include)
target_link_libraries(benchmarks benchmark::benchmark)