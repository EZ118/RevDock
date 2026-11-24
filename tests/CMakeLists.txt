add_executable(revdock_tests
    test_main.cpp
    test_layout.cpp
    test_launchpad.cpp
)
target_link_libraries(revdock_tests PRIVATE revdock_core doctest)
target_include_directories(revdock_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME revdock_tests COMMAND revdock_tests)
