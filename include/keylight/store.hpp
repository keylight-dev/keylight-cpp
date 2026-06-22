#pragma once
#include "result.hpp"
#include "config.hpp"
#include <filesystem>
#include <fstream>
#include <string>

namespace keylight {

// ---------------------------------------------------------------------------
// LicenseStore — abstract cache seam for persisting the verified lease blob
// ---------------------------------------------------------------------------
class LicenseStore {
public:
    virtual ~LicenseStore() = default;

    // Returns the stored lease blob, or an ok Result with an empty string if
    // no lease has been saved yet. A missing file is NOT an error.
    virtual Result<std::string> load() = 0;

    // Persists the lease blob. Implementations should write atomically so
    // a crash during save never leaves a half-written file behind.
    virtual Result<void> save(const std::string& data) = 0;

    // Removes the stored lease. Removing a file that does not exist is NOT
    // an error.
    virtual Result<void> clear() = 0;
};

// ---------------------------------------------------------------------------
// FileStore — default on-disk implementation
//
// save() writes atomically: data → temp file → std::filesystem::rename.
// Parent directories are created on first save.
// All filesystem_errors are caught and mapped to Result::err(ErrorCode::Io).
// ---------------------------------------------------------------------------
class FileStore : public LicenseStore {
public:
    explicit FileStore(std::string path) : path_(std::move(path)) {}

    Result<std::string> load() override {
        namespace fs = std::filesystem;
        try {
            if (!fs::exists(path_)) {
                return Result<std::string>::ok(std::string{});
            }
            std::ifstream f(path_, std::ios::binary);
            if (!f) {
                return Result<std::string>::err(
                    {ErrorCode::Io, "FileStore: cannot open " + path_});
            }
            std::string data(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
            return Result<std::string>::ok(std::move(data));
        } catch (const std::filesystem::filesystem_error& e) {
            return Result<std::string>::err({ErrorCode::Io, e.what()});
        }
    }

    Result<void> save(const std::string& data) override {
        namespace fs = std::filesystem;
        try {
            fs::path target(path_);

            // Create parent directories if they don't exist
            if (target.has_parent_path()) {
                fs::create_directories(target.parent_path());
            }

            // Write to a sibling temp file, then rename atomically
            fs::path tmp = target;
            tmp += ".tmp";

            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    return Result<void>::err(
                        {ErrorCode::Io, "FileStore: cannot write " + tmp.string()});
                }
                f.write(data.data(), static_cast<std::streamsize>(data.size()));
                if (!f) {
                    return Result<void>::err(
                        {ErrorCode::Io, "FileStore: write failed for " + tmp.string()});
                }
            } // flush + close before rename

            fs::rename(tmp, target);
            return Result<void>::ok();
        } catch (const std::filesystem::filesystem_error& e) {
            return Result<void>::err({ErrorCode::Io, e.what()});
        }
    }

    Result<void> clear() override {
        namespace fs = std::filesystem;
        try {
            std::error_code ec;
            fs::remove(path_, ec);
            // Ignore ec: removing a non-existent file is not an error
            return Result<void>::ok();
        } catch (const std::filesystem::filesystem_error& e) {
            return Result<void>::err({ErrorCode::Io, e.what()});
        }
    }

private:
    std::string path_;
};

// ---------------------------------------------------------------------------
// default_store_path — sensible per-tenant/per-product path
//
// POSIX: $HOME/.keylight/<tenantId>-<productId>.lease
// Fallback: /tmp/.keylight/<tenantId>-<productId>.lease
// ---------------------------------------------------------------------------
inline std::string default_store_path(const Config& cfg) {
    namespace fs = std::filesystem;

    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".keylight"
                         : fs::temp_directory_path() / ".keylight";

    std::string filename = cfg.tenantId + "-" + cfg.productId + ".lease";
    return (base / filename).string();
}

} // namespace keylight
