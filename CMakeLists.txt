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
# Demo — opt-in; requires the httplib transport (and thus OpenSSL).
#   cmake -B build-demo -DKEYLIGHT_BUILD_DEMO=ON \
#         -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
# Default cmake -B build is unaffected.
# ---------------------------------------------------------------------------
option(KEYLIGHT_BUILD_DEMO
  "Build the Keylight Notes console demo (requires KEYLIGHT_BUILD_HTTPLIB_TRANSPORT + OpenSSL)"
  OFF)

if(KEYLIGHT_BUILD_DEMO)
  # The demo always needs the httplib transport — force it on.
  if(NOT KEYLIGHT_BUILD_HTTPLIB_TRANSPORT)
    set(KEYLIGHT_BUILD_HTTPLIB_TRANSPORT ON CACHE BOOL "" FORCE)

    # Duplicate the httplib-transport setup (guard skipped above because the
    # option was OFF when evaluated; re-run here).
    if(NOT DEFINED OPENSSL_ROOT_DIR AND EXISTS "/opt/homebrew/opt/openssl@3")
      set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
    endif()

    find_package(OpenSSL REQUIRED)

    add_library(keylight_httplib_transport INTERFACE)
    target_link_libraries(keylight_httplib_transport INTERFACE
      keylight OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(keylight_httplib_transport INTERFACE
      CPPHTTPLIB_OPENSSL_SUPPORT
      KEYLIGHT_BUILD_HTTPLIB_TRANSPORT)
    target_include_directories(keylight_httplib_transport INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
  endif()

  add_subdirectory(demo/notes)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(KEYLIGHT_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
if(KEYLIGHT_BUILD_TESTS)
  enable_testing()

  # Default test suite — all tests EXCEPT test_live.cpp (no network, no OpenSSL)
  # and EXCEPT test_amalgamation.cpp (has its own main; built separately below).
  file(GLOB KL_TESTS tests/test_*.cpp)
  list(REMOVE_ITEM KL_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_live.cpp")
  list(REMOVE_ITEM KL_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_amalgamation.cpp")

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

  # ---------------------------------------------------------------------------
  # Single-header amalgamation test
  #
  # Regenerates keylight_single.hpp before compiling test_amalgamation.cpp so
  # the compiled binary always reflects the current headers.  The test binary
  # depends only on the generated single-header file — no split headers are
  # added to its include path.
  # ---------------------------------------------------------------------------
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(NOT Python3_FOUND)
    message(WARNING
      "Python3 not found — keylight_amalgamate target will not regenerate "
      "keylight_single.hpp automatically.  Run tools/amalgamate.py manually.")
  endif()

  set(AMALGAMATE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate.py")
  set(SINGLE_HEADER     "${CMAKE_CURRENT_SOURCE_DIR}/keylight_single.hpp")

  # Custom command: regenerate the single-header whenever any source header changes.
  file(GLOB_RECURSE KEYLIGHT_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/keylight/*.hpp")
  list(FILTER KEYLIGHT_HEADERS EXCLUDE REGEX "transport/httplib\\.hpp$")

  if(Python3_FOUND)
    add_custom_command(
      OUTPUT  "${SINGLE_HEADER}"
      COMMAND "${Python3_EXECUTABLE}" "${AMALGAMATE_SCRIPT}"
      DEPENDS ${AMALGAMATE_SCRIPT} ${KEYLIGHT_HEADERS}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Regenerating keylight_single.hpp"
      VERBATIM)
  else()
    # Python3 not available — use the pre-committed keylight_single.hpp as-is.
    add_custom_command(
      OUTPUT  "${SINGLE_HEADER}"
      COMMAND "${CMAKE_COMMAND}" -E echo
        "keylight_single.hpp already present (Python3 unavailable; using committed copy)"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Using pre-committed keylight_single.hpp (Python3 not found)"
      VERBATIM)
  endif()

  add_custom_target(keylight_amalgamate ALL DEPENDS "${SINGLE_HEADER}")

  # The standalone test includes keylight_single.hpp from the repo root.
  add_executable(test_amalgamation tests/test_amalgamation.cpp)
  add_dependencies(test_amalgamation keylight_amalgamate)
  # Only the repo root is on the include path — no split-header directories.
  target_include_directories(test_amalgamation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME test_amalgamation COMMAND test_amalgamation)
  set_tests_properties(test_amalgamation PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
