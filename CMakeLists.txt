cmake_minimum_required(VERSION 3.16)
project(keylight CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Core library — zero external dependencies
# ---------------------------------------------------------------------------
add_library(keylight INTERFACE)
add_library(keylight::keylight ALIAS keylight)
target_include_directories(keylight INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

# ---------------------------------------------------------------------------
# Opt-in: cpp-httplib + OpenSSL transport
#   cmake -DKEYLIGHT_BUILD_HTTPLIB_TRANSPORT=ON \
#         -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 …
# When OFF the core build and all tests have NO OpenSSL dependency.
# ---------------------------------------------------------------------------
option(KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
  "Build the cpp-httplib/OpenSSL Transport implementation (requires OpenSSL)"
  OFF)

if(KEYLIGHT_BUILD_HTTPLIB_TRANSPORT)
  # Hint for Homebrew OpenSSL on macOS; harmless on Linux.
  if(NOT DEFINED OPENSSL_ROOT_DIR AND EXISTS "/opt/homebrew/opt/openssl@3")
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
  endif()

  find_package(OpenSSL REQUIRED)

  # Interface library that carries the HTTPS flag + OpenSSL linkage.
  # Consumers link keylight_httplib_transport to get everything they need.
  add_library(keylight_httplib_transport INTERFACE)
  target_link_libraries(keylight_httplib_transport INTERFACE
    keylight OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(keylight_httplib_transport INTERFACE
    CPPHTTPLIB_OPENSSL_SUPPORT
    KEYLIGHT_BUILD_HTTPLIB_TRANSPORT)
  target_include_directories(keylight_httplib_transport INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(KEYLIGHT_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
if(KEYLIGHT_BUILD_TESTS)
  enable_testing()

  # Default test suite — all tests EXCEPT test_live.cpp (no network, no OpenSSL).
  file(GLOB KL_TESTS tests/test_*.cpp)
  list(REMOVE_ITEM KL_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_live.cpp")

  add_executable(keylight_tests ${KL_TESTS})
  target_link_libraries(keylight_tests PRIVATE keylight)
  target_include_directories(keylight_tests PRIVATE tests)
  add_test(NAME keylight_tests COMMAND keylight_tests)
  set_tests_properties(keylight_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  if(KEYLIGHT_BUILD_HTTPLIB_TRANSPORT)
    # Httplib test suite: all default tests + test_live.cpp (needs OpenSSL + network).
    # Separate executable so default ctest stays zero-dep.
    add_executable(keylight_tests_httplib ${KL_TESTS} tests/test_live.cpp)
    target_link_libraries(keylight_tests_httplib PRIVATE keylight_httplib_transport)
    target_include_directories(keylight_tests_httplib PRIVATE tests)
    add_test(NAME keylight_tests_httplib COMMAND keylight_tests_httplib)
    set_tests_properties(keylight_tests_httplib PROPERTIES
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
endif()
