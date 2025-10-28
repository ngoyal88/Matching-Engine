cmake_minimum_required(VERSION 3.10)
project(matching_engine_cpp)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


include_directories(${CMAKE_SOURCE_DIR}/vendor)
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/vendor/uWebSockets/src)

# Ensure Windows 10+ API visible for httplib on MinGW/Windows
add_definitions(-D_WIN32_WINNT=0x0A00)


add_executable(matching_engine
src/main.cpp
src/order_store.cpp
src/server.cpp
src/order_book.cpp 
src/wal.cpp
src/wal_integration.cpp
src/ws_server.cpp
)

if (WIN32)
	target_link_libraries(matching_engine PRIVATE ws2_32)
	target_link_libraries(ws_server PRIVATE ws2_32)
    
endif()


enable_testing()
add_executable(
    tests 
    tests/test_order.cpp 
    src/order_store.cpp 
    tests/test_order_book.cpp
    src/order_book.cpp 
    src/wal.cpp 
    src/wal_integration.cpp 
)
add_test(NAME order_tests COMMAND tests)