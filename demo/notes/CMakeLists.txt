cmake_minimum_required(VERSION 3.16)

# This file is included only when KEYLIGHT_BUILD_DEMO=ON (set in the root
# CMakeLists.txt).  keylight_httplib_transport — which carries OpenSSL linkage
# and CPPHTTPLIB_OPENSSL_SUPPORT — must already be defined by the time we get here.

add_executable(notes main.cpp)

target_link_libraries(notes PRIVATE keylight_httplib_transport)

# The demo executable lands in the same directory that cmake --build produces,
# making it easy to run: ./build-demo/demo/notes/notes <subcommand>
set_target_properties(notes PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/demo/notes")
