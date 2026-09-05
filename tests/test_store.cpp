#include "doctest.h"
#include "keylight/store.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace keylight;

// Helper: a unique temp path for each test run
static fs::path tmp_lease_path(const std::string& name) {
    return fs::temp_directory_path() / ("keylight_test_" + name + ".lease");
}

// Cleanup guard
struct TempFile {
    fs::path path;
    explicit TempFile(fs::path p) : path(std::move(p)) {
        // remove any leftover from a previous crashed run
        fs::remove(path);
    }
    ~TempFile() { fs::remove(path); }
};

TEST_CASE("FileStore: save then load round-trips exact bytes") {
    TempFile guard(tmp_lease_path("roundtrip"));
    FileStore store(guard.path.string());

    const std::string data = R"({"hello":"world","num":42})";
    auto sv = store.save(data);
    REQUIRE(sv.is_ok());

    auto lv = store.load();
    REQUIRE(lv.is_ok());
    CHECK(lv.value() == data);
}

TEST_CASE("FileStore: clear then load returns empty-string ok") {
    TempFile guard(tmp_lease_path("clear"));
    FileStore store(guard.path.string());

    REQUIRE(store.save("some content").is_ok());

    auto cv = store.clear();
    REQUIRE(cv.is_ok());

    auto lv = store.load();
    REQUIRE(lv.is_ok());
    CHECK(lv.value().empty());
}

TEST_CASE("FileStore: load on never-created path returns empty-string ok") {
    TempFile guard(tmp_lease_path("missing"));
    // Do not call save — file never exists
    FileStore store(guard.path.string());

    auto lv = store.load();
    REQUIRE(lv.is_ok());
    CHECK(lv.value().empty());
}

TEST_CASE("FileStore: save creates parent directories") {
    auto base = fs::temp_directory_path() / "keylight_test_parent_dir_creation";
    fs::remove_all(base);  // ensure clean state

    auto lease_path = base / "subdir" / "nested.lease";
    FileStore store(lease_path.string());

    const std::string data = "nested-content";
    auto sv = store.save(data);
    REQUIRE(sv.is_ok());

    auto lv = store.load();
    REQUIRE(lv.is_ok());
    CHECK(lv.value() == data);

    fs::remove_all(base);
}

TEST_CASE("FileStore: clear on non-existent file is ok") {
    TempFile guard(tmp_lease_path("clear_missing"));
    // File was removed in guard constructor; never written
    FileStore store(guard.path.string());

    auto cv = store.clear();
    CHECK(cv.is_ok());
}

TEST_CASE("default_store_path: produces a non-empty path under HOME") {
    Config cfg;
    cfg.tenantId  = "tenant123";
    cfg.productId = "prod456";

    std::string path = default_store_path(cfg);
    CHECK_FALSE(path.empty());
    // Should contain the tenantId and productId somewhere
    CHECK(path.find("tenant123") != std::string::npos);
    CHECK(path.find("prod456")   != std::string::npos);
}


// ---------------------------------------------------------------------------
// EncryptedFileStore — machine-bound, authenticated on-disk store.
// ---------------------------------------------------------------------------

TEST_CASE("EncryptedFileStore: round-trips a blob") {
    const std::string path = (std::filesystem::temp_directory_path() /
                              "kl-enc-roundtrip.bin").string();
    std::filesystem::remove(path);

    keylight::EncryptedFileStore store(path, "machine-a");
    REQUIRE(store.save(R"({"trialStart":1781076246})").is_ok());

    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value() == R"({"trialStart":1781076246})");

    std::filesystem::remove(path);
}

TEST_CASE("EncryptedFileStore: the blob is not readable on disk") {
    const std::string path = (std::filesystem::temp_directory_path() /
                              "kl-enc-opaque.bin").string();
    std::filesystem::remove(path);

    keylight::EncryptedFileStore store(path, "machine-a");
    REQUIRE(store.save(R"({"trialStart":1781076246})").is_ok());

    std::ifstream f(path, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    // Resetting trialStart with a text editor is the attack this closes.
    CHECK(raw.find("trialStart") == std::string::npos);
    CHECK(raw.size() >= 28);   // 12-byte nonce + 16-byte tag minimum

    std::filesystem::remove(path);
}

TEST_CASE("EncryptedFileStore: a blob from another machine does not open") {
    const std::string path = (std::filesystem::temp_directory_path() /
                              "kl-enc-transplant.bin").string();
    std::filesystem::remove(path);

    {
        keylight::EncryptedFileStore origin(path, "machine-a");
        REQUIRE(origin.save(R"({"lease":"..."})").is_ok());
    }

    // Copying the file to a second machine derives a different key. This is
    // the whole point of the design: a store this SDK writes is not portable.
    keylight::EncryptedFileStore elsewhere(path, "machine-b");
    auto loaded = elsewhere.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().empty());   // undecryptable reads as "no data"

    std::filesystem::remove(path);
}

TEST_CASE("EncryptedFileStore: a missing file is not an error") {
    const std::string path = (std::filesystem::temp_directory_path() /
                              "kl-enc-absent.bin").string();
    std::filesystem::remove(path);

    keylight::EncryptedFileStore store(path, "machine-a");
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().empty());
}

TEST_CASE("EncryptedFileStore: a truncated file is not an error") {
    const std::string path = (std::filesystem::temp_directory_path() /
                              "kl-enc-truncated.bin").string();
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "short";
    }

    // A half-written file from a crash must degrade to "no data", never crash.
    keylight::EncryptedFileStore store(path, "machine-a");
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().empty());

    std::filesystem::remove(path);
}

TEST_CASE("EncryptedFileStore: two saves of the same data differ on disk") {
    const std::string path = (std::filesystem::temp_directory_path() /
                              "kl-enc-nonce.bin").string();
    std::filesystem::remove(path);

    keylight::EncryptedFileStore store(path, "machine-a");
    REQUIRE(store.save("same").is_ok());
    std::string first;
    { std::ifstream f(path, std::ios::binary);
      first.assign((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>()); }

    REQUIRE(store.save("same").is_ok());
    std::string second;
    { std::ifstream f(path, std::ios::binary);
      second.assign((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>()); }

    // A repeated nonce under one key breaks Poly1305 outright.
    CHECK(first != second);

    std::filesystem::remove(path);
}

TEST_CASE("EncryptedFileStore: an absent machine id still encrypts, and binds to a constant") {
    // read_hardware_id() returns nullopt on a machine with no stable id (a
    // Linux image with neither /etc/machine-id nor the dbus fallback). The
    // empty string is hashed like any other id. This pins that choice.
    const std::string path = (std::filesystem::temp_directory_path() /
                              "kl-enc-noid.bin").string();
    std::filesystem::remove(path);

    keylight::EncryptedFileStore store(path, "");
    REQUIRE(store.save(R"({"trialStart":1781076246})").is_ok());

    // Nobody is locked out: it round-trips like any other store.
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value() == R"({"trialStart":1781076246})");

    // And the tamper protection is unaffected — this is the property that
    // matters most, and it holds on every machine.
    std::ifstream f(path, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    CHECK(raw.find("trialStart") == std::string::npos);

    // The accepted trade, asserted rather than left implicit: two id-less
    // machines derive the same key, so a blob IS portable between them.
    // Binding degrades exactly where the OS gave us nothing to bind to.
    keylight::EncryptedFileStore other_idless_machine(path, "");
    auto shared = other_idless_machine.load();
    REQUIRE(shared.is_ok());
    CHECK(shared.value() == R"({"trialStart":1781076246})");

    // But it is still not portable to a machine that DOES have an id.
    keylight::EncryptedFileStore identified(path, "machine-a");
    auto denied = identified.load();
    REQUIRE(denied.is_ok());
    CHECK(denied.value().empty());

    std::filesystem::remove(path);
}


// ---------------------------------------------------------------------------
// One-time migration from the pre-0.2.0 plaintext store.
// ---------------------------------------------------------------------------

TEST_CASE("EncryptedFileStore: imports a plaintext store on first read") {
    const auto dir = std::filesystem::temp_directory_path();
    const std::string enc = (dir / "kl-mig-enc.bin").string();
    const std::string old = (dir / "kl-mig-old.lease").string();
    std::filesystem::remove(enc);

    const std::string legacy = R"({"trialStart":1781076246,"licenseKey":"XXXX-YYYY"})";
    { std::ofstream f(old, std::ios::binary | std::ios::trunc); f << legacy; }

    keylight::EncryptedFileStore store(enc, "machine-a", old);
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());

    // Everything is imported, trial start included. Dropping it would buy
    // nothing: deleting any store mints a fresh trial, encrypted or not.
    CHECK(loaded.value() == legacy);

    // The plaintext file is removed so the install stops producing copies.
    CHECK(std::filesystem::exists(old) == false);
    CHECK(std::filesystem::exists(enc) == true);

    std::filesystem::remove(enc);
}

TEST_CASE("EncryptedFileStore: an existing encrypted store wins over plaintext") {
    const auto dir = std::filesystem::temp_directory_path();
    const std::string enc = (dir / "kl-mig-precedence-enc.bin").string();
    const std::string old = (dir / "kl-mig-precedence-old.lease").string();
    std::filesystem::remove(enc);

    { keylight::EncryptedFileStore seed(enc, "machine-a"); REQUIRE(seed.save("current").is_ok()); }
    { std::ofstream f(old, std::ios::binary | std::ios::trunc); f << "stale"; }

    keylight::EncryptedFileStore store(enc, "machine-a", old);
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value() == "current");
    // The plaintext file is never even read, so it is left alone.
    CHECK(std::filesystem::exists(old) == true);

    std::filesystem::remove(enc);
    std::filesystem::remove(old);
}

TEST_CASE("EncryptedFileStore: no plaintext file is a clean first run") {
    const auto dir = std::filesystem::temp_directory_path();
    const std::string enc = (dir / "kl-mig-none-enc.bin").string();
    const std::string old = (dir / "kl-mig-none-old.lease").string();
    std::filesystem::remove(enc);
    std::filesystem::remove(old);

    keylight::EncryptedFileStore store(enc, "machine-a", old);
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().empty());
}

TEST_CASE("EncryptedFileStore: an empty plaintext file imports nothing") {
    const auto dir = std::filesystem::temp_directory_path();
    const std::string enc = (dir / "kl-mig-empty-enc.bin").string();
    const std::string old = (dir / "kl-mig-empty-old.lease").string();
    std::filesystem::remove(enc);
    { std::ofstream f(old, std::ios::binary | std::ios::trunc); }

    keylight::EncryptedFileStore store(enc, "machine-a", old);
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().empty());
    // Nothing was imported, so nothing is written and no encrypted file appears.
    CHECK(std::filesystem::exists(enc) == false);

    std::filesystem::remove(old);
}

TEST_CASE("legacy_plaintext_path still points at the pre-0.2.0 .lease file") {
    Config cfg;
    cfg.tenantId  = "tenant123";
    cfg.productId = "prod456";

    // default_store_path moved to .bin with the encrypted format; the legacy
    // helper must keep naming the old file, or migration finds nothing.
    CHECK(default_store_path(cfg).substr(default_store_path(cfg).size() - 4) == ".bin");
    const std::string legacy = legacy_plaintext_path(cfg);
    CHECK(legacy.substr(legacy.size() - 6) == ".lease");
    CHECK(legacy.find("tenant123") != std::string::npos);
    CHECK(legacy.find("prod456")   != std::string::npos);
}

TEST_CASE("EncryptedFileStore::migrating is the README's call, and it works") {
    // The README tells integrators to use this exact form. Exercising it here
    // is what stops the documented call from drifting away from the API — an
    // unverified snippet is how the JUCE adapter shipped a non-compiling
    // signature.
    //
    // Temp paths, not default_store_path(): migrating() DELETES the legacy
    // file it imports, and no test may be able to remove something from the
    // developer's real ~/.keylight.
    const auto dir = std::filesystem::temp_directory_path();
    const std::string enc      = (dir / "kl-mig-factory-enc.bin").string();
    const std::string old_path = (dir / "kl-mig-factory-old.lease").string();
    std::filesystem::remove(enc);

    const std::string legacy = R"({"licenseKey":"XXXX-YYYY"})";
    { std::ofstream f(old_path, std::ios::binary | std::ios::trunc); f << legacy; }

    auto store = keylight::EncryptedFileStore::migrating(enc, old_path);
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());

    // Binds to the real machine id, so the import round-trips on this machine.
    CHECK(loaded.value() == legacy);
    CHECK(std::filesystem::exists(old_path) == false);

    std::filesystem::remove(enc);
}

TEST_CASE("default_store_path and legacy_plaintext_path do not collide") {
    // Migration reads one and writes the other. If they resolved to the same
    // file, save() would truncate the source mid-import.
    Config cfg;
    cfg.tenantId  = "tenant123";
    cfg.productId = "prod456";
    CHECK(default_store_path(cfg) != legacy_plaintext_path(cfg));
}
