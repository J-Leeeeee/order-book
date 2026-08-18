add_executable(order_book_benchmark
    matching_benchmark.cpp
)

target_link_libraries(order_book_benchmark PRIVATE order_book::order_book)
order_book_enable_warnings(order_book_benchmark)
