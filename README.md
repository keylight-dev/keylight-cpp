# Keylight C++ SDK

[![CI](https://github.com/keylight-dev/keylight-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/keylight-dev/keylight-cpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](#requirements)
[![Conformance](https://img.shields.io/badge/conformance-cross--SDK%20vectors-success.svg)](#conformance)

Open-source C++ SDK for [Keylight](https://keylight.dev) — license your native apps, game engine
plugins, and audio tools with online activation and offline Ed25519 license verification.

> **In one line:** a software-licensing SDK for C++ — license-key activation and validation,
> entitlement/feature gating, trials, and tamper-resistant **offline license verification** (signed
> `v3` lease, Ed25519 + clock-skew tolerance) for desktop apps, Unreal Engine 5 plugins, and
> JUCE audio applications. Header-only core, C++17, no mandatory external dependencies.

## Why Keylight

Licensing shouldn't mean bolting a heavyweight, phone-home-or-die SDK onto your app.

- **Works offline.** The license is a signed lease your app verifies locally with Ed25519 — no
  network round-trip to gate a feature, no lockout when the machine is offline.
- **Tamper-resistant by design.** Entitlements live *inside* the signature; a forged or hand-edited
  lease can't pass verification without the tenant's private key.
- **Audio-thread safe.** `state()` reads a `std::atomic` — safe to call from JUCE audio callbacks
  or game-thread hot paths with no lock taken.
- **One SDK family.** Verifies licenses identically to the Swift, Rust, JavaScript, and C# SDKs,
  proven by shared conformance vectors.
- **Header-only core.** Drop in `keylight_single.hpp` or use CMake FetchContent — zero mandatory
  external dependencies for the verifier and state machine.

## Table of Contents

- [Why Keylight](#why-keylight)
- [Install](#install)
  - [CMake FetchContent (recommended)](#cmake-fetchcontent-recommended)
  - [Single-header drop-in](#single-header-drop-in)
  - [vcpkg](#vcpkg)
  - [Conan](#conan)
- [Quick Start](#quick-start)
- [Unreal Engine](#unreal-engine)
- [JUCE](#juce)
- [License Lifecycle](#license-lifecycle)
- [License States](#license-states)
- [Entitlements](#entitlements)
- [Offline & Security](#offline--security)
- [Configuration Reference](#configuration-reference)
- [Cross-SDK Conformance Vectors](#cross-sdk-conformance-vectors)
- [Documentation](#documentation)
- [Other SDKs](#other-sdks)
- [License](#license)

## Install

### CMake FetchContent (recommended)

```cmake
include(FetchContent)
FetchContent_Declare(
  keylight
  GIT_REPOSITORY https://github.com/keylight-dev/keylight-cpp.git
  GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(keylight)

target_link_libraries(my_app PRIVATE keylight::keylight)
```

The core library (`keylight::keylight`) is interface-only — **no external dependencies**. To enable
the bundled [cpp-httplib](https://github.com/yhirose/cpp-httplib) transport (requires OpenSSL):

```cmake
FetchContent_Declare(keylight ...)
set(KEYLIGHT_BUILD_HTTPLIB_TRANSPORT ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(keylight)

target_link_libraries(my_app PRIVATE keylight::keylight keylight::httplib_transport)
```

### Single-header drop-in

For projects that don't use CMake, copy [`keylight_single.hpp`](keylight_single.hpp) (the
pre-generated amalgamation of all core headers) into your source tree and `#include` it directly.
No build system changes needed; the core verifier and state machine are entirely self-contained.

```cpp
#include "keylight_single.hpp"
```

To include the optional httplib transport alongside it, also copy
`include/keylight/transport/httplib.hpp` (which itself needs OpenSSL link flags).

### vcpkg

```bash
vcpkg install keylight
# with the optional httplib transport:
vcpkg install "keylight[httplib-transport]"
```

> vcpkg port submission is planned for a future release. Until then, use FetchContent or
> the single-header drop-in.

### Conan

```bash
conan install keylight/0.1.0@
```

> Conan Center submission is planned for a future release.

## Quick Start

```cpp
#include <keylight/keylight.hpp>
#include <keylight/keyset.hpp>
#include <keylight/store.hpp>
#include <keylight/transport/httplib.hpp>  // opt-in; requires OpenSSL

int main() {
    // 1. Build a Config with your tenant/product credentials.
    keylight::Config cfg;
    cfg.tenantId      = "your-tenant";
    cfg.productId     = "your-product";
    cfg.sdkKey        = "sdk_live_...";
    cfg.maxOfflineDays = 7;  // optional offline grace window

    // 2. Fetch the tenant's trusted Ed25519 keyset so leases verify offline.
    //    (You can also pin keys explicitly via cfg.trustedKeys["kid"] = base64_pub.)
    keylight::HttplibTransport transport;
    auto ks = keylight::fetchKeyset(transport, cfg.apiBaseUrl, cfg.tenantId);
    if (ks.is_ok()) {
        cfg.trustedKeys = ks.value();
    }

    // 3. Create a FileStore (persists the verified lease between launches).
    keylight::FileStore store(keylight::default_store_path(cfg));

    // 4. Construct the Client — primes state from the persisted store immediately.
    keylight::Client client(cfg, transport, store);

    // 5. On launch: refresh if the cached lease is stale (debounced, non-blocking).
    client.checkOnLaunch();

    // 6. Activate a license key (online). The returned lease is Ed25519-verified
    //    *before* anything is persisted.
    auto res = client.activate("USER-LICENSE-KEY");
    if (res.is_ok() && res.value() == keylight::State::Licensed) {
        // seat locked to this device
    }

    // 7. Gate features on entitlements — reads std::atomic, audio-thread safe.
    if (client.hasEntitlement("pro")) {
        // unlock pro features
    }

    // 8. Current high-level state (no network call).
    keylight::State s = client.state();

    // 9. Release the seat on uninstall / device switch.
    client.deactivate();
}
```

> **No background threads by default.** Call `checkOnLaunch()` on startup and
> `refreshIfNeeded()` on meaningful events (window focus, purchase, resume). The state machine
> applies a 5-minute debounce and refreshes automatically when the cached lease is stale or
> within 24 hours of expiry. An optional background thread is available via
> `client.startAutoValidation()` for daemon or headless applications.

## Unreal Engine

An Unreal Engine 5 plugin lives in [`integrations/unreal/Keylight/`](integrations/unreal/Keylight/).
It provides:

- **`UKeylightSubsystem`** — a `UGameInstanceSubsystem` with Blueprint-callable
  `Activate` / `Validate` / `Deactivate` / `HasEntitlement` / `GetState` methods and
  `FOnKeylightResult` async delegates.
- **`FHttpTransport`** — a `keylight::Transport` adapter over UE's `FHttpModule` that blocks a
  background thread via `FEvent`, never the game thread.
- **`UELicenseStore`** — persists the lease under `Saved/Keylight/` in the project directory.

The plugin depends only on `Core`, `CoreUObject`, `Engine`, `HTTP`, and `Json` — no extra
dependencies beyond what ships with Unreal Engine.

> **Manual build required.** No UE toolchain is available in CI. A developer with UE 5.x installed
> must build the plugin and smoke-test before shipping. See
> [`integrations/unreal/README.md`](integrations/unreal/README.md).

## JUCE

A JUCE adapter lives in [`integrations/juce/`](integrations/juce/) and provides:

- **`keylight::juce_integration::JuceUrlTransport`** — a `keylight::Transport` adapter over
  `juce::URL::createInputStream` with no OpenSSL dependency and no cpp-httplib.
- **`keylight::juce_integration::Licensing`** — owns the `Client`, `FileStore`, and transport;
  exposes `activate` / `validate` / `deactivate` / `checkOnLaunch` with message-thread callbacks
  via `juce::MessageManager::callAsync`. `state()` and `hasFeature()` read a `std::atomic`
  snapshot — safe to call from the audio thread.

Compiles against JUCE 7 and JUCE 8 with zero extra dependencies beyond `juce_core`.

> **Manual build required.** No JUCE toolchain is available in CI. A developer with JUCE 7 or 8
> installed must compile and smoke-test before shipping. See
> [`integrations/juce/README.md`](integrations/juce/README.md).

## License Lifecycle

```
┌─────────────┐     ┌─────────────┐     ┌──────────────┐
│  activate   │────▶│  validate   │────▶│  deactivate  │
└─────────────┘     └─────────────┘     └──────────────┘
                          ▲
                          │ on launch / on events (no background threads by default)
                  ┌─────────────────────┐
                  │   refreshIfNeeded   │
                  └─────────────────────┘
```

| Method | Description |
|--------|-------------|
| `activate(key) → Result<State>` | Activates a key on this device. Verifies the returned lease before persisting. |
| `validate() → Result<State>` | Re-checks the stored license online. Network failures are non-fatal (grace window applies). |
| `deactivate() → Result<void>` | Releases the seat and clears local license state, even if the network call fails. |
| `refreshIfNeeded() → Result<State>` | Validates only if due (debounce 5 min, stale 6 h, within 24 h of expiry). Safe to call often. |
| `checkOnLaunch() → Result<State>` | Convenience: refresh if a license is stored, else no-op. |

## License States

`state()` resolves a single high-level status from the cached, Ed25519-verified lease (no network
call). It reads a `std::atomic<State>` and is safe to call from any thread.

| State | Meaning |
|-------|---------|
| `Licensed` | Current, signature-valid `active` lease. |
| `Trial` | No license, but a local trial is active. |
| `Expired` | Trusted lease expired, or lease status is `"fallback"` / `"expired"`. |
| `Invalid` | No trusted lease and no active trial. |

```cpp
switch (client.state()) {
    case keylight::State::Licensed: /* full access */    break;
    case keylight::State::Trial:    /* show trial UI */  break;
    case keylight::State::Expired:
    case keylight::State::Invalid:  /* prompt activate */ break;
}
```

## Entitlements

Entitlements are feature keys carried inside the signed lease and checked offline:

```cpp
if (client.hasEntitlement("cloud-sync")) {
    enableCloudSync();
}
```

`hasEntitlement` returns `true` only when the cached lease is signature-valid, unexpired, and not
`expired`-status — so offline feature gating never disagrees with the resolved `Expired` state.

## Offline & Security

The offline artifact is a signed **`v3` lease** issued by the Keylight API. The SDK reconstructs
the exact signed payload (entitlements sorted, pipe-delimited) and verifies it with **Ed25519**
against the tenant's trusted keyset, applying a **300-second clock-skew** tolerance.

- The trusted keyset is fetched once from `GET /{tenant}/.well-known/keylight-keys`
  (`fetchKeyset`) or pinned at build time via `cfg.trustedKeys["kid"] = base64_pub`.
- `hasEntitlement` and `state()` only read from the in-memory verified-lease cache — no
  network call, no disk I/O, safe from the audio thread.
- The on-disk lease file is a JSON blob. **The security boundary is the Ed25519 signature**, not
  at-rest encryption — a tampered or forged lease cannot pass verification without the tenant's
  private key.

## Configuration Reference

Populate a `keylight::Config` struct:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `tenantId` | `std::string` | — | Your Keylight tenant (required). |
| `productId` | `std::string` | — | Your product (required). |
| `sdkKey` | `std::string` | — | Tenant SDK key (sent as `X-Keylight-SDK-Key`). |
| `trustedKeys` | `map<string,string>` | empty | Trusted Ed25519 public keys (`kid → base64`) for offline verification. |
| `maxOfflineDays` | `int` | `7` | Offline grace window since last online validation. Set `0` to run offline as long as the lease itself is current. |
| `keyPrefix` | `std::string` | — | Client-side key-format check (e.g. `"PROD"`). |
| `trialDurationDays` | `int` | `0` | Local trial length in days (0 = trials disabled). |
| `apiBaseUrl` | `std::string` | `https://api.keylight.dev` | Keylight API base URL. |
| `appVersion` | `std::string` | — | Reported in activation/validation telemetry. |
| `autoValidationIntervalMs` | `int` | `1800000` | Background auto-validation interval (ms); used only when `startAutoValidation()` is called. |

## Cross-SDK Conformance Vectors

The security-critical lease verifier is gated by Keylight's frozen **cross-SDK conformance
vectors** (`tests/test_conformance.cpp`). The C++ verifier must agree with every vector on
`{ kid_known, signature_valid, expired }`, which keeps offline verification behavior
byte-identical across the Keylight SDK family (Swift, Rust, JavaScript, C#, C++).

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

The conformance suite runs as part of the CI matrix on Ubuntu, macOS, and Windows.

## Requirements

- **C++17** or later
- Supported compilers: GCC 9+, Clang 10+, MSVC 2019+
- Supported platforms: Linux, macOS, Windows (all CI-tested)
- The core library has **zero runtime dependencies** — the opt-in httplib transport adds a
  dependency on [cpp-httplib](https://github.com/yhirose/cpp-httplib) and OpenSSL

## Documentation

- **Platform docs:** [docs.keylight.dev](https://docs.keylight.dev)
- **Website:** [keylight.dev](https://keylight.dev)
- **API host:** `https://api.keylight.dev`

## Other SDKs

| Platform | Status | Repository |
|----------|--------|------------|
| Swift (macOS/iOS) | Available | [keylight-swift](https://github.com/keylight-dev/keylight-swift) |
| Rust (CLIs/daemons/Tauri) | Available | [keylight-rust](https://github.com/keylight-dev/keylight-rust) |
| JavaScript/TypeScript | Available | [keylight-js](https://github.com/keylight-dev/keylight-js) |
| C# (.NET/Godot/Unity) | Available | [keylight-csharp](https://github.com/keylight-dev/keylight-csharp) |
| C++ (this repo) | Available | [keylight-cpp](https://github.com/keylight-dev/keylight-cpp) |

## License

MIT License. See [LICENSE](LICENSE) for details.

---

<sub>Keylight C++ SDK — software licensing for C++: license-key activation & validation, offline
Ed25519 lease verification, entitlement/feature gating, trials, and pluggable transport/storage —
for desktop apps, Unreal Engine 5 plugins, and JUCE audio applications.</sub>
