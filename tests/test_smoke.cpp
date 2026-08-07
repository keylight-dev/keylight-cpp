#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "keylight/version.hpp"

TEST_CASE("sdk version is set") {
    // Shape, not a literal. Pinning the exact version here meant every release
    // bump failed its own release build until someone edited this line — which
    // is a chore disguised as a guard, and it does not catch what matters. The
    // real drift risk is version.hpp disagreeing with vcpkg.json, and that is
    // checked by the release workflow, not here.
    const std::string v = KEYLIGHT_SDK_VERSION;
    CHECK(v.find('.') != std::string::npos);
    CHECK(v.size() >= 5);
    CHECK(v.find_first_not_of("0123456789.") == std::string::npos);
}
