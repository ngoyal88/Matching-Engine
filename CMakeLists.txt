cmake_minimum_required(VERSION 3.10)
project(matching_engine_cpp)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add C++17 filesystem library
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

include_directories(${CMAKE_SOURCE_DIR}/vendor)
include_directories(${CMAKE_SOURCE_DIR}/include)

# Ensure Windows 10+ API visible for httplib on MinGW/Windows
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

# Main executable
add_executable(matching_engine
    src/main.cpp
    src/order_store.cpp
    src/server.cpp
    src/order_book.cpp 
    src/wal.cpp
    src/wal_integration.cpp
    src/ws_server.cpp
    src/stop_order_manager.cpp
    src/global_state.cpp
    src/broadcast_queue.cpp
)

# Link libraries
if(WIN32)
    target_link_libraries(matching_engine PRIVATE ws2_32 advapi32)
else()
    # For Linux/Unix - link pthread and filesystem
    target_link_libraries(matching_engine PRIVATE pthread)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        target_link_libraries(matching_engine PRIVATE stdc++fs)
    endif()
endif()

# Tests
enable_testing()
add_executable(tests 
    tests/test_order.cpp 
    src/order_store.cpp 
    tests/test_order_book.cpp
    src/order_book.cpp 
    src/wal.cpp 
    src/wal_integration.cpp 
    src/stop_order_manager.cpp
    src/global_state.cpp
    src/broadcast_queue.cpp
    src/ws_server.cpp
)

if(WIN32)
    target_link_libraries(tests PRIVATE ws2_32 advapi32)
else()
    target_link_libraries(tests PRIVATE pthread)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        target_link_libraries(tests PRIVATE stdc++fs)
    endif()
endif()

add_test(NAME order_tests COMMAND tests)

# Benchmark tool
add_executable(benchmark
    tests/benchmark.cpp
)

if(WIN32)
    target_link_libraries(benchmark PRIVATE ws2_32)
else()
    target_link_libraries(benchmark PRIVATE pthread)
endif()

# Print configuration
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")