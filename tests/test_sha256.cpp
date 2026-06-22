#include "doctest.h"
#include "keylight/sha256.hpp"
using keylight::sha256_hex;

TEST_CASE("sha256 known vectors") {
    CHECK(sha256_hex("") ==
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") ==
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
