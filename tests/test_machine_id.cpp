// tests/test_machine_id.cpp — hardware identity, the cross-SDK machine_hash,
// and the UUIDv4 used for the anonymous free-tier instance id.

#include "doctest.h"
#include "keylight/machine_id.hpp"

#include <set>
#include <string>

using namespace keylight;

TEST_CASE("machine_hash matches the cross-SDK canonical vector") {
    // Pinned byte-for-byte by keylight-rust (keylight/src/machine.rs) and Swift.
    CHECK(detail::machine_hash("testco", "testapp", "hardware-1")
          == "8e8871112f28cabda180ada131d0b4f4f07c72fb47c5d884edbe32812885b22a");
}

TEST_CASE("machine_hash is 64 lowercase hex chars") {
    const std::string h = detail::machine_hash("a", "b", "c");
    REQUIRE(h.size() == 64);
    for (char c : h) {
        CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
}

TEST_CASE("machine_hash varies with every input component") {
    const std::string base = detail::machine_hash("t", "p", "id");
    CHECK(detail::machine_hash("T", "p",  "id")  != base);
    CHECK(detail::machine_hash("t", "P",  "id")  != base);
    CHECK(detail::machine_hash("t", "p",  "id2") != base);
}

TEST_CASE("uuid_v4 has the right shape and version/variant nibbles") {
    const std::string u = detail::uuid_v4();
    REQUIRE(u.size() == 36);
    CHECK(u[8]  == '-');
    CHECK(u[13] == '-');
    CHECK(u[18] == '-');
    CHECK(u[23] == '-');
    CHECK(u[14] == '4');                        // version 4
    CHECK((u[19] == '8' || u[19] == '9' ||
           u[19] == 'a' || u[19] == 'b'));      // RFC 4122 variant
}

TEST_CASE("uuid_v4 does not repeat") {
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) seen.insert(detail::uuid_v4());
    CHECK(seen.size() == 200);
}

TEST_CASE("read_hardware_id returns either nothing or a non-empty trimmed id") {
    // Platform-dependent: CI containers often have no machine-id at all, so the
    // contract under test is "never an empty or whitespace-padded string",
    // never "an id exists".
    auto id = detail::read_hardware_id();
    if (id.has_value()) {
        CHECK_FALSE(id->empty());
        CHECK(id->find('\n') == std::string::npos);
        CHECK(id->front() != ' ');
        CHECK(id->back()  != ' ');
    }
}
