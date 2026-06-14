add_executable(engine_tests OrderBookTests.cpp)

target_link_libraries(engine_tests PRIVATE 
    Threads::Threads
)

target_sources(engine_tests PRIVATE 
    ../src/core/OrderBook.cpp
)