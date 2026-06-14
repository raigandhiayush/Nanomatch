find_package(benchmark REQUIRED)

add_executable(engine_benchmark EngineBenchmark.cpp)
target_link_libraries(engine_benchmark PRIVATE 
    benchmark::benchmark 
    Threads::Threads
)

# Pull matching core object files
target_sources(engine_benchmark PRIVATE 
    ../src/core/OrderBook.cpp
)