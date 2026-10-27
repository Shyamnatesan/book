cmake_minimum_required(VERSION 3.16)
project(book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.9.1
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

add_library(book INTERFACE)
target_include_directories(book INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(book_demo src/demo.cpp)
target_link_libraries(book_demo PRIVATE book)

add_executable(book_test tests/book_test.cpp)
target_link_libraries(book_test PRIVATE book)

add_executable(book_bench bench/book_bench.cpp)
target_link_libraries(book_bench PRIVATE book benchmark::benchmark)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  foreach(target book_demo book_test book_bench)
    target_compile_options(${target} PRIVATE -O3 -march=native)
  endforeach()
endif()

enable_testing()
add_test(NAME book_test COMMAND book_test)
