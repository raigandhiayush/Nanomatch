find_package(benchmark QUIET)
if(benchmark_FOUND)
    add_executable(engine_benchmark EngineBenchmark.cpp)
    target_sources(engine_benchmark PRIVATE
        ../src/core/OrderBook.cpp
    )
    target_include_directories(engine_benchmark PRIVATE
        ../src/core
        ../src/ingestion
        ../src/logging
    )
    target_link_libraries(engine_benchmark PRIVATE
        benchmark::benchmark
        Threads::Threads
    )
endif()

    add_executable(stlbaseline_benchmark Baselinecomparsion.cpp)
    target_sources(stlbaseline_benchmark PRIVATE
        ../src/core/OrderBook.cpp
    )
    target_include_directories(stlbaseline_benchmark PRIVATE
        .
        ../src/core
        ../src/ingestion
        ../src/logging
    )
    target_link_libraries(stlbaseline_benchmark PRIVATE
        benchmark::benchmark
        Threads::Threads
    )