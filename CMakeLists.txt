cmake_minimum_required(VERSION 3.16)
project(SeqlockMarketData CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

option(SEQLOCK_BUILD_BENCH "Build the latency/throughput benchmark" OFF)

# Header-only library: the seqlock and (eventually) market_data live in src/.
add_library(seqlock INTERFACE)
target_include_directories(seqlock INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(seqlock INTERFACE Threads::Threads)

enable_testing()

# --- Demo ----------------------------------------------------------------
add_executable(seqlock_demo src/main.cpp)
target_link_libraries(seqlock_demo PRIVATE seqlock)
target_compile_options(seqlock_demo PRIVATE -Wall -Wextra)

# --- Tests ---------------------------------------------------------------
add_executable(seqlock_test tests/seqlock_test.cpp)
target_link_libraries(seqlock_test PRIVATE seqlock)
target_compile_options(seqlock_test PRIVATE -Wall -Wextra)

add_test(NAME seqlock_test COMMAND seqlock_test)
# Concurrent test runs ~3s; give it generous headroom on a loaded CI box.
set_tests_properties(seqlock_test PROPERTIES TIMEOUT 60)

# --- Benchmark (optional) ------------------------------------------------
if(SEQLOCK_BUILD_BENCH)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/seqlock_bench.cpp)
        add_executable(seqlock_bench bench/seqlock_bench.cpp)
        target_link_libraries(seqlock_bench PRIVATE seqlock)
        target_compile_options(seqlock_bench PRIVATE -Wall -Wextra)
    else()
        message(WARNING "SEQLOCK_BUILD_BENCH=ON but bench/seqlock_bench.cpp not found; skipping.")
    endif()
endif()
