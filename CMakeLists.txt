cmake_minimum_required(VERSION 3.14)
project(nanoserve VERSION 0.1.0 LANGUAGES CXX)

add_library(nanoserve INTERFACE)
target_include_directories(nanoserve INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(nanoserve INTERFACE cxx_std_17)
add_library(nanoserve::nanoserve ALIAS nanoserve)

option(NANOSERVE_BUILD_EXAMPLES "Build examples" ON)
option(NANOSERVE_BUILD_TESTS "Build tests" ON)

if(NANOSERVE_BUILD_EXAMPLES)
  add_executable(chat examples/chat.cpp)
  target_link_libraries(chat PRIVATE nanoserve)
endif()

if(NANOSERVE_BUILD_TESTS)
  enable_testing()
  add_executable(test_server tests/test_server.cpp)
  target_link_libraries(test_server PRIVATE nanoserve)
  target_compile_options(test_server PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME server COMMAND test_server)
endif()
