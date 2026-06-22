cmake_minimum_required(VERSION 3.16)
project(keylight CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(keylight INTERFACE)
add_library(keylight::keylight ALIAS keylight)
target_include_directories(keylight INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

option(KEYLIGHT_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
if(KEYLIGHT_BUILD_TESTS)
  enable_testing()
  file(GLOB KL_TESTS tests/test_*.cpp)
  add_executable(keylight_tests ${KL_TESTS})
  target_link_libraries(keylight_tests PRIVATE keylight)
  target_include_directories(keylight_tests PRIVATE tests)
  add_test(NAME keylight_tests COMMAND keylight_tests)
  set_tests_properties(keylight_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
