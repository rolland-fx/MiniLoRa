cmake_minimum_required(VERSION 3.16)
project(minilora_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(minilora_tests
    test_main.cpp
    test_payload.cpp
    test_config.cpp
    test_diag.cpp
    ../main/domain/Payload.cpp
    ../main/domain/Config.cpp
    ../main/domain/DiagPayload.cpp
)

target_include_directories(minilora_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# Domain code must never depend on ESP-IDF — this build enforces it.
target_compile_options(minilora_tests PRIVATE -Wall -Wextra -Werror)
