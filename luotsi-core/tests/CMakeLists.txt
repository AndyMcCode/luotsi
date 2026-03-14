include(GoogleTest)

add_executable(luotsi_tests
    test_main.cpp
)

target_link_libraries(luotsi_tests PRIVATE
    luotsi_lib
    GTest::gtest_main
)

target_compile_definitions(luotsi_tests PRIVATE TEST_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/test_config.yaml")

gtest_discover_tests(luotsi_tests)

add_test(NAME IntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_integration.sh $<TARGET_FILE:luotsi> ${CMAKE_CURRENT_SOURCE_DIR}/integration_config.yaml
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(NAME JsonRpcIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_jsonrpc_test.sh $<TARGET_FILE:luotsi> ${CMAKE_CURRENT_SOURCE_DIR}/jsonrpc_config.yaml
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
