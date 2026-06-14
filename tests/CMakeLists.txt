add_executable(engine_tests OrderBookTests.cpp)

target_include_directories(engine_tests PRIVATE
    ../src/core
    ../src/ingestion
    ../src/logging
)

target_sources(engine_tests PRIVATE
    ../src/core/OrderBook.cpp
    ../src/ingestion/MmapParser.cpp
)

target_link_libraries(engine_tests PRIVATE
    Threads::Threads
)