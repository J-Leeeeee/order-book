cmake_minimum_required(VERSION 3.25)

project(order_book
    VERSION 0.1.0
    DESCRIPTION "Deterministic limit order book and matching engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(order_book_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(order_book
    src/order_book.cpp
)

add_library(order_book::order_book ALIAS order_book)

target_include_directories(order_book
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(order_book PUBLIC cxx_std_23)
order_book_enable_warnings(order_book)

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

option(ORDER_BOOK_BUILD_BENCHMARKS "Build benchmark executables" OFF)
if(ORDER_BOOK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
