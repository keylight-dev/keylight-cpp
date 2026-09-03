# Free Tier & Keyless Beacon Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the C++ SDK the free-tier feature the other Keylight SDKs already have — a `FreeTier` state, an anonymous keyless heartbeat, and the cross-SDK `machine_hash`.

**Architecture:** Header-only additions to `keylight::Client`, ported from `keylight-rust`. One new header (`machine_id.hpp`) carries platform hardware-ID reads, the SHA-256 machine hash, and a UUIDv4 generator. All new persisted fields route through the single `build_blob_locked_()` serializer PR #3 introduced. The beacon is caller-driven in the core; the JUCE adapter fires it automatically on its existing background dispatch.

**Tech Stack:** C++17, header-only, doctest, CMake 3.16. New link deps: IOKit + CoreFoundation (Apple), advapi32 (Windows).

**Spec:** `docs/superpowers/specs/2026-09-03-free-tier-keyless-design.md`

## Global Constraints

- **C++17**, header-only core. No new third-party libraries.
- Branch `claude/keylight-cpp-free-tier-keyless`, stacked on `claude/keylight-cpp-auth-trial-m2kizb` (PR #3). Do **not** rebase onto `main`.
- Wire strings are fixed by the server and other SDKs: `"trial"`, `"free_tier"`, `"expired"`.
- Canonical cross-SDK hash vector, verified: `sha256("keylight-keyless-machine-v1|testco|testapp|hardware-1")` == `8e8871112f28cabda180ada131d0b4f4f07c72fb47c5d884edbe32812885b22a`.
- The hardware ID never falls back to a random value. No true hardware ID ⇒ `machine_hash` is omitted from the body entirely.
- Debounce markers persist **only** on HTTP 200.
- `State::FreeTier` is appended **after** `Invalid`; never renumber existing enumerators.
- Existing public constructors keep working unchanged.
- Run the full suite with `cmake --build build -j8 && ctest --test-dir build --output-on-failure` before every commit.

---

### Task 1: `machine_id.hpp` — hardware ID, machine hash, UUIDv4

**Files:**
- Create: `include/keylight/machine_id.hpp`
- Create: `tests/test_machine_id.cpp`
- Modify: `CMakeLists.txt` (link IOKit/CoreFoundation/advapi32)

**Interfaces:**
- Consumes: `keylight::sha256_hex` from `sha256.hpp:189`
- Produces:
  - `keylight::detail::read_hardware_id() -> std::optional<std::string>`
  - `keylight::detail::machine_hash(const std::string& tenant, const std::string& product, const std::string& stable_id) -> std::string`
  - `keylight::detail::uuid_v4() -> std::string`

- [ ] **Step 1: Write the failing test**

Create `tests/test_machine_id.cpp`:

```cpp
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j8 2>&1 | tail -5`
Expected: FAIL — `fatal error: 'keylight/machine_id.hpp' file not found`

- [ ] **Step 3: Write minimal implementation**

Create `include/keylight/machine_id.hpp`:

```cpp
#pragma once
// keylight/machine_id.hpp — device identity for the anonymous keyless beacon.
//
// Ported from keylight-rust keylight/src/machine.rs and
//            keylight/src/store/device.rs
//
// `machine_hash` lets the server dedupe keyless/free-tier devices by a *true*
// OS/hardware identifier instead of a random per-install id.  It is only ever
// computed from a real hardware id — callers MUST omit the field entirely when
// read_hardware_id() returns nullopt.  Substituting a random fallback would
// defeat the cross-install dedup the hash exists for.

#include "sha256.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <string>

#if defined(__APPLE__)
#  include <CoreFoundation/CoreFoundation.h>
#  include <IOKit/IOKitLib.h>
#elif defined(_WIN32)
#  include <windows.h>
#else
#  include <fstream>
#  include <sstream>
#endif

namespace keylight {
namespace detail {

/// sha256("keylight-keyless-machine-v1|{tenant}|{product}|{stable_id}"),
/// lowercase hex.  Must match byte-for-byte across all Keylight SDKs.
inline std::string machine_hash(const std::string& tenant_id,
                                const std::string& product_id,
                                const std::string& stable_id)
{
    return sha256_hex("keylight-keyless-machine-v1|" + tenant_id + "|" +
                      product_id + "|" + stable_id);
}

#if !defined(__APPLE__) && !defined(_WIN32)
/// Read a file and trim surrounding whitespace; nullopt when missing or blank.
inline std::optional<std::string> read_file_trimmed_(const char* path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    const char* ws = " \t\n\r\f\v";
    const auto  b  = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::nullopt;
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}
#endif

/// True OS/hardware machine id, or nullopt when the platform has none.
/// NEVER returns a random fallback — see the header comment.
inline std::optional<std::string> read_hardware_id() {
#if defined(__APPLE__)
    // MACH_PORT_NULL selects the default port on every macOS version, which
    // sidesteps the kIOMasterPortDefault -> kIOMainPortDefault rename in the
    // macOS 12 SDK.
    io_service_t svc = IOServiceGetMatchingService(
        MACH_PORT_NULL, IOServiceMatching("IOPlatformExpertDevice"));
    if (!svc) return std::nullopt;
    CFTypeRef prop = IORegistryEntryCreateCFProperty(
        svc, CFSTR("IOPlatformUUID"), kCFAllocatorDefault, 0);
    IOObjectRelease(svc);
    if (!prop) return std::nullopt;
    std::optional<std::string> out;
    if (CFGetTypeID(prop) == CFStringGetTypeID()) {
        char buf[128] = {0};
        if (CFStringGetCString(static_cast<CFStringRef>(prop), buf, sizeof(buf),
                               kCFStringEncodingUTF8) && buf[0] != '\0') {
            out = std::string(buf);
        }
    }
    CFRelease(prop);
    return out;

#elif defined(_WIN32)
    // RRF_SUBKEY_WOW6464KEY forces the 64-bit registry view.  Without it a
    // 32-bit plugin host is redirected and reads a DIFFERENT MachineGuid than
    // the other SDKs, silently breaking the cross-SDK hash.
    char  buf[256];
    DWORD size = sizeof(buf);
    const LSTATUS rc = ::RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Cryptography",
        "MachineGuid",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
        nullptr, buf, &size);
    if (rc != ERROR_SUCCESS || buf[0] == '\0') return std::nullopt;
    return std::string(buf);

#else
    if (auto v = read_file_trimmed_("/etc/machine-id")) return v;
    return read_file_trimmed_("/var/lib/dbus/machine-id");
#endif
}

/// RFC 4122 version-4 UUID.  Used for the anonymous free-tier instance id.
inline std::string uuid_v4() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<unsigned> dist(0, 255);

    uint8_t b[16];
    for (auto& x : b) x = static_cast<uint8_t>(dist(gen));
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);  // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);  // variant

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += kHex[(b[i] >> 4) & 0x0f];
        out += kHex[ b[i]       & 0x0f];
    }
    return out;
}

} // namespace detail
} // namespace keylight
```

- [ ] **Step 4: Add the platform link dependencies**

In `CMakeLists.txt`, immediately after the `target_include_directories(keylight INTERFACE ...)` block, add:

```cmake
# machine_id.hpp reads the OS hardware id for the keyless beacon's machine_hash.
# This is the ONLY external linkage the core needs; there are still no
# third-party library dependencies.
if(APPLE)
  target_link_libraries(keylight INTERFACE "-framework IOKit" "-framework CoreFoundation")
elseif(WIN32)
  target_link_libraries(keylight INTERFACE advapi32)
endif()
```

Then update the section comment above `add_library(keylight INTERFACE)` from
`# Core library — zero external dependencies` to
`# Core library — no third-party dependencies (system frameworks only)`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: PASS — 96 test cases, 0 failures.

- [ ] **Step 6: Mutation-check the canonical vector**

Change the prefix in `machine_hash` from `"keylight-keyless-machine-v1|"` to `"keylight-keyless-machine-v2|"`, rebuild, and confirm the canonical-vector test fails. Restore it and confirm the suite passes again.

- [ ] **Step 7: Commit**

```bash
git add include/keylight/machine_id.hpp tests/test_machine_id.cpp CMakeLists.txt
git commit -m "feat(machine-id): cross-SDK machine_hash, hardware id and uuid_v4"
```

---

### Task 2: `State::FreeTier`, the config flag, and state resolution

**Files:**
- Modify: `include/keylight/config.hpp`
- Modify: `include/keylight/client.hpp:46-51` (State enum), `:960-972` (`resolve_with_trial_`)
- Create: `tests/test_free_tier.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `keylight::Config::freeTierEnabled` (`bool`, default `false`)
  - `keylight::State::FreeTier` (appended after `Invalid`)
  - `keylight::KeylessState` (`Trial`, `FreeTier`, `Expired`)
  - `keylight::keyless_state_wire(KeylessState) -> const char*`

- [ ] **Step 1: Write the failing test**

Create `tests/test_free_tier.cpp`. Copy the `RecordingTransport`, `MemStore`, `make_config`, `PUBKEY_K1`, `T0`, `SDK_KEY`, `ACTIVATE_OK` and `DAY` helpers verbatim from `tests/test_auth_trial.cpp:22-175` into an anonymous namespace (the two TUs link into one binary, so the anonymous namespace is what keeps the names from colliding). Then add:

```cpp
TEST_CASE("Wire strings for KeylessState match the other SDKs") {
    CHECK(std::string(keyless_state_wire(KeylessState::Trial))    == "trial");
    CHECK(std::string(keyless_state_wire(KeylessState::FreeTier)) == "free_tier");
    CHECK(std::string(keyless_state_wire(KeylessState::Expired))  == "expired");
}

TEST_CASE("FreeTier: disabled by default — no license resolves Invalid") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    CHECK(client.checkOnLaunch().value() == State::Invalid);
}

TEST_CASE("FreeTier: enabled and no trial resolves FreeTier offline") {
    auto cfg = make_config();
    cfg.freeTierEnabled = true;
    RecordingTransport transport;
    MemStore           store;
    transport.fail = true;             // any request would fail — there must be none
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value()      == State::FreeTier);
    CHECK(client.state() == State::FreeTier);
    CHECK(transport.calls.empty());
}

TEST_CASE("FreeTier: an active trial still outranks the free tier") {
    auto cfg = make_config(14);
    cfg.freeTierEnabled = true;
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());
    CHECK(client.state() == State::Trial);
}

TEST_CASE("FreeTier: an ELAPSED trial drops to FreeTier, not Expired") {
    // keylight-rust resolve_state(): the `free_tier_enabled` arm sits after the
    // trial match, so an elapsed trial falls through to FreeTier.  Without free
    // tier the same client resolves Expired.
    auto cfg = make_config(14);
    cfg.freeTierEnabled = true;
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    {
        Client client(cfg, transport, store, [&]{ return now; });
        REQUIRE(client.startTrial().is_ok());
    }

    now = T0 + 14 * DAY;
    Client elapsed(cfg, transport, store, [&]{ return now; });
    CHECK(elapsed.checkOnLaunch().value() == State::FreeTier);

    auto no_ft = make_config(14);
    Client without(no_ft, transport, store, [&]{ return now; });
    CHECK(without.checkOnLaunch().value() == State::Expired);
}

TEST_CASE("FreeTier: a paid license still wins") {
    auto cfg = make_config();
    cfg.freeTierEnabled = true;
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.activate("KL-TEST-KEY").is_ok());
    CHECK(client.state() == State::Licensed);
}

TEST_CASE("FreeTier: deactivate lands on FreeTier rather than the paywall") {
    auto cfg = make_config();
    cfg.freeTierEnabled = true;
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.activate("KL-TEST-KEY").is_ok());
    REQUIRE(client.state() == State::Licensed);

    transport.next_body = "{}";
    REQUIRE(client.deactivate().is_ok());
    CHECK(client.state() == State::FreeTier);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j8 2>&1 | tail -5`
Expected: FAIL — `no member named 'freeTierEnabled' in 'keylight::Config'`

- [ ] **Step 3: Add the config flag**

In `include/keylight/config.hpp`, after `int trialDurationDays = 0;`:

```cpp
    // Free tier: when true, a device with no license and no active trial
    // resolves State::FreeTier instead of Invalid/Expired.  Parity with
    // keylight-rust Config::free_tier_enabled.
    bool        freeTierEnabled    = false;
```

- [ ] **Step 4: Add the enums**

In `include/keylight/client.hpp`, extend the `State` enum (append only — existing
enumerator values must not renumber):

```cpp
enum class State {
    Licensed,   // trusted, unexpired active lease
    Trial,      // no license; within trial window
    Expired,    // trusted lease expired, or license status "expired"/"fallback"
    Invalid,    // no trusted lease, no active trial
    FreeTier,   // no license and no trial, but the product offers a free tier.
                // Appended last on purpose: renumbering the values above would
                // break any integrator that persisted a State as an integer.
};
```

Directly below the `TrialStatus` enum, add:

```cpp
// ---------------------------------------------------------------------------
// KeylessState — what the anonymous keyless beacon reports (mirrors
// keylight-rust KeylessState).  The wire strings are fixed by the server and
// shared with every other Keylight SDK.
// ---------------------------------------------------------------------------
enum class KeylessState {
    Trial,
    FreeTier,
    Expired,
};

inline const char* keyless_state_wire(KeylessState s) {
    switch (s) {
        case KeylessState::Trial:    return "trial";
        case KeylessState::FreeTier: return "free_tier";
        case KeylessState::Expired:  return "expired";
    }
    return "expired";
}
```

- [ ] **Step 5: Teach the resolver about the free tier**

Replace the body of `resolve_with_trial_` in `include/keylight/client.hpp`:

```cpp
    /// Apply the local-trial and free-tier fallbacks to a state resolved from
    /// paid licensing.  Priority: valid paid license → active trial → free tier
    /// → elapsed trial → Invalid.  Mirrors keylight-rust resolve_state(), where
    /// the `free_tier_enabled` arm sits AFTER the trial match: an elapsed trial
    /// on a free-tier product drops to the free tier, not the paywall.
    /// Must NOT be called while holding cache_mutex_ (checkTrial() locks it).
    State resolve_with_trial_(State paid_state) const {
        if (paid_state != State::Invalid) {
            return paid_state;
        }
        switch (checkTrial()) {
            case TrialStatus::Active:
                return State::Trial;
            case TrialStatus::Expired:
                return cfg_.freeTierEnabled ? State::FreeTier : State::Expired;
            case TrialStatus::NotStarted:
                break;
        }
        return cfg_.freeTierEnabled ? State::FreeTier : State::Invalid;
    }
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: PASS. Every pre-existing test must still pass — `freeTierEnabled`
defaults to `false`, so no existing behavior changes.

- [ ] **Step 7: Commit**

```bash
git add include/keylight/config.hpp include/keylight/client.hpp tests/test_free_tier.cpp
git commit -m "feat(free-tier): State::FreeTier, KeylessState and the resolver arm"
```

---

### Task 3: The anonymous free-tier instance id

**Files:**
- Modify: `include/keylight/client.hpp` — `startTrial()` (`:377`), `build_blob_locked_()` (`:1039`), `refresh_state_from_store_()` (`:~915`), cache members
- Modify: `tests/test_free_tier.cpp`

**Interfaces:**
- Consumes: `keylight::detail::uuid_v4()` (Task 1)
- Produces: `std::string keylight::Client::freeTierInstanceId()` — mints on first call, persists, returns the same value forever after. Empty string only if the store write fails.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_free_tier.cpp`:

```cpp
TEST_CASE("Instance id: minted once and stable across calls") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    const std::string a = client.freeTierInstanceId();
    const std::string b = client.freeTierInstanceId();

    CHECK(a.size() == 36);
    CHECK(a == b);
    CHECK(store.str_field("freeTierInstanceId") == a);
}

TEST_CASE("Instance id: restored by a new Client on the same store") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    std::string first;
    {
        Client client(cfg, transport, store, [&]{ return now; });
        first = client.freeTierInstanceId();
    }
    Client relaunched(cfg, transport, store, [&]{ return now; });
    CHECK(relaunched.freeTierInstanceId() == first);
}

TEST_CASE("Instance id: startTrial mints one for conversion attribution") {
    // keylight-rust start_trial() writes FREE_TIER_INSTANCE_ID alongside
    // TRIAL_START so a trial that converts can be attributed.
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    CHECK(store.has_field("trialStart"));
    CHECK(store.str_field("freeTierInstanceId").size() == 36);
}

TEST_CASE("Instance id: survives activate, validate and deactivate") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    const std::string id = client.freeTierInstanceId();

    REQUIRE(client.activate("KL-TEST-KEY").is_ok());
    CHECK(store.str_field("freeTierInstanceId") == id);

    REQUIRE(client.validate().is_ok());
    CHECK(store.str_field("freeTierInstanceId") == id);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j8 2>&1 | tail -5`
Expected: FAIL — `no member named 'freeTierInstanceId' in 'keylight::Client'`

- [ ] **Step 3: Add the cache member and include**

In `include/keylight/client.hpp`, add `#include "machine_id.hpp"` next to the other
local includes. Beside `std::optional<int64_t> cached_trial_start_;` add:

```cpp
    std::optional<std::string> cached_free_tier_instance_id_;
```

- [ ] **Step 4: Persist and restore the field**

In `build_blob_locked_()`, after the `trialStart` block:

```cpp
        if (cached_free_tier_instance_id_.has_value()) {
            append("\"freeTierInstanceId\":" +
                   json_str(*cached_free_tier_instance_id_));
        }
```

Update the blob-format doc comment above it to list `freeTierInstanceId`.

In `refresh_state_from_store_()`, beside the existing `trialStart` load:

```cpp
            {
                // Anonymous free-tier instance id (see freeTierInstanceId()).
                std::string v = j["freeTierInstanceId"].as_string();
                if (!v.empty()) cached_free_tier_instance_id_ = v;
            }
```

- [ ] **Step 5: Add the public accessor**

In the public section of `Client`, next to the trial methods:

```cpp
    /// Anonymous, per-install identifier for keyless/free-tier reporting.
    /// Minted on first use and persisted; never derived from a license or from
    /// hardware.  Returns an empty string only when the store write fails.
    /// Mirrors keylight-rust free_tier_instance_id().
    std::string freeTierInstanceId() {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_free_tier_instance_id_.has_value()) {
                return *cached_free_tier_instance_id_;
            }
            cached_free_tier_instance_id_ = detail::uuid_v4();
        }
        if (!save_cache_().is_ok()) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_free_tier_instance_id_.reset();
            return {};
        }
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return *cached_free_tier_instance_id_;
    }
```

- [ ] **Step 6: Mint one in `startTrial()`**

Inside `startTrial()`, after the trial start is written and before returning,
add the attribution id (parity with keylight-rust `start_trial()`):

```cpp
        // Attribution: a trial that later converts must carry the same
        // anonymous id the keyless beacon reported it under.
        freeTierInstanceId();
```

- [ ] **Step 7: Run tests to verify they pass**

Run: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: PASS, including every pre-existing `Trial:` persistence case.

- [ ] **Step 8: Commit**

```bash
git add include/keylight/client.hpp tests/test_free_tier.cpp
git commit -m "feat(free-tier): persist an anonymous free-tier instance id"
```

---

### Task 4: Hardware-ID caching and the injectable seam

**Files:**
- Modify: `include/keylight/client.hpp` — constructors (`:124-146`), cache members, `build_blob_locked_()`, `refresh_state_from_store_()`
- Modify: `tests/test_free_tier.cpp`

**Interfaces:**
- Consumes: `detail::read_hardware_id()`, `detail::machine_hash()` (Task 1)
- Produces:
  - New 5-arg constructor `Client(Config, Transport&, LicenseStore&, std::function<int64_t()>, std::function<std::optional<std::string>()>)`
  - Private `std::optional<std::string> machine_hash_() const`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_free_tier.cpp`:

```cpp
// Canonical cross-SDK vector for tenant "testco" / product "testapp" /
// hardware id "hardware-1".
const char* CANONICAL_HASH =
    "8e8871112f28cabda180ada131d0b4f4f07c72fb47c5d884edbe32812885b22a";

Config canonical_config() {
    Config cfg;
    cfg.tenantId          = "testco";
    cfg.productId         = "testapp";
    cfg.sdkKey            = SDK_KEY;
    cfg.trustedKeys["k1"] = PUBKEY_K1;
    cfg.freeTierEnabled   = true;
    return cfg;
}

TEST_CASE("Hardware id: cached after the first successful read") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now   = T0;
    int     reads = 0;

    // Succeeds once, then fails — models a transient ioreg/registry failure.
    auto flaky = [&]() -> std::optional<std::string> {
        if (reads++ == 0) return std::string("hardware-1");
        return std::nullopt;
    };

    Client client(cfg, transport, store, [&]{ return now; }, flaky);
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(store.str_field("cachedHardwareId") == "hardware-1");

    // A later beacon must still carry the hash from the cached id.
    transport.calls.clear();
    client.reportKeylessState(KeylessState::Expired);
    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->body.find(CANONICAL_HASH) != std::string::npos);
}

TEST_CASE("Hardware id: machine_hash omitted entirely when none is available") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);

    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->body.find("machine_hash") == std::string::npos);
}

TEST_CASE("Hardware id: an empty id is treated as absent") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    auto empty = []() -> std::optional<std::string> { return std::string(""); };

    Client client(cfg, transport, store, [&]{ return now; }, empty);
    client.reportKeylessState(KeylessState::FreeTier);

    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->body.find("machine_hash") == std::string::npos);
}
```

These reference `reportKeylessState`, which Task 5 implements. Write them now,
watch them fail to compile, and leave them failing until Task 5 — or, if you
prefer a green bar between tasks, implement Task 4 Steps 2-5 first and add these
three cases at the start of Task 5. Either order is fine; do not skip them.

- [ ] **Step 2: Add the members**

```cpp
    std::function<std::optional<std::string>()> hardware_id_fn_;
    std::optional<std::string>                  cached_hardware_id_;
```

- [ ] **Step 3: Add the constructor overload**

Keep both existing constructors; they delegate with the real reader:

```cpp
    Client(Config cfg, Transport& transport, LicenseStore& store)
        : Client(std::move(cfg), transport, store,
                 []{ return static_cast<int64_t>(std::time(nullptr)); },
                 []{ return detail::read_hardware_id(); })
    {}

    Client(Config                   cfg,
           Transport&               transport,
           LicenseStore&            store,
           std::function<int64_t()> now_fn)
        : Client(std::move(cfg), transport, store, std::move(now_fn),
                 []{ return detail::read_hardware_id(); })
    {}

    // Testable constructor — inject a deterministic clock and hardware id.
    // hardware_id_fn() returns the true OS/hardware id, or nullopt when the
    // platform has none.  It must NEVER return a random per-install value.
    Client(Config                                      cfg,
           Transport&                                  transport,
           LicenseStore&                               store,
           std::function<int64_t()>                    now_fn,
           std::function<std::optional<std::string>()> hardware_id_fn)
        : cfg_(std::move(cfg))
        , transport_(transport)
        , store_(store)
        , now_fn_(std::move(now_fn))
        , hardware_id_fn_(std::move(hardware_id_fn))
        , verifier_(cfg_.trustedKeys)
        , state_(State::Invalid)
    {
        refresh_state_from_store_();
    }
```

Declare `hardware_id_fn_` after `now_fn_` in the member list so the
initialisation order matches and `-Wreorder` stays quiet.

- [ ] **Step 4: Persist and restore `cachedHardwareId`**

In `build_blob_locked_()`, after the `freeTierInstanceId` block:

```cpp
        if (cached_hardware_id_.has_value()) {
            append("\"cachedHardwareId\":" + json_str(*cached_hardware_id_));
        }
```

In `refresh_state_from_store_()`:

```cpp
            {
                std::string v = j["cachedHardwareId"].as_string();
                if (!v.empty()) cached_hardware_id_ = v;
            }
```

- [ ] **Step 5: Add the hash helper**

```cpp
    /// The true hardware id: read live, written through to the store on
    /// success, falling back to the last cached value when a live read fails.
    /// Keeps machine_hash stable across transient ioreg/registry failures.
    /// NO random fallback — nullopt means "omit machine_hash".
    std::optional<std::string> cached_hardware_id_value_() {
        std::optional<std::string> live =
            hardware_id_fn_ ? hardware_id_fn_() : std::nullopt;
        if (live.has_value() && !live->empty()) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (cached_hardware_id_ != live) {
                    cached_hardware_id_ = live;
                    changed = true;
                }
            }
            if (changed) (void)save_cache_();
            return live;
        }
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cached_hardware_id_.has_value() && !cached_hardware_id_->empty()) {
            return cached_hardware_id_;
        }
        return std::nullopt;
    }

    /// Cross-SDK machine_hash from the cached hardware id, if any.
    std::optional<std::string> machine_hash_() {
        auto hw = cached_hardware_id_value_();
        if (!hw.has_value()) return std::nullopt;
        return detail::machine_hash(cfg_.tenantId, cfg_.productId, *hw);
    }
```

- [ ] **Step 6: Run the suite**

Run: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: the three new cases fail to link/compile until Task 5 adds
`reportKeylessState`; every pre-existing test passes.

- [ ] **Step 7: Commit**

```bash
git add include/keylight/client.hpp tests/test_free_tier.cpp
git commit -m "feat(free-tier): cache the hardware id and add the injectable seam"
```

---

### Task 5: `reportKeylessState` — the beacon and its debounce

**Files:**
- Modify: `include/keylight/client.hpp` — public API, `build_blob_locked_()`, `refresh_state_from_store_()`
- Modify: `tests/test_free_tier.cpp`

**Interfaces:**
- Consumes: `freeTierInstanceId()` (Task 3), `machine_hash_()` (Task 4), `keyless_state_wire()` (Task 2), `api_url_()`, `json_headers_()`, `build_json_()`
- Produces: `void keylight::Client::reportKeylessState(KeylessState state)`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_free_tier.cpp`:

```cpp
TEST_CASE("Beacon: posts instance_id, state and the SDK key to /keyless") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto hw = []() -> std::optional<std::string> { return std::string("hardware-1"); };

    Client client(cfg, transport, store, [&]{ return now; }, hw);
    const std::string id = client.freeTierInstanceId();
    client.reportKeylessState(KeylessState::FreeTier);

    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->method == "POST");
    CHECK(call->url    == "https://api.keylight.dev/testco/testapp/keyless");
    CHECK(RecordingTransport::header(*call, "X-Keylight-SDK-Key") == SDK_KEY);
    CHECK(call->body.find("\"instance_id\":\"" + id + "\"") != std::string::npos);
    CHECK(call->body.find("\"state\":\"free_tier\"")        != std::string::npos);
    CHECK(call->body.find(CANONICAL_HASH)                   != std::string::npos);
    CHECK(call->body.find("\"sdk\":\"cpp\"")                != std::string::npos);
}

TEST_CASE("Beacon: debounced — same state inside 24h sends nothing") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);
    REQUIRE(transport.calls.size() == 1);

    now = T0 + 23 * 3600;
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 1);       // still debounced
}

TEST_CASE("Beacon: a changed state defeats the debounce immediately") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::Trial);
    REQUIRE(transport.calls.size() == 1);

    now = T0 + 60;
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 2);
}

TEST_CASE("Beacon: the same state sends again once 24h have passed") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);
    REQUIRE(transport.calls.size() == 1);

    now = T0 + DAY + 1;
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 2);
}

TEST_CASE("Beacon: a non-200 response does NOT arm the debounce") {
    // keylight-rust records the debounce only on success, so a failed beacon
    // cannot suppress free-tier reporting for a full day.
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_status = 500;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);
    REQUIRE(transport.calls.size() == 1);
    CHECK_FALSE(store.has_field("keylessLastState"));

    now = T0 + 60;
    transport.next_status = 200;
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 2);
    CHECK(store.str_field("keylessLastState") == "free_tier");
    CHECK(store.int_field("lastKeylessPingAt") == now);
}

TEST_CASE("Beacon: a network error is swallowed and leaves the debounce unarmed") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.fail = true;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);   // must not throw
    CHECK_FALSE(store.has_field("keylessLastState"));
}

TEST_CASE("Beacon: the debounce survives a relaunch") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    {
        Client client(cfg, transport, store, [&]{ return now; }, none);
        client.reportKeylessState(KeylessState::FreeTier);
    }
    REQUIRE(transport.calls.size() == 1);

    now = T0 + 3600;
    Client relaunched(cfg, transport, store, [&]{ return now; }, none);
    relaunched.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 1);   // still debounced across processes
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j8 2>&1 | tail -5`
Expected: FAIL — `no member named 'reportKeylessState' in 'keylight::Client'`

- [ ] **Step 3: Add the debounce cache members**

```cpp
    std::optional<std::string> cached_keyless_last_state_;
    int64_t                    cached_last_keyless_ping_at_ = 0;
```

- [ ] **Step 4: Persist and restore them**

In `build_blob_locked_()`, after the `cachedHardwareId` block:

```cpp
        if (cached_keyless_last_state_.has_value()) {
            append("\"keylessLastState\":" + json_str(*cached_keyless_last_state_));
        }
        if (cached_last_keyless_ping_at_ != 0) {
            append("\"lastKeylessPingAt\":" +
                   std::to_string(cached_last_keyless_ping_at_));
        }
```

In `refresh_state_from_store_()`:

```cpp
            {
                std::string v = j["keylessLastState"].as_string();
                if (!v.empty()) cached_keyless_last_state_ = v;
            }
            cached_last_keyless_ping_at_ = j["lastKeylessPingAt"].as_int();
```

- [ ] **Step 5: Implement the beacon**

In the public section of `Client`:

```cpp
    /// Anonymous keyless/free-tier beacon.  Fire-and-forget: every error is
    /// swallowed, nothing is thrown, and the caller's state is never changed.
    /// Debounced to once per 24h per state — a state *change* always sends.
    ///
    /// Nothing calls this for you.  keylight-rust behaves the same way: the
    /// core never emits network traffic the integrator did not ask for, which
    /// is what keeps checkOnLaunch() free of network I/O during a DAW plugin
    /// scan.  The JUCE adapter wires it to state transitions for you.
    ///
    /// Blocking network call — never invoke it from an audio thread.
    void reportKeylessState(KeylessState state) {
        const std::string wire = keyless_state_wire(state);

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            const bool changed =
                !cached_keyless_last_state_.has_value() ||
                *cached_keyless_last_state_ != wire;
            const bool within_24h =
                cached_last_keyless_ping_at_ != 0 &&
                (now_fn_() - cached_last_keyless_ping_at_) < 86400;
            if (!changed && within_24h) {
                return;
            }
        }

        const std::string instance = freeTierInstanceId();
        if (instance.empty()) {
            return;   // could not persist an id; nothing to report under
        }

        std::vector<std::pair<std::string, std::string>> fields{
            {"instance_id", json_str(instance)},
            {"state",       json_str(wire)},
        };
        if (auto hash = machine_hash_()) {
            fields.push_back({"machine_hash", json_str(*hash)});
        }

        auto hr = transport_.request("POST", api_url_("keyless"),
                                     json_headers_(),
                                     build_json_(std::move(fields), true));
        // Arm the debounce ONLY on a real 200.  A failed beacon must not
        // suppress reporting for a day (keylight-rust does the same).
        if (!hr.is_ok() || hr.value().status != 200) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_keyless_last_state_   = wire;
            cached_last_keyless_ping_at_ = now_fn_();
        }
        (void)save_cache_();
    }
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: PASS, including the three Task 4 hardware-id cases that were waiting on this method.

- [ ] **Step 7: Mutation-check the debounce**

Change `if (!hr.is_ok() || hr.value().status != 200)` to `if (!hr.is_ok())`,
rebuild, and confirm **"Beacon: a non-200 response does NOT arm the debounce"**
fails. Restore it and confirm the suite is green.

- [ ] **Step 8: Commit**

```bash
git add include/keylight/client.hpp tests/test_free_tier.cpp
git commit -m "feat(free-tier): reportKeylessState with a 24h/on-change debounce"
```

---

### Task 6: Conversion-attribution fields on activate and validate

**Files:**
- Modify: `include/keylight/client.hpp` — `activate()` (`:172`), `validate()` (`:253`), `build_validate_body_()` (`:1126`)
- Modify: `tests/test_free_tier.cpp`

**Interfaces:**
- Consumes: `freeTierInstanceId()`, `machine_hash_()`
- Produces: no new public API — only extra body fields.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_free_tier.cpp`:

```cpp
TEST_CASE("Attribution: activate carries free_tier_instance_id and machine_hash") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;
    auto hw = []() -> std::optional<std::string> { return std::string("hardware-1"); };

    Client client(cfg, transport, store, [&]{ return now; }, hw);
    const std::string id = client.freeTierInstanceId();
    REQUIRE(client.activate("KL-TEST-KEY").is_ok());

    const auto* call = transport.last_call_for("activate");
    REQUIRE(call != nullptr);
    CHECK(call->body.find("\"free_tier_instance_id\":\"" + id + "\"") != std::string::npos);
    CHECK(call->body.find(CANONICAL_HASH) != std::string::npos);
}

TEST_CASE("Attribution: activate omits free_tier_instance_id when none was minted") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    REQUIRE(client.activate("KL-TEST-KEY").is_ok());

    const auto* call = transport.last_call_for("activate");
    REQUIRE(call != nullptr);
    CHECK(call->body.find("free_tier_instance_id") == std::string::npos);
}

TEST_CASE("Attribution: validate carries machine_hash") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;
    auto hw = []() -> std::optional<std::string> { return std::string("hardware-1"); };

    Client client(cfg, transport, store, [&]{ return now; }, hw);
    REQUIRE(client.activate("KL-TEST-KEY").is_ok());
    REQUIRE(client.validate().is_ok());

    const auto* call = transport.last_call_for("validate");
    REQUIRE(call != nullptr);
    CHECK(call->body.find(CANONICAL_HASH) != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j8 && ./build/keylight_tests -tc="Attribution*"`
Expected: FAIL — the bodies contain neither field.

- [ ] **Step 3: Add a small body helper**

```cpp
    /// Append the anonymous free-tier id (only if one already exists — never
    /// mint one here) and machine_hash to an outgoing body.  Mirrors
    /// keylight-rust, which attaches both to activate and machine_hash to
    /// validate, so a device that converts from free tier to paid is counted
    /// once rather than twice.
    void append_attribution_fields_(
        std::vector<std::pair<std::string, std::string>>& fields,
        bool include_instance_id)
    {
        if (include_instance_id) {
            std::optional<std::string> id;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                id = cached_free_tier_instance_id_;
            }
            if (id.has_value() && !id->empty()) {
                fields.push_back({"free_tier_instance_id", json_str(*id)});
            }
        }
        if (auto hash = machine_hash_()) {
            fields.push_back({"machine_hash", json_str(*hash)});
        }
    }
```

- [ ] **Step 4: Use it in activate**

Change `activate()` so the field vector is built, extended, then encoded:

Replace the existing `build_json_({...}, true)` call at `client.hpp:172-175`
with a field vector that can be extended. The two existing fields are kept
exactly as they are — `instance_name` is the literal `"device"` today, not a
detected machine name:

```cpp
        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key",   json_str(key)},
            {"instance_name", json_str("device")},
        };
        append_attribution_fields_(fields, /*include_instance_id=*/true);
        std::string body = build_json_(std::move(fields), true);
```

- [ ] **Step 5: Use it in validate**

`validate()`, the launch reconciliation and the auto-validation tick all go
through `build_validate_body_()` (`client.hpp:1125`), so changing that one
helper covers every path. It is currently `const`; `machine_hash_()` writes the
cached hardware id through to the store, so **drop the `const`**:

```cpp
    /// Build the JSON body for a validate request.
    std::string build_validate_body_() {
        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key", json_str(load_license_key_())},
            {"instance_id", json_str(load_instance_id_())},
        };
        append_attribution_fields_(fields, /*include_instance_id=*/false);
        return build_json_(std::move(fields), true);
    }
```

Dropping `const` will cascade to any `const` caller. Follow the compiler until
it is quiet; do not add `mutable` to work around it.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: PASS. The `Auth:` cases from PR #3 must still pass — the header is untouched.

- [ ] **Step 7: Commit**

```bash
git add include/keylight/client.hpp tests/test_free_tier.cpp
git commit -m "feat(free-tier): attribute conversions on activate and validate"
```

---

### Task 7: JUCE adapter

**Files:**
- Modify: `integrations/juce/KeylightJuce.h`
- Modify: `integrations/juce/citest/main.cpp`
- Modify: `integrations/juce/README.md`

**Interfaces:**
- Consumes: `Client::reportKeylessState`, `keylight::KeylessState`, `State::FreeTier`
- Produces: `Licensing::reportKeylessState(keylight::KeylessState, std::function<void()> = {})`

- [ ] **Step 1: Add the method**

In `integrations/juce/KeylightJuce.h`, directly after `startTrial`:

```cpp
    // -----------------------------------------------------------------------
    // reportKeylessState — anonymous free-tier/trial beacon.  Blocking network
    // call, so it runs on the same background dispatch as every other
    // licensing call; never call it from processBlock.
    //
    // You normally do not call this at all: the adapter fires it for you on
    // every state transition (see notify_()).  keylight::Client itself never
    // does, which is what keeps checkOnLaunch() free of network I/O when a DAW
    // scans the plugin.
    // -----------------------------------------------------------------------
    void reportKeylessState(keylight::KeylessState state,
                            std::function<void()> callback = {})
    {
        dispatch_([this, state, cb = std::move(callback)]()
        {
            client_->reportKeylessState(state);
            if (cb) juce::MessageManager::callAsync([cb]() { cb(); });
        });
    }
```

- [ ] **Step 2: Auto-report on transitions**

Find the adapter's state-change fan-out (the handler wired to
`client_->subscribe(...)` that drives `onStateChanged`). At its end, map the new
state onto a keyless report and dispatch it. Only the three keyless states are
reportable — a `Licensed` device is no longer keyless:

```cpp
        // Swift's LicenseManager reports the funnel automatically; the C++ core
        // deliberately does not, so the adapter does it here — off both the
        // message and audio threads.  Licensed/Invalid are not keyless states.
        switch (s)
        {
            case keylight::State::Trial:
                reportKeylessState(keylight::KeylessState::Trial);    break;
            case keylight::State::FreeTier:
                reportKeylessState(keylight::KeylessState::FreeTier); break;
            case keylight::State::Expired:
                reportKeylessState(keylight::KeylessState::Expired);  break;
            default:
                break;
        }
```

The SDK's own 24h/on-change debounce makes this safe to call on every
transition — no extra guard is needed here.

- [ ] **Step 3: Extend the CI smoke test**

In `integrations/juce/citest/main.cpp`, alongside the existing trial coverage,
add a compile+link check and a real SDK-level assertion:

```cpp
    // Compile+link check: the adapter method completes via callAsync and this
    // headless console app runs no message loop, so only the SDK call below is
    // exercised for real.
    licensing.reportKeylessState(keylight::KeylessState::FreeTier);

    // Exercise the underlying SDK call directly.
    client.reportKeylessState(keylight::KeylessState::FreeTier);
    std::cout << "keyless beacon ok\n";
```

- [ ] **Step 4: Build and run the JUCE smoke test**

Run the same command `.github/workflows/juce.yml` uses for the citest target.
Expected: builds and prints `keyless beacon ok`.

- [ ] **Step 5: Document it**

In `integrations/juce/README.md`, add a **Free tier** section: set
`cfg.freeTierEnabled = true`, handle `keylight::State::FreeTier` next to
`State::Trial` in the state switch, note that the beacon is automatic and
debounced, and note that `reportKeylessState` is background-dispatched while
`state()` stays audio-thread safe.

- [ ] **Step 6: Commit**

```bash
git add integrations/juce/
git commit -m "feat(juce): free-tier state and the automatic keyless beacon"
```

---

### Task 8: Amalgamation, version bump and docs

**Files:**
- Modify: `include/keylight/version.hpp`, `vcpkg.json`, `conanfile.py`
- Modify: `README.md`, `CHANGELOG.md`
- Modify: `tests/test_amalgamation.cpp`
- Regenerate: `keylight_single.hpp`

- [ ] **Step 1: Add an amalgamation test case**

`tools/amalgamate.py` walks `#include` directives from `keylight/keylight.hpp`
topologically, so `machine_id.hpp` is picked up automatically once `client.hpp`
includes it — no script change needed. Prove the shipped header actually works
by appending to `tests/test_amalgamation.cpp`:

```cpp
TEST_CASE("amalgamation: free tier resolves and the beacon posts") {
    Config cfg;
    cfg.tenantId        = "testco";
    cfg.productId       = "testapp";
    cfg.sdkKey          = "sdk_live_test";
    cfg.freeTierEnabled = true;

    RecordingTransport transport;
    MemStore           store;
    int64_t now = 1781076256LL;
    auto hw = []() -> std::optional<std::string> { return std::string("hardware-1"); };

    Client client(cfg, transport, store, [&]{ return now; }, hw);
    CHECK(client.checkOnLaunch().value() == State::FreeTier);

    client.reportKeylessState(KeylessState::FreeTier);
    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->body.find(
        "8e8871112f28cabda180ada131d0b4f4f07c72fb47c5d884edbe32812885b22a")
        != std::string::npos);
}
```

Reuse whatever transport/store fakes `test_amalgamation.cpp` already defines
rather than adding new ones.

- [ ] **Step 2: Regenerate the single header**

Run: `python3 tools/amalgamate.py`
Then: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: PASS, `test_amalgamation` included.

- [ ] **Step 3: Bump the version to 0.1.6**

- `include/keylight/version.hpp`: `#define KEYLIGHT_SDK_VERSION "0.1.6"`
- `vcpkg.json`: `"version": "0.1.6"`
- `conanfile.py`: `version = "0.1.6"`
- Re-run `python3 tools/amalgamate.py` so the embedded version follows.

- [ ] **Step 4: Update the README**

- `GIT_TAG v0.1.5` → `v0.1.6`; `conan install keylight/0.1.5@` → `0.1.6`.
- New **Free tier** section: `freeTierEnabled`, `State::FreeTier`,
  `reportKeylessState`, `freeTierInstanceId`, the 24h debounce, and the fact
  that the core never fires the beacon on its own.
- State table: add `FreeTier`.
- Note the new system-framework linkage (IOKit/CoreFoundation on Apple,
  advapi32 on Windows) and correct any remaining "zero dependencies" wording.
- State plainly that `machine_hash` is a one-way hash of an OS machine ID, is
  sent only on the keyless/activate/validate routes, and is omitted entirely
  when no hardware ID is available.

- [ ] **Step 5: Update the CHANGELOG**

Add a `## [0.1.6]` entry. Under **Added**: free tier, the beacon, `machine_hash`,
`freeTierInstanceId`. Under **Changed**, call out the two behavior changes
explicitly:

- `State::FreeTier` is a new enum value — exhaustive `switch` statements over
  `State` will warn until a case is added. It is appended last, so existing
  enumerator values are unchanged.
- With `freeTierEnabled = true`, an **elapsed** trial now resolves `FreeTier`
  instead of `Expired`. Products that leave the flag `false` are unaffected.

- [ ] **Step 6: Full verification**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
cmake -S . -B build-httplib -DKEYLIGHT_BUILD_HTTPLIB_TRANSPORT=ON -DKEYLIGHT_BUILD_DEMO=ON
cmake --build build-httplib -j8 && ctest --test-dir build-httplib --output-on-failure -E test_live
```

Expected: both green. Record the actual case/assertion counts in the PR body —
do not estimate them.

- [ ] **Step 7: Commit and open the PR**

```bash
git add -A
git commit -m "chore(release): 0.1.6 — free tier and the keyless beacon"
git push -u origin claude/keylight-cpp-free-tier-keyless
gh pr create --base claude/keylight-cpp-auth-trial-m2kizb \
  --title "feat(free-tier): State::FreeTier, keyless beacon and machine_hash"
```

Base the PR on the PR #3 branch, **not** `main`, so the diff shows only this work.

---

## Self-Review

**Spec coverage**

| Spec section | Task |
|---|---|
| `machine_id.hpp`, hardware ID per platform | 1 |
| `machine_hash` + canonical vector | 1 |
| WOW64 registry view (D6) | 1, Step 3 |
| Link dependencies (D7) | 1, Step 4 |
| `Config::freeTierEnabled` | 2 |
| `State::FreeTier`, appended | 2 |
| `KeylessState` + wire strings | 2 |
| State resolution incl. D5 | 2 |
| `deactivate()` → `FreeTier` | 2 (test only; falls out of the resolver) |
| Free-tier instance ID + UUIDv4 | 1 (uuid), 3 |
| `startTrial()` mints the ID | 3 |
| Hardware-ID caching / flaky reads | 4 |
| Test seam constructor | 4 |
| `reportKeylessState` + debounce + 200-gate | 5 |
| Attribution on activate/validate | 6 |
| JUCE adapter + auto-report (D3) | 7 |
| Amalgamation, version, README, CHANGELOG | 8 |

No spec requirement is unassigned.

**Known ordering wrinkle:** Task 4's three tests call `reportKeylessState`, which
Task 5 implements. Task 4 Step 1 flags this and gives both orderings. This is
deliberate — those tests belong to the caching behavior, not the beacon.

**Type consistency:** `freeTierInstanceId()`, `reportKeylessState()`,
`machine_hash_()`, `cached_hardware_id_value_()`, `append_attribution_fields_()`,
`keyless_state_wire()`, `detail::read_hardware_id()`, `detail::machine_hash()`,
`detail::uuid_v4()` — each is defined once and referenced consistently.
Persisted keys are `freeTierInstanceId`, `cachedHardwareId`, `keylessLastState`,
`lastKeylessPingAt` throughout.

**Note on `machine_hash_()` constness:** it writes through to the store, so it is
non-`const`. Any caller that is currently `const` (notably `build_validate_body_()`)
must drop `const`. Task 6 Step 5 says so explicitly.
