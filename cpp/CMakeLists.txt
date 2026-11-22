cmake_minimum_required(VERSION 3.16)
project(kvstore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_library(kvcore STATIC
  src/engines/array_engine.cpp
  src/engines/hash_engine.cpp
  src/engines/rbtree_engine.cpp
  src/engines/skiplist_engine.cpp
  src/engines/unordered_engine.cpp
  src/engines/factory.cpp
  src/core/buffer.cpp
  src/core/protocol.cpp
  src/core/store.cpp
  src/core/wal.cpp
  src/net/socket.cpp
  src/net/poller.cpp
  src/net/event_loop.cpp
  src/net/server.cpp
)

target_include_directories(kvcore PUBLIC include)
target_link_libraries(kvcore PUBLIC Threads::Threads)

if(WIN32)
  # Winsock; the reactor falls back to a WSAPoll backend on Windows.
  target_link_libraries(kvcore PUBLIC ws2_32)
endif()

add_executable(kvstore src/main.cpp)
target_link_libraries(kvstore PRIVATE kvcore)

add_executable(kv_bench bench/bench.cpp)
target_link_libraries(kv_bench PRIVATE kvcore)

enable_testing()
add_executable(kv_tests
  tests/test_main.cpp
  tests/test_engines.cpp
  tests/test_protocol.cpp
  tests/test_store.cpp
  tests/test_wal.cpp
  tests/test_net.cpp
)
target_link_libraries(kv_tests PRIVATE kvcore)
add_test(NAME kv_tests COMMAND kv_tests)
