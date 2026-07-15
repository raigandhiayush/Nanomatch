cmake_minimum_required(VERSION 3.16)
project(NanoMatch CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Production HFT compilation configurations
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto -Wall -Wextra -pthread")

# Universal path lookup mapping
include_directories(src/core src/ingestion src/logging)

find_package(Threads REQUIRED)

add_executable(nanomatch
    src/core/main.cpp
    src/core/OrderBook.cpp
    src/ingestion/MmapParser.cpp
)
target_link_libraries(nanomatch PRIVATE Threads::Threads)
add_subdirectory(benchmarks)
add_subdirectory(tests)

add_executable(itch_replay
    src/core/itch_main.cpp
    src/ingestion/ItchParser.cpp
    src/core/OrderBook.cpp
)
target_link_libraries(itch_replay PRIVATE Threads::Threads)