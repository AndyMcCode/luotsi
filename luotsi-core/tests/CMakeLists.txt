include(GoogleTest)

add_executable(luotsi_tests
    test_main.cpp
)

target_link_libraries(luotsi_tests PRIVATE
    luotsi_lib
    GTest::gtest_main
)

gtest_discover_tests(luotsi_tests)

add_test(NAME IntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_integration.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME JsonRpcIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_jsonrpc_test.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
