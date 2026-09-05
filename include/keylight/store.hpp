#pragma once
#include "result.hpp"
#include "config.hpp"
#include "chacha20poly1305.hpp"
#include "machine_id.hpp"
#include "sha256.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
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
// POSIX: $HOME/.keylight/<tenantId>-<productId>.bin
// Fallback: /tmp/.keylight/<tenantId>-<productId>.bin
//
// The extension changed with the encrypted format, so a 0.2.0 store never
// collides with the plaintext file it migrated from — and so pointing a plain
// FileStore at this path cannot silently overwrite a pre-0.2.0 store.
// ---------------------------------------------------------------------------
inline std::string default_store_path(const Config& cfg) {
    namespace fs = std::filesystem;

    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".keylight"
                         : fs::temp_directory_path() / ".keylight";

    std::string filename = cfg.tenantId + "-" + cfg.productId + ".bin";
    return (base / filename).string();
}

// The pre-0.2.0 plaintext store location. Kept only so 0.2.0 can import it
// once; nothing writes here any more. Pass it as EncryptedFileStore's third
// argument to enable that import.
inline std::string legacy_plaintext_path(const Config& cfg) {
    namespace fs = std::filesystem;

    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".keylight"
                         : fs::temp_directory_path() / ".keylight";

    return (base / (cfg.tenantId + "-" + cfg.productId + ".lease")).string();
}

namespace detail {

/// Store-encryption key, bound to this machine.
///
/// A plaintext store is a portable license: copy the file and the lease, the
/// trial start and the instance id all travel with it. Binding the key to the
/// machine makes anything this SDK writes useless if copied.
///
/// Mirrors keylight-rust, which derives its ChaCha20-Poly1305 key with BLAKE3
/// under the same domain string. SHA-256 is used here because this SDK already
/// vendors it. The digest is taken over raw bytes, not over a hex rendering.
inline std::array<uint8_t, 32> derive_store_key(const std::string& machine_id) {
    const std::string material = "keylight-store-v1" + machine_id;
    return sha256_bytes(reinterpret_cast<const uint8_t*>(material.data()),
                        material.size());
}

/// The machine id used for store binding, or a constant when the platform
/// exposes none.
///
/// read_hardware_id() returns nullopt on a machine with no stable identifier —
/// a Linux image with neither /etc/machine-id nor the dbus fallback is the
/// realistic case. The empty string is then hashed like any other id, which
/// means every such machine derives the SAME key.
///
/// That is a deliberate trade, not an oversight:
///   - Tamper protection is unaffected. The blob is still authenticated
///     everywhere, so editing trialStart in a text editor still fails.
///   - Machine binding degrades only where the OS gave us nothing to bind to.
///     Blobs stay portable among such machines, and remain non-portable to or
///     from any machine that does have an id.
///   - Nobody is locked out. The alternatives — refusing to persist, or
///     inventing a random id — either force a reactivation on every launch
///     (burning a seat each time) or add a sidecar file whose loss does the
///     same.
///
/// Distinct from machine_hash(), which must NEVER substitute a value when the
/// id is absent: that one exists to dedupe a device server-side, and a
/// made-up value corrupts the count. Here the id is only key material.
inline std::string store_binding_id() {
    if (auto id = read_hardware_id()) return *id;
    return std::string{};
}

} // namespace detail

// ---------------------------------------------------------------------------
// EncryptedFileStore — machine-bound, authenticated on-disk store.
//
// Layout: nonce(12) || ciphertext || tag(16)
//
// Anything that fails to decrypt reads as "no data" rather than an error: a
// truncated file from a crash, a file copied from another machine, and a
// file from a previous machine id are all indistinguishable to us and all
// mean the same thing operationally — there is no usable cached state.
//
// Note the operational consequence: if this machine's id CHANGES (a hardware
// swap, an OS reinstall, a regenerated /etc/machine-id), the existing store
// stops opening and the app sees a first run. That costs the user a
// reactivation, and is the intended cost of the store not being portable.
// ---------------------------------------------------------------------------
class EncryptedFileStore : public LicenseStore {
public:
    // `legacy_path`, when set, is a pre-0.2.0 plaintext store to import once.
    EncryptedFileStore(std::string path, std::string machine_id,
                       std::string legacy_path = std::string())
        : path_(std::move(path)),
          legacy_path_(std::move(legacy_path)),
          key_(detail::derive_store_key(machine_id)) {}

    explicit EncryptedFileStore(std::string path)
        : EncryptedFileStore(std::move(path), detail::store_binding_id()) {}

    /// The migrating default: bind to this machine, and import a pre-0.2.0
    /// plaintext store once if one is there.
    ///
    /// This exists so an integrator never has to reach into `detail::` for the
    /// machine id. The three-argument constructor takes an explicit id because
    /// tests need to pin it; ordinary callers want this.
    static EncryptedFileStore migrating(std::string path,
                                        std::string legacy_path) {
        return EncryptedFileStore(std::move(path), detail::store_binding_id(),
                                  std::move(legacy_path));
    }

    Result<std::string> load() override {
        namespace fs = std::filesystem;
        try {
            // One-time migration from the pre-0.2.0 plaintext store.
            //
            // Everything is imported, trial start included. Gating any of it
            // buys nothing: deleting a store mints a fresh trial whether or
            // not it was encrypted, and a lease copied from a pre-0.2.0
            // install is no more useful than a shared license key. What this
            // DOES buy is that the supply dries up — once an install upgrades,
            // its plaintext file is gone and never regenerated.
            //
            // See spec section 4.3: seat sharing is a protocol gap, closed by
            // binding an instance to machine_hash server-side, not here.
            if (!legacy_path_.empty() && !fs::exists(path_) && fs::exists(legacy_path_)) {
                std::ifstream lf(legacy_path_, std::ios::binary);
                if (lf) {
                    const std::string legacy((std::istreambuf_iterator<char>(lf)),
                                              std::istreambuf_iterator<char>());
                    lf.close();
                    if (!legacy.empty()) {
                        // Only unlink once the encrypted copy is safely on
                        // disk — a crash between the two must not lose state.
                        if (save(legacy).is_ok()) {
                            std::error_code ec;
                            fs::remove(legacy_path_, ec);
                        }
                        return Result<std::string>::ok(legacy);
                    }
                }
            }

            if (!fs::exists(path_)) return Result<std::string>::ok(std::string{});

            std::ifstream f(path_, std::ios::binary);
            if (!f) return Result<std::string>::ok(std::string{});

            const std::string blob((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
            if (blob.size() < 12 + 16) return Result<std::string>::ok(std::string{});

            const uint8_t* p       = reinterpret_cast<const uint8_t*>(blob.data());
            const size_t   ct_len  = blob.size() - 12 - 16;
            const uint8_t* nonce   = p;
            const uint8_t* ct      = p + 12;
            const uint8_t* tag     = p + 12 + ct_len;

            std::string out(ct_len, '\0');
            const bool ok = detail::aead_open(
                key_.data(), nonce, ct, ct_len, tag,
                ct_len ? reinterpret_cast<uint8_t*>(&out[0]) : nullptr);
            if (!ok) return Result<std::string>::ok(std::string{});

            return Result<std::string>::ok(std::move(out));
        } catch (const std::filesystem::filesystem_error&) {
            return Result<std::string>::ok(std::string{});
        }
    }

    Result<void> save(const std::string& data) override {
        namespace fs = std::filesystem;
        try {
            fs::path target(path_);
            if (target.has_parent_path()) fs::create_directories(target.parent_path());

            // A repeated nonce under one key breaks Poly1305 outright, so this
            // is drawn fresh for every write, never derived from the content.
            uint8_t nonce[12];
            {
                static thread_local std::mt19937_64 rng{std::random_device{}()};
                for (int i = 0; i < 12; i += 8) {
                    uint64_t v = rng();
                    for (int j = 0; j < 8 && i + j < 12; ++j) {
                        nonce[i + j] = static_cast<uint8_t>(v >> (8 * j));
                    }
                }
            }

            std::string ct(data.size(), '\0');
            uint8_t     tag[16];
            detail::aead_seal(
                key_.data(), nonce,
                data.empty() ? nullptr : reinterpret_cast<const uint8_t*>(data.data()),
                data.size(),
                data.empty() ? nullptr : reinterpret_cast<uint8_t*>(&ct[0]),
                tag);

            std::string blob;
            blob.reserve(12 + ct.size() + 16);
            blob.append(reinterpret_cast<const char*>(nonce), 12);
            blob.append(ct);
            blob.append(reinterpret_cast<const char*>(tag), 16);

            fs::path tmp = target; tmp += ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    return Result<void>::err(
                        {ErrorCode::Io, "EncryptedFileStore: cannot write " + tmp.string()});
                }
                f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
                if (!f) {
                    return Result<void>::err(
                        {ErrorCode::Io, "EncryptedFileStore: write failed for " + tmp.string()});
                }
            }
            fs::rename(tmp, target);
            return Result<void>::ok();
        } catch (const std::filesystem::filesystem_error& e) {
            return Result<void>::err({ErrorCode::Io, e.what()});
        }
    }

    Result<void> clear() override {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        return Result<void>::ok();
    }

private:
    std::string                path_;
    std::string                legacy_path_;
    std::array<uint8_t, 32>    key_;
};

} // namespace keylight
