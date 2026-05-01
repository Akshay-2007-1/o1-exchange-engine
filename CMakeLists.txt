cmake_minimum_required(VERSION 3.15)
project(ExchangeEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

option(FETCH_GTEST "Fetch GoogleTest from GitHub" ON)
option(FETCH_JSON "Fetch nlohmann/json from GitHub" ON)

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

# ── Boost ─────────────────────────────────
if(APPLE)
    set(CMAKE_PREFIX_PATH /opt/homebrew/opt/boost)
    find_package(Boost 1.74 REQUIRED)
    set(BOOST_LINK_LIB Boost::headers)
else()
    find_package(Boost 1.74 REQUIRED COMPONENTS system)
    set(BOOST_LINK_LIB Boost::system)
endif()

# ── Main executable ────────────────────────
add_executable(engine src/main.cpp src/server.cpp)
target_include_directories(engine PRIVATE include)
target_link_libraries(engine ${BOOST_LINK_LIB} pthread nlohmann_json::nlohmann_json)

# ── Test executable ────────────────────────
add_executable(tests tests/test_orderbook.cpp)
target_include_directories(tests PRIVATE include)
target_link_libraries(tests GTest::gtest_main)

enable_testing()
add_test(NAME OrderBookTests COMMAND tests)