# CMake generated Testfile for 
# Source directory: /Users/halloweed/Coding/Projects/keylight-cpp
# Build directory: /Users/halloweed/Coding/Projects/keylight-cpp/build-httplib
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(keylight_tests "/Users/halloweed/Coding/Projects/keylight-cpp/build-httplib/keylight_tests")
set_tests_properties(keylight_tests PROPERTIES  WORKING_DIRECTORY "/Users/halloweed/Coding/Projects/keylight-cpp" _BACKTRACE_TRIPLES "/Users/halloweed/Coding/Projects/keylight-cpp/CMakeLists.txt;129;add_test;/Users/halloweed/Coding/Projects/keylight-cpp/CMakeLists.txt;0;")
add_test(keylight_tests_httplib "/Users/halloweed/Coding/Projects/keylight-cpp/build-httplib/keylight_tests_httplib")
set_tests_properties(keylight_tests_httplib PROPERTIES  WORKING_DIRECTORY "/Users/halloweed/Coding/Projects/keylight-cpp" _BACKTRACE_TRIPLES "/Users/halloweed/Coding/Projects/keylight-cpp/CMakeLists.txt;139;add_test;/Users/halloweed/Coding/Projects/keylight-cpp/CMakeLists.txt;0;")
add_test(test_amalgamation "/Users/halloweed/Coding/Projects/keylight-cpp/build-httplib/test_amalgamation")
set_tests_properties(test_amalgamation PROPERTIES  WORKING_DIRECTORY "/Users/halloweed/Coding/Projects/keylight-cpp" _BACKTRACE_TRIPLES "/Users/halloweed/Coding/Projects/keylight-cpp/CMakeLists.txt;204;add_test;/Users/halloweed/Coding/Projects/keylight-cpp/CMakeLists.txt;0;")
subdirs("demo/notes")
