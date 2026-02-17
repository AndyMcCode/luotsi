cmake_minimum_required(VERSION 3.14)

# Prefer GCC 13 if available
if(NOT CMAKE_CXX_COMPILER)
    find_program(GCC_13 "g++-13")
    if(GCC_13)
        set(CMAKE_CXX_COMPILER ${GCC_13})
    endif()
endif()

project(luotsi-core VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

include(FetchContent)

# 1. Spdlog (Logging)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.12.0
)
FetchContent_MakeAvailable(spdlog)

# 2. nlohmann_json (JSON)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.2
)
FetchContent_MakeAvailable(json)

# 3. yaml-cpp (Configuration)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
)
# Suppress yaml-cpp tests/tools to speed up build
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# 4. Asio (Async I/O)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-28-0
)
FetchContent_MakeAvailable(asio)

# 5. Core Libraries
add_library(luotsi_lib
    src/core/runtime.cpp
    src/core/config.cpp
    src/core/observability.cpp
    src/adapters/stdio_adapter.cpp
    src/adapters/jsonrpc_tcp_adapter.cpp
)

target_include_directories(luotsi_lib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${asio_SOURCE_DIR}/asio/include
)

find_package(Threads REQUIRED)

target_link_libraries(luotsi_lib PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    yaml-cpp
    Threads::Threads
)
target_compile_definitions(luotsi_lib PUBLIC ASIO_STANDALONE)

# 6. Executable
add_executable(luotsi src/core/main.cpp)
target_link_libraries(luotsi PRIVATE luotsi_lib)

# 7. Testing
enable_testing()
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(tests)
