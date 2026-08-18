add_executable(order_book_tests
    order_book_tests.cpp
)

target_link_libraries(order_book_tests PRIVATE order_book::order_book)
order_book_enable_warnings(order_book_tests)

add_test(NAME order_book_tests COMMAND order_book_tests)
