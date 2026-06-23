# ---------------------------------------------------------------------------
# JUCE adapter compile + smoke test (CI only)
#
# Fetches a real JUCE (version pinned via -DKEYLIGHT_JUCE_TAG) and builds a
# tiny console app that includes integrations/juce/KeylightJuce.h. This proves
# the JUCE adapter compiles and links against an actual JUCE toolchain on every
# push, replacing the manual "compile it in a real plugin" verification step.
#
# This project is intentionally standalone (not part of the SDK's root
# CMakeLists) so the core SDK build stays JUCE-free and dependency-free.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.22)
project(keylight_juce_citest VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE version to test against (override on the CLI, e.g. -DKEYLIGHT_JUCE_TAG=7.0.12).
# The canonical set of versions actually tested lives in .github/workflows/juce.yml;
# keep this default equal to that matrix's current-generation pin so a bare local
# `cmake -B build integrations/juce/citest` reproduces a real CI cell.
set(KEYLIGHT_JUCE_TAG "8.0.6" CACHE STRING "JUCE git tag to fetch and build against")

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        ${KEYLIGHT_JUCE_TAG}
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_console_app(keylight_juce_citest)

target_sources(keylight_juce_citest PRIVATE main.cpp)

# KeylightJuce.h lives one dir up; the SDK headers live at <repo>/include.
target_include_directories(keylight_juce_citest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..                 # integrations/juce  (KeylightJuce.h)
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)   # repo include/      (keylight/*.hpp)

# Headless console app: no GUI, no browser, no curl needed (the adapter's
# transport uses juce::URL, which we never invoke in this offline smoke test).
target_compile_definitions(keylight_juce_citest PRIVATE
  JUCE_STANDALONE_APPLICATION=1
  JUCE_USE_CURL=0
  JUCE_WEB_BROWSER=0)

target_link_libraries(keylight_juce_citest PRIVATE
  juce::juce_core
  juce::juce_events
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)

# Register the smoke test with CTest so CI can run it generator-agnostically
# (`ctest --test-dir build -C Release`) without hardcoding JUCE's artefact path.
enable_testing()
add_test(NAME keylight_juce_smoke COMMAND keylight_juce_citest)
