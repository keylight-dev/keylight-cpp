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
> JUCE audio applications. Header-only core, C++17, no third-party dependencies.

## Why Keylight

Licensing shouldn't mean bolting a heavyweight, phone-home-or-die SDK onto your app.

- **Works offline.** The license is a signed lease your app verifies locally with Ed25519 — no
  network round-trip to gate a feature, no lockout when the machine is offline.
- **Tamper-resistant by design.** Entitlements live *inside* the signature; a forged or hand-edited
  lease can't pass verification without the tenant's private key.
- **Audio-thread safe.** `state()` reads atomics only — safe to call from JUCE audio callbacks
  or game-thread hot paths with no lock taken and nothing allocated.
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
- [Trials](#trials)
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
  GIT_TAG        v0.1.6
)
FetchContent_MakeAvailable(keylight)

target_link_libraries(my_app PRIVATE keylight::keylight)
```

The core library (`keylight::keylight`) is interface-only and pulls in **no third-party
dependencies**. It does link two system frameworks so it can read the OS machine identifier for
free-tier device de-duplication: **IOKit + CoreFoundation** on macOS and **advapi32** on Windows
(nothing on Linux). CMake adds these for you. If you use the
[single header](#single-header-drop-in) you must add that link line yourself. To enable
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
conan install keylight/0.1.6@
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
    cfg.tenantId          = "your-tenant";
    cfg.productId         = "your-product";
    cfg.sdkKey            = "sdk_live_...";  // sent as X-Keylight-SDK-Key on every call
    cfg.trialDurationDays = 14;              // local trial length (0 = trials disabled)
    cfg.maxOfflineDays    = 7;               // optional offline grace window

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

    // 5. On launch: revalidate a stored license, or resolve an already-started
    //    local trial offline. Never starts a trial by itself.
    client.checkOnLaunch();

    // 5b. Start the local trial — only when the user asks for one.
    if (client.checkTrial() == keylight::TrialStatus::NotStarted) {
        client.startTrial();   // → State::Trial for the next 14 days
    }

    // 6. Activate a license key (online). The returned lease is Ed25519-verified
    //    *before* anything is persisted.
    auto res = client.activate("USER-LICENSE-KEY");
    if (res.is_ok() && res.value() == keylight::State::Licensed) {
        // seat locked to this device
    }

    // 7. Gate features on entitlements — mutex-guarded, NOT audio-thread safe.
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
> `client.startAutoValidation()` for daemon or headless applications. Both it and
> `stopAutoValidation()` are safe from any thread, including from a state-change
> listener, and neither blocks. Note `stopAutoValidation()` retires the worker rather
> than waiting for it: one more tick can land after it returns. `~Client()` joins.

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
  exposes `activate` / `validate` / `deactivate` / `checkOnLaunch` / `startTrial` with
  message-thread callbacks via `juce::MessageManager::callAsync`. `state()` and `hasFeature()` read a `std::atomic`
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
| `deactivate() → Result<void>` | Releases the seat and clears local license state. The local cache is cleared either way, but a server rejection is returned as an error: the seat is still consumed and only you can decide to retry. |
| `refreshIfNeeded() → Result<State>` | Validates only if due (debounce 5 min, stale 6 h, within 24 h of expiry). With no stored license it re-resolves the local trial offline. Safe to call often. |
| `checkOnLaunch() → Result<State>` | Revalidates a stored license; otherwise resolves the persisted local trial offline. Never starts a trial. |
| `startTrial() → Result<State>` | Explicitly begins the local trial (idempotent; never restarts one). No network call. |
| `checkTrial() → TrialStatus` | `NotStarted` / `Active` / `Expired` for the local trial. |
| `trialDaysLeft() → int` | Whole days left in the local trial (0 when disabled, not started, or elapsed). |

## License States

`state()` resolves a single high-level status from the cached, Ed25519-verified lease (no network
call). It reads atomics only and is safe to call from any thread.

| State | Meaning |
|-------|---------|
| `Licensed` | Current, signature-valid `active` lease. |
| `Trial` | No license, but a local trial is active. |
| `Limited` | Trusted lease with status `"fallback"`: the server could not mint a full lease, so run degraded rather than locked. |
| `Expired` | Trusted lease expired, or lease status is `"expired"`. |
| `Invalid` | No trusted lease, no active trial, and no free tier. Also what `state()` reports when the system clock has been rolled back more than an hour since the last server contact. |
| `FreeTier` | No license and no active trial, but `freeTierEnabled` is set. Also where an **elapsed** trial and a `deactivate()` land. |

```cpp
switch (client.state()) {
    case keylight::State::Licensed: /* full access */      break;
    case keylight::State::Trial:    /* show trial UI */    break;
    case keylight::State::Limited:  /* degraded, not locked */ break;
    case keylight::State::FreeTier: /* reduced features */ break;
    case keylight::State::Expired:
    case keylight::State::Invalid:  /* prompt activate */  break;
}
```

`state()` is `noexcept` and audio-thread safe, and it calls the clock function you pass to the
`Client` constructor. If you supply your own, it must be non-throwing, non-blocking and
allocation-free — an exception escaping it is `std::terminate`, on whichever thread called
`state()`. The default (`std::time`) already satisfies this.

Subscribers registered with `subscribe()` receive the same value `state()` would return, so a
paywall driven by events and one driven by the query API cannot disagree. Events are delivered
in order, with no SDK lock held — your callback may take your own locks and may call back into
the `Client`; a re-entrant call queues its event rather than recursing, so it may be delivered
by a different thread. Two things not to do: destroy the `Client` from a callback, and throw
from one — an exception has nowhere to go, so it is caught and swallowed and the remaining
listeners still get the event. Note also that `unsubscribe()` does not fence a delivery already
in flight on another thread; keep whatever your callback captures alive across that window.

The clock-rollback guard raises its own event in both directions — once when the rollback is
detected, and again when the clock becomes honest. Because a moving clock changes no underlying
state, that event comes from `refreshIfNeeded()` or `validate()`. Call `startAutoValidation()`
(or one of those on focus/resume) in a long-running host, or a mid-session rollback will reach
`state()` and never reach your callback.

## Trials

Trials are **local and offline-first**. `startTrial()` persists a start timestamp
next to the lease; the window is then measured against the local clock with no
API call at all. (The free-tier / keyless reporting feature is separate — trial
validity never depends on it.)

```cpp
keylight::Config cfg;
cfg.tenantId          = "your-tenant";
cfg.productId         = "your-product";
cfg.sdkKey            = "sdk_live_...";
cfg.trialDurationDays = 14;      // 0 (the default) disables trials entirely

keylight::Client client(cfg, transport, store);

// Explicit — nothing starts a trial implicitly.
client.startTrial();             // → State::Trial

client.checkTrial();             // TrialStatus::Active
client.trialDaysLeft();          // 14

// On the next launch: resolves the persisted trial with no network call.
client.checkOnLaunch();          // → State::Trial (or Expired once elapsed)
```

Rules the state machine guarantees:

- **`trialDurationDays <= 0` disables trials.** Nothing is persisted and
  `checkTrial()` stays `NotStarted`.
- **`checkOnLaunch()` never starts a trial.** It only resolves one the user
  already started — important for JUCE plugins, since a DAW may scan or
  instantiate a plugin without the user ever asking for a trial.
- **`startTrial()` is idempotent.** An existing start timestamp is never
  overwritten, so an elapsed trial cannot be restarted.
- **A running trial elapses on its own.** `state()` reads a cached snapshot, so
  it never recomputes; `checkOnLaunch()` and `refreshIfNeeded()` re-resolve the
  trial (offline, no request) and notify subscribers, and
  `startAutoValidation()` ticks the latter for long-running hosts.
- **Paid licensing always wins.** Activating during a trial resolves
  `Licensed`; deactivating later returns to whatever the *original* trial has
  become (`Trial` if still running, `Expired` if it elapsed meanwhile,
  `Invalid` if there never was one). Deactivation does not reset the trial clock.

- **`checkTrial()` and `trialDaysLeft()` do not apply the clock-rollback guard.**
  They are local arithmetic over the persisted start timestamp, so on a rolled-back
  clock `checkTrial()` can report `Active` while `state()` reports `Invalid`. Gate
  access on `state()`; use these two for display.

State priority: valid paid license → active local trial → elapsed local trial →
`Invalid`.

> **Not tamper-proof.** The trial start lives in the same on-disk JSON blob as
> the lease. Clearing that file (or a fresh install on a clean machine) starts
> the user over — the store makes no reinstall-proof or anti-tamper claim. The
> Ed25519 signature is the security boundary for *paid* licenses; the local
> trial is a convenience, and a backwards clock jump is clamped rather than
> credited.

## Free Tier

Set `freeTierEnabled` and a device with no license and no active trial resolves
`State::FreeTier` rather than `Invalid`:

```cpp
cfg.freeTierEnabled = true;
```

Resolution order is: valid paid license → active trial → free tier → elapsed trial → `Invalid`.
Two consequences, both matching the Rust and Swift SDKs:

- An **elapsed** trial resolves `FreeTier`, not `Expired` — a lapsed trial drops to the free tier
  rather than the paywall.
- `deactivate()` lands on `FreeTier` for the same reason. Releasing a paid seat returns the user to
  the tier they are still entitled to.

### The keyless beacon

`reportKeylessState()` sends an anonymous funnel signal so Keylight can show *trials started →
converted / in free tier / expired*:

```cpp
client.reportKeylessState(keylight::KeylessState::FreeTier);  // or ::Trial / ::Expired
```

- **Nothing calls it for you.** `checkOnLaunch()` still makes no network request when no license is
  stored, so a DAW scanning your plugin does not phone home. The
  [JUCE adapter](integrations/juce/README.md) opts in on your behalf and reports on every state
  transition; the core never does.
- **Debounced** to one request per 24 hours per state; a state *change* always sends. The debounce
  is recorded only on HTTP 200, so a failed beacon retries instead of going quiet for a day.
- **Fire-and-forget.** Errors are swallowed, nothing is thrown, and the resolved state is unchanged.
- **Blocking** — never call it from an audio thread.

What it sends: a random per-install id (`freeTierInstanceId()`, persisted alongside the lease), the
state string, and — only where the OS exposes a stable machine identifier — `machine_hash`, a
one-way SHA-256 of it. Never a license key, never a raw device id. Where no such identifier exists
the field is omitted entirely rather than substituting a random value.

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
| `sdkKey` | `std::string` | — | Tenant SDK key, sent as `X-Keylight-SDK-Key` on every API call. Required — the API answers `401` without it. |
| `trustedKeys` | `map<string,string>` | empty | Trusted Ed25519 public keys (`kid → base64`) for offline verification. |
| `maxOfflineDays` | `int` | `7` | Offline grace window since last online validation. Set `0` to run offline as long as the lease itself is current. |
| `keyPrefix` | `std::string` | — | Client-side key-format check (e.g. `"PROD"`). |
| `trialDurationDays` | `int` | `0` | Local trial length in days (0 = trials disabled). See [Trials](#trials). |
| `freeTierEnabled` | `bool` | `false` | Resolve `State::FreeTier` instead of `Invalid`/`Expired` when there is no license and no active trial. See [Free Tier](#free-tier). |
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

## About Keylight

Keylight is the licensing layer for desktop apps. You keep your own Stripe account,
your own pricing, and your own customers — Keylight issues the licenses and tells your
app who is allowed to run it.

- **License keys** issued automatically when a payment completes
- **Device activations** with limits you set, and self-serve deactivation
- **Offline validation** — signed Ed25519 leases your app verifies locally
- **Feature entitlements** signed into the lease, so tiers work offline too

[keylight.dev](https://keylight.dev) · [Documentation](https://docs.keylight.dev) · [Pricing](https://keylight.dev/pricing)

### Further reading

- [Offline Licensing for Your VST/AU Plugin Without a Backend](https://keylight.dev/blog/audio-plugin-licensing-cpp)
- [License Your Unreal Engine Game Offline in an Afternoon](https://keylight.dev/blog/unreal-engine-game-licensing-cpp)

## License

MIT License. See [LICENSE](LICENSE) for details.

---

<sub>Keylight C++ SDK — software licensing for C++: license-key activation & validation, offline
Ed25519 lease verification, entitlement/feature gating, trials, and pluggable transport/storage —
for desktop apps, Unreal Engine 5 plugins, and JUCE audio applications.</sub>
