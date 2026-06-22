#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "keylight/version.hpp"

TEST_CASE("sdk version is set") {
    CHECK(std::string(KEYLIGHT_SDK_VERSION) == "0.1.0");
}
