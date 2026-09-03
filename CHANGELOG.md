## 0.2.0 (unreleased)

### Fixed

- **`deactivate()` never released the seat.** The request omitted `license_key`,
  which the API requires, so every deactivation was rejected before any mutation
  ran while the SDK reported success. Apps believed they were deactivated; the
  seat stayed consumed until the customer hit their activation limit.
- `maxOfflineDays` is now applied when state is resolved at launch. It was
  previously ignored on that path, so a tenant configuring 2 days still got the
  lease's full 7-day TTL offline.

### Added

- `State::Limited` for a lease with server status `fallback`.
- `os_version` and `arch` are now sent on activate, validate and keyless, so the
  matching dashboard cards are populated for C++ tenants.
- `instance_name` now carries the real machine name instead of the constant
  `"device"`.
- `X-Keylight-Request-Id` on every request, echoed by the API, so app logs and
  server logs can be correlated during support.
- Clock-rollback detection: state fails closed when the system clock has moved
  backward more than an hour since the last server contact.

### Changed

- **Breaking:** a lease with status `fallback` now resolves to `State::Limited`
  instead of `State::Expired`. A server-side signing incident degrades the app
  rather than locking it. Exhaustive `switch` statements over `State` will need
  a `Limited` arm. Unreal Blueprint users: `EKeylightState` enum is appended
  with `FreeTier` and `Limited`; ensure your state-switch logic covers both
  values (existing serialized state values are unchanged).
- **Breaking:** `deactivate()` returns the server's error instead of always
  succeeding. The local cache is still cleared either way.
- Errors from the API now carry the server's message ("License key not found",
  "Activation limit reached") instead of a bare `"activate HTTP 422"`.

## [0.1.6] - 2026-09-03

### Added

- **Free tier.** Set `Config::freeTierEnabled` and a device with no license and
  no active trial resolves the new `State::FreeTier` instead of `Invalid`.
  Resolution order is: valid paid license → active trial → free tier → elapsed
  trial → `Invalid`, matching `keylight-rust`'s `resolve_state()`.
- **`Client::reportKeylessState(KeylessState)`** — the anonymous keyless
  beacon, powering the *trials started → converted / in free tier / expired*
  funnel. Debounced to one request per 24 hours per state, with a state change
  always sending. The debounce is armed only on HTTP 200, so a failed beacon
  retries rather than suppressing reporting for a day. Fire-and-forget: errors
  are swallowed and the resolved state never changes. Blocking — never call it
  from an audio thread.
- **`KeylessState`** (`Trial` / `FreeTier` / `Expired`) with the wire strings
  `trial` / `free_tier` / `expired`, shared with every other Keylight SDK.
- **`Client::freeTierInstanceId()`** — a random, persisted, per-install id used
  for anonymous reporting. `startTrial()` now mints one too, so a trial that
  converts to paid can be attributed.
- **`machine_hash`** — where the OS exposes a stable machine identifier, a
  one-way SHA-256 of it is attached to the keyless beacon and to
  `activate`/`validate`, so a device that converts is counted once rather than
  twice. Read via IOKit on macOS, the registry on Windows and `/etc/machine-id`
  on Linux. Where no such identifier exists the field is omitted entirely — a
  random value is never substituted. `deactivate` sends neither field.
- **JUCE:** `Licensing::reportKeylessState()`, and automatic reporting on every
  state transition. The core SDK never reports on its own.

### Changed

- **`State::FreeTier` is a new enumerator.** An exhaustive `switch` over
  `keylight::State` compiled with `-Werror=switch` will not build until you add
  the case. It is appended after `Invalid`, so existing enumerator values do not
  change.
- **With `freeTierEnabled`, an elapsed trial now resolves `FreeTier` rather than
  `Expired`,** and `deactivate()` lands on `FreeTier` rather than the paywall.
  Products leaving the flag at its `false` default are unaffected.
- **The core now links two system frameworks** — IOKit + CoreFoundation on
  macOS, advapi32 on Windows, nothing on Linux — to read the machine
  identifier. CMake adds them automatically; single-header users must add the
  link line themselves. There are still no third-party dependencies.

### Fixed

- **`tests/test_amalgamation.cpp` was passing vacuously in any build defining
  `NDEBUG`.** It checks everything with bare `assert()`, so a Release build
  compiled every check away. CI builds Debug and was genuinely asserting, but
  the release workflow builds Release — the release gate had been weaker than
  the PR gate. Now immune to `NDEBUG`.
- **`demo/notes` reported `"Unknown"` for new license states.** No compiler
  warning flags were configured at all, so a missing `switch` case was silent.
  `-Wall -Wswitch-enum` is now enabled on this repo's own targets (never on the
  `keylight` INTERFACE target, which would force flags onto consumers).

## [0.1.5] - 2026-09-03

### Fixed

- **The tenant SDK key is now actually sent.** `Config::sdkKey` was documented as
  being sent as `X-Keylight-SDK-Key`, but the core `Client` only ever set
  `Content-Type`, so `activate`, `validate`, `deactivate` and the launch-time
  revalidation could come back `401`. Every API call now goes through one header
  helper that adds `X-Keylight-SDK-Key` whenever `sdkKey` is configured. Set
  `cfg.sdkKey` and no other code change is needed.
- **Persisted state is serialized in one place.** `activate`, `validate`, lease
  refresh, lease clearing (revoke), and `deactivate` each rebuilt the on-disk JSON
  blob themselves, so a partial write could drop fields it had not touched
  (`licenseKey` was lost on every `validate`). The blob is now rebuilt from the
  in-memory cache by a single serializer, so no path can erase a field it does
  not own.

### Added

- **Local trials.** `Config::trialDurationDays` is finally wired up, following the
  Rust SDK's model: trials are **local and offline-first**, persisted on-device as
  a `trialStart` timestamp, and never dependent on the free-tier/keyless endpoint
  (which stays a separate feature).

  New public API on `keylight::Client`:

  | Method | Description |
  |--------|-------------|
  | `startTrial() → Result<State>` | Explicitly begins the trial. Idempotent — never restarts an existing or elapsed one. |
  | `checkTrial() → TrialStatus` | `NotStarted` / `Active` / `Expired`. |
  | `trialDaysLeft() → int` | Whole days remaining (0 when disabled/not started/elapsed). |

  Semantics: `trialDurationDays <= 0` disables trials entirely; `checkOnLaunch()`
  resolves a persisted trial offline but **never starts one** (a DAW scanning a
  plugin must not consume the user's trial); state priority is valid paid license
  → active trial → elapsed trial → `Invalid`; and deactivating a paid license
  restores the *original* trial state without resetting its clock.

  The trial start is stored in the same JSON blob as the lease — it is a
  convenience, not a tamper-proof or reinstall-proof mechanism.

- **`refreshIfNeeded()` re-resolves the local trial.** With no stored license it
  used to return the cached state verbatim, so a trial that ran out mid-session
  stayed `Trial` until the next launch. `keylight-rust` and `keylight-js`
  recompute `check_trial()` inside `state()` on every call; C++ `state()` is an
  atomic read (audio-thread contract) and cannot, so `refreshIfNeeded()` carries
  that duty — hosts already call it on focus/resume and `startAutoValidation()`
  ticks it. Still no network call when there is no license to validate.

- **JUCE adapter trial support.** `Licensing::startTrial(callback)` runs on the
  existing background dispatch (never `processBlock`) and reports through the same
  state snapshot / `onStateChanged` subscription; `trialStatus()` and
  `trialDaysLeft()` are message-thread UI queries.

## [0.1.4] - 2026-08-07

### Added

- **Coarse device telemetry.** `activate` and `validate` now also send `cpu_cores`
  and `memory` alongside the existing `platform` / `sdk` / `sdk_version` fields, so
  the dashboard can show what class of machine your customers run on. Both are
  **buckets**, never the real numbers — `cpu_cores` is one of `1-2`, `3-4`, `5-8`,
  `9-16`, `17+` and `memory` is one of `<4GB`, `4-8GB`, `8-16GB`, `16-32GB`,
  `32-64GB`, `64GB+`. The exact core count and exact RAM size never leave the
  machine. Both fields are optional and omitted when the platform cannot report
  them. No API change and nothing to do in your code.

## [0.1.3] - 2026-08-01

### Fixed

- **Revocation now enforced; offline use bounded to 15 days.** Launch always performs a
  server `validate`, so a dashboard revoke/expiry takes effect on the next launch. A real
  HTTP 422 revoke response is decoded and enforced instead of being swallowed as a transient
  failure. The default `max_offline_days` is 15.

### Added

- **The SDK now identifies itself on the wire.** `activate` and `validate` send
  `sdk: "cpp"` alongside the existing `platform` field. `platform` reports the
  operating system and nothing more — it is identical across the C++, Rust and C#
  SDKs, so it could not say which SDK a device was running. No API change and
  nothing to do in your code.

## [0.1.2] - 2026-06-23

### Fixed
- **Unreal Engine plugin now compiles on clang targets (macOS/Linux/iOS/Android).**
  `Keylight.Build.cs` built the module with C++ exceptions disabled, but the
  SDK's `FileStore` (which the plugin's `UELicenseStore` wraps) catches
  `std::filesystem_error` around its atomic-rename writes. Clang rejected this
  with "cannot use 'try' with exceptions disabled". The module now sets
  `bEnableExceptions = true` (RTTI stays off; the SDK uses neither
  `dynamic_cast` nor `typeid`). The public SDK API remains exception-free.
- **Unreal plugin: safe teardown of the licensing client.** `UKeylightSubsystem`
  now declares an out-of-line destructor so its `TUniquePtr` members (holding
  forward-declared SDK types) are destroyed where those types are complete —
  guaranteeing `~Client()` runs (joining the background auto-validation thread)
  instead of being silently skipped against an incomplete type.

## [0.1.1] - 2026-06-23

### Fixed
- **JUCE adapter now compiles.** Two errors in `integrations/juce/KeylightJuce.h`
  prevented it from building in a real JUCE project:
  - It used `juce::MessageManager::callAsync` but only included
    `<juce_core/juce_core.h>`; `MessageManager` lives in `juce_events`. The
    header now includes `<juce_events/juce_events.h>` so it is self-contained.
    (If you wire JUCE modules manually, link `juce_events` as well as `juce_core`.)
  - `JuceUrlTransport` built `juce::URL::InputStreamOptions` by reassignment,
    but that type is not copy-assignable; the options are now built in a single
    chained expression.

### Added
- Continuous integration for the JUCE adapter (`.github/workflows/juce.yml`):
  every push compiles, links, and smoke-tests `KeylightJuce.h` against real JUCE
  (7.0.12 and 8.0.6) on Linux, macOS, and Windows, so the adapter no longer
  relies on manual verification.

## [0.1.0] - 2026-06-22

### Added
- JUCE single-header adapter (`integrations/juce/KeylightJuce.h`):
  `keylight::juce_integration::JuceUrlTransport` (implements `keylight::Transport`
  over `juce::URL::createInputStream` with `InputStreamOptions` — no OpenSSL,
  no cpp-httplib) and `keylight::juce_integration::Licensing` (owns the `Client`,
  `FileStore`, and transport; exposes `activate`/`validate`/`deactivate`/
  `checkOnLaunch` with message-thread callbacks via
  `juce::MessageManager::callAsync`; audio-thread-safe `state()` and
  `hasFeature()` via `std::atomic` snapshots updated by the SDK subscription
  callback; multi-instance safe — no global/static mutable state).  Compiles
  against JUCE 7 and JUCE 8 headers with zero extra dependencies beyond
  `juce_core`.  **Manual plugin-project build pending:** no JUCE toolchain is
  available in CI; a developer with JUCE 7/8 installed must compile and
  smoke-test before shipping.
- Unreal Engine plugin (`integrations/unreal/Keylight/`): `UKeylightSubsystem`
  (Blueprint-callable `Activate`/`Validate`/`Deactivate`/`HasEntitlement`/`GetState`
  with `FOnKeylightResult` async delegates), `FHttpTransport` (UE `FHttpModule`
  adapter for `keylight::Transport` — blocks a background thread via `FEvent`,
  never the game thread), and `UELicenseStore` (lease persistence under
  `Saved/Keylight/`).  The plugin compiles against UE 5.x headers with zero
  extra dependencies beyond `Core`, `CoreUObject`, `Engine`, `HTTP`, and `Json`.
  **Manual editor build pending:** no UE toolchain is available in CI; a
  developer with UE 5.x installed must compile and smoke-test before shipping.
