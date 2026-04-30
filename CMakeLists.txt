cmake_minimum_required(VERSION 3.15)
project(ExchangeEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# ── Main executable ──────────────────────
add_executable(engine src/main.cpp)
target_include_directories(engine PRIVATE include)

# ── Google Test (auto-downloaded) ────────
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# ── Test executable ───────────────────────
add_executable(tests tests/test_orderbook.cpp)
target_include_directories(tests PRIVATE include)
target_link_libraries(tests GTest::gtest_main)

enable_testing()
add_test(NAME OrderBookTests COMMAND tests)