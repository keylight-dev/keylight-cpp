# Keylight JUCE starter plugin
#
# A complete, buildable VST3/AU plugin with Keylight licensing and a demo mode
# already wired in. Clone it, put your dashboard values in Source/KeylightConfig.h,
# and build.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# JUCE is fetched automatically. Point KEYLIGHT_SDK_DIR at a checkout of
# keylight-cpp if it is not sitting next to this directory.

cmake_minimum_required(VERSION 3.22)

project(KeylightStarter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.6
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# Keylight SDK headers
#
# The SDK is header-only at this boundary — there is nothing to link. By
# default we look for the repo three levels up (this directory lives at
# integrations/juce/starter inside keylight-cpp). Override when vendoring:
#
#   cmake -B build -DKEYLIGHT_SDK_DIR=/path/to/keylight-cpp
# ---------------------------------------------------------------------------
set(KEYLIGHT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." CACHE PATH
    "Path to a keylight-cpp checkout")

if(NOT EXISTS "${KEYLIGHT_SDK_DIR}/include/keylight/config.hpp")
    message(FATAL_ERROR
        "Keylight SDK headers not found under ${KEYLIGHT_SDK_DIR}.\n"
        "Pass -DKEYLIGHT_SDK_DIR=/path/to/keylight-cpp, or drop the "
        "amalgamated keylight_single.hpp into Source/ and include that instead.")
endif()

juce_add_plugin(KeylightStarter
    COMPANY_NAME              "Your Company"
    BUNDLE_ID                 "com.yourcompany.keylightstarter"
    PLUGIN_MANUFACTURER_CODE  Ycmp
    PLUGIN_CODE               Klst
    FORMATS                   VST3 AU Standalone
    PRODUCT_NAME              "Keylight Starter"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    COPY_PLUGIN_AFTER_BUILD   TRUE)

target_sources(KeylightStarter PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(KeylightStarter PRIVATE
    "${KEYLIGHT_SDK_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../")   # for KeylightJuce.h

target_compile_definitions(KeylightStarter PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(KeylightStarter PRIVATE
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_events
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
