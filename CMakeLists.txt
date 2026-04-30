cmake_minimum_required(VERSION 3.15)
project(ExchangeEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Main executable
add_executable(engine src/main.cpp)
target_include_directories(engine PRIVATE include)