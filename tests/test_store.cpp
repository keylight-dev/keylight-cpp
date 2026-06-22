#include "doctest.h"
#include "keylight/store.hpp"
#include <filesystem>

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
