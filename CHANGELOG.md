## [0.2.1] - 2026-09-05

### Fixed

- **`Config`'s two new fields moved to the end of the struct.** `0.2.0` added
  `requireSignedConfig` and `keylessHeartbeatIntervalMs` in the MIDDLE, ahead of
  `autoValidationIntervalMs`. Positional aggregate initialisation binds by
  position, so `Config cfg{"tenant", "product", ..., 60000}` written against
  `0.1.6` silently shifted every value after the insertion point, or failed to
  compile. Nothing else changes — the fields, their names, their defaults and
  their behaviour are identical.

  Only affects code that aggregate-initialises `Config` positionally; the
  documented style is field assignment, and that was never affected. If you
  have not adopted `0.2.0` yet, upgrade straight to this and the question does
  not arise.

  The struct now carries a comment saying to append rather than insert, which
  is the rule `State::FreeTier` and `State::Limited` already document for the
  same reason.

## [0.2.0] - 2026-09-05

### Fixed

- **`deactivate()` never released the seat.** The request omitted `license_key`,
  which the API requires, so every deactivation was rejected before any mutation
  ran while the SDK reported success. Apps believed they were deactivated; the
  seat stayed consumed until the customer hit their activation limit.
- **Busy-spin on a misconfigured auto-validation interval.** A non-positive
  `Config::autoValidationIntervalMs` made the background thread's wait return
  immediately, so it hammered `refreshIfNeeded()` instead of polling. Clamped
  to 1 ms.
- `maxOfflineDays` is now applied when state is resolved at launch. It was
  previously ignored on that path, so a tenant configuring 2 days still got the
  lease's full 7-day TTL offline.
- **The headers are warning-free under strict flags.** `client.hpp`,
  `ed25519.hpp` and `verifier.hpp` emitted 11 warnings on Clang under
  `-Wextra`-class settings (JUCE applies these by default via
  `juce_recommended_warning_flags`), which meant an integrator building with
  `-Werror` had to carve the SDK out of it. All behaviour-neutral: three
  omitted `PersistData` field initialisers now spelled `std::nullopt`, seven
  signed loop indices used to subscript containers, and one dead parameter.

### Added

- `State::Limited` for a lease with server status `fallback`.
- `os_version` and `arch` are now sent on activate, validate and keyless, so the
  matching dashboard cards are populated for C++ tenants.
- `instance_name` now carries the real machine name instead of the constant
  `"device"`. **This means the machine's hostname is now sent to the API**,
  where it appears in the customer's own device list on your dashboard —
  previously every device was listed as `"device"`. It is capped at 64 bytes
  and stripped of control characters; there is no opt-out.
- `X-Keylight-Request-Id` on every request, echoed by the API, so app logs and
  server logs can be correlated during support.
- **`keylight::clock_rolled_back(last_seen, now)`** and
  **`keylight::CLOCK_BACKWARD_TOLERANCE`** (1 hour), in `keylight/clock.hpp`.
  Public surface: `clock_rolled_back` is true when `now` is more than the
  tolerance behind `last_seen`. It deliberately omits the forward-jump half of
  keylight-rust's `clock_manipulated()`, because going offline for a while is
  governed by `maxOfflineDays`, not by this guard.
- **Transient HTTP failures are retried** — 408, 429, 5xx and network errors,
  up to three attempts with exponential backoff from 500ms, honouring
  `Retry-After` on a 429 (clamped to an hour, so a hostile value cannot park
  the calling thread). A rate limit is no longer a flat activation failure for
  the end user. Every other 4xx is treated as the server's final answer and is
  not retried. Note the cost: a call that used to fail fast can now take the
  sum of the backoffs before it returns.
- `HttpResponse::headers`, with names lowercased. Appended last, so existing
  `{status, body}` aggregate initialisation still compiles. A custom
  `Transport` that does not populate it simply loses `Retry-After` and falls
  back to plain exponential backoff.
- **`EncryptedFileStore`** — ChaCha20-Poly1305 (RFC 8439, vendored) with a key
  derived from this machine's stable id. A store copied to another machine no
  longer opens. Anything undecryptable reads as "no data" rather than an error,
  so a truncated file from a crash degrades instead of failing.
- **`EncryptedFileStore::migrating(path, legacy_path)`** — the form integrators
  want: binds to this machine and imports a pre-0.2.0 plaintext store once.
- **`legacy_plaintext_path(cfg)`** — the pre-0.2.0 `.lease` location, to hand
  to `migrating()`.
- **Server-owned trial configuration.** Trial length is now a dashboard field
  per app, not a value you compile in. `Config::trialDurationDays` is only the
  pre-first-contact seed; resolution order is server → seed → 0. The settings
  ride on responses to calls already being made (`validate` for licensed
  installs, the keyless beacon for the rest), so nothing pays an extra round
  trip. `Client::fetchConfig()` is there for an explicit refresh.
  `Client::effectiveTrialDurationDays()` and `effectiveFreeTierEnabled()`
  report what is actually in force.
- **The config response is signed, and verified.** `GET /config` was the
  unsigned soft spot in an otherwise signed protocol: a lease cannot be forged,
  so a fake server mints no licence — but it could mint an unlimited *trial*,
  via nothing more than a hosts entry. `Config::requireSignedConfig` (default
  `false` during the rollout) governs responses carrying no signature; one that
  is present is always verified, and a bad one always rejected. See
  `config_payload.hpp` for the payload format and for what this does *not* buy.
- **`Client::activeRevalidate()`** — force a revalidation when the app is
  foregrounded, debounced to 60s. The debounce is per-session, so a relaunch
  always revalidates.
- **`Client::refreshAfterUpgrade()`** and **`refreshAfterUpgradeAsync()`** — a
  bounded poll so an in-app upgrade unlocks without a relaunch, rather than
  waiting out payment-webhook lag.
- **`Client::upgradeUrl()`** and **`Client::normalizeKey()`**.
- **`Client::cachedLease()`**, **`hasStoredLicense()`**, **`cachedLicenseKey()`**.
  `cachedLease()` returns the lease as-is and does not re-verify — it is for
  displaying what is stored, not for deciding what to unlock.
- **`Client::isValidKeyFormat()`**, driven by `Config::keyPrefix` — which was
  declared but never read until now.
- **Lifecycle events** (`Renewed`, `Cancelled`, `Expired`, `Restored`) via
  `Client::onLifecycle()`, plus `on("renewed")`-style names. Distinct from
  `subscribe()`, which fires on every transition; these are the subset worth
  telling a customer about. `State` moved to the new `keylight/lifecycle.hpp`,
  which `client.hpp` includes — no change to the enum or its values.
- **Keyless heartbeat**, on by default at 6h
  (`Config::keylessHeartbeatIntervalMs`, `0` to disable). Beacon only: it never
  revalidates a licence, and a `Licensed` or `Limited` device never beacons at
  all. The thread is spawned lazily on first state resolution and its first
  tick is at `+interval`, so plugin scanning pays for neither.
- **`sdk_trial_duration_days`** on activate and validate — the trial length the
  *build* was compiled with. Diagnostic only, so a dashboard can flag a 30-day
  build running against a 14-day setting. The server must never gate on it: a
  patched client sends whatever it likes.

### Changed

- **Breaking:** a lease with status `fallback` now resolves to `State::Limited`
  instead of `State::Expired`. A server-side signing incident degrades the app
  rather than locking it. Exhaustive `switch` statements over `State` will need
  a `Limited` arm. Unreal Blueprint users: `EKeylightState` enum is appended
  with `FreeTier` and `Limited`; ensure your state-switch logic covers both
  values (existing serialized state values are unchanged).
- **JUCE:** `Licensing::hasFeature()` takes `juce::StringRef` instead of
  `const juce::String&`. Source-compatible — a literal or a `juce::String`
  both convert — but it fixes a heap allocation on the audio thread: binding
  `hasFeature("pro")` to a `const String&` materialised a temporary
  `juce::String` on every `processBlock` call, in release builds too. The
  allocation is only removed at the default `JUCE_STRING_UTF_TYPE == 8`; at 16
  or 32 `StringRef` holds a `juce::String` member and a literal allocates
  again.
- **JUCE:** `hasFeature()` is documented as returning `false` for every key
  other than `"pro"`. That was always the behaviour; the docstring claimed it
  cached the last subscribed feature. Cache additional entitlements yourself —
  the README shows the pattern.
- **JUCE:** The state switch (`Licensing::onStateChanged`) now handles
  `State::Limited`, so a fallback lease from a signing-key incident surfaces as
  the degraded state rather than being treated as expired.
- **Clock-rollback guard, at every state read point.** With the system clock
  more than an hour behind the last recorded server contact, `state()`,
  `hasEntitlement()` and *every* public method returning a `Result<State>` —
  `activate()`, `validate()`, `startTrial()`, `checkOnLaunch()`,
  `refreshIfNeeded()` — fail closed together, reporting `Invalid` and `false`
  rather than ageing a cached lease against a clock that moved. Note
  `startTrial()`: it still starts and persists the trial, but reports
  `Invalid` while the clock is untrusted. Errors are passed through untouched
  — an error is not a state claim. `maxOfflineDays` is enforced the same way
  when the stored anchor sits *ahead* of the clock, which previously disabled
  the bound silently. The guard clears itself the moment the clock is honest
  again, or on the next successful server contact.
- **State-change events now report the guarded state, and the guard raises
  its own.** Subscribers registered with `subscribe()` / `on()` are handed
  what `state()` would return, so a paywall driven by the event stream cannot
  disagree with one driven by the query API. Because a clock that moves
  changes no underlying state, the event is raised from `refreshIfNeeded()` and
  `validate()` — `startAutoValidation()` already ticks the former — in both
  directions: once when the rollback is detected, and again when the clock
  becomes honest. Events
  are deduplicated on the reported value, so a host that caches the last one
  cannot be left holding a stale `Licensed`.
  Events are ordered: concurrent notifiers cannot deliver out of order and
  leave a subscriber holding a value `state()` contradicts. Ordering is fixed
  under an SDK lock, but delivery happens with **no lock held**, so a callback
  may take your own locks and may call back into the `Client` — a re-entrant
  call queues its event rather than recursing, and may be delivered by a
  different thread. Do not destroy the `Client` from a callback.
- Because `validate()` also raises the guard's event, a `validate()` that spans
  a clock correction now emits `Invalid` where it previously emitted nothing —
  followed by the resolved state on the success path, and on a decodable 422
  (revoke, deactivated instance, expired lease), which also resolves a state.
  A network failure, a bad-JSON body, another non-200 or an undecodable 422
  returns early and emits just the `Invalid`; the next poll corrects it, and it
  fails closed meanwhile. Both events are accurate — during the round trip
  `state()` genuinely reported `Invalid` — but debounce your paywall if you
  drive UI straight off events.
- **Breaking (auto-validation):** `stopAutoValidation()` no longer waits for
  the background worker to exit. It retires the worker and returns
  immediately; a worker already inside `refreshIfNeeded()` finishes that call
  first, so one more tick — and possibly one more state-change event — can
  land shortly after the call returns. If you relied on "stopped means
  stopped", gate on your own flag rather than on the call returning.

  `~Client()` still joins, so a worker can never outlive the `Client` — but
  note the cost moved rather than vanished. It usually returns immediately:
  the destructor retires the worker and wakes it before joining, so one parked
  in its interval wait exits without another cycle. It blocks only when the
  worker is mid-cycle — by up to one round trip, plus every queued listener
  callback if that worker is the one delivering events. That upper bound is
  unbounded in principle, because listeners are your code. It matters if you
  destroy the client on a UI thread (JUCE's `~Licensing` does).

  Workers are now retired by epoch instead of being joined, which is what makes
  the rest of this section true. `startAutoValidation()` and
  `stopAutoValidation()` are safe from any thread, including from a
  state-change listener delivered on the worker thread itself; neither ever
  blocks on the other or on a listener; both are idempotent; and stop-then-
  start restarts polling, including when the stop came from such a listener.
  Previously that combination could self-join and abort the process via
  `std::terminate`, deadlock, silently no-op forever, or leave two workers
  polling at once, depending on the timing.
- A listener that throws no longer costs the other listeners their event, and
  no longer propagates out of the SDK call that delivered it.
- **Breaking:** `deactivate()` returns the server's error instead of always
  succeeding. The local cache is still cleared either way.
- Errors from the API now carry the server's message ("License key not found",
  "Activation limit reached") instead of a bare `"activate HTTP 422"`.
- **Breaking:** the default store path moved from
  `~/.keylight/<tenant>-<product>.lease` to the same name with a `.bin`
  extension. Existing plaintext stores are imported once on first read and then
  deleted, so licensed users and users mid-trial keep their state and see
  nothing. The import is unconditional — trial start included — and the
  plaintext file is unlinked only after the encrypted copy is safely written.

  This matters if you construct your own store. `default_store_path(cfg)` now
  names a different file, so a plain `FileStore` pointed at it will not find a
  pre-0.2.0 store; it will also not overwrite one. Use
  `EncryptedFileStore::migrating(default_store_path(cfg), legacy_plaintext_path(cfg))`
  — what the README now shows.
- The default store path is no longer where the SDK's own docs pointed a
  `FileStore`; `FileStore` remains supported for integrators supplying their
  own storage, and is unchanged.

- **Breaking:** `startTrial()` now records the trial start even when the
  effective duration is 0, where it previously wrote nothing at all. The
  duration is a server setting that can arrive after first launch, and bailing
  out early left no clock for it to measure — which is what made a
  dashboard-set trial do nothing. A recorded start is honoured as-is: enabling
  trials later does not retroactively grant one to an install that already
  started. The anonymous free-tier instance id is minted at the same point, for
  the same reason, so a trial that later converts can be attributed.
- **Breaking:** `checkTrial()` and `trialDaysLeft()` measure against the
  effective duration rather than `Config::trialDurationDays`, and state
  resolution reads the effective free-tier flag. A dashboard setting now takes
  effect without shipping an app release — and, in the other direction, turns a
  seed-enabled trial off.

### Security

- **The local store is no longer plaintext.** Editing `trialStart` with a text
  editor to extend a trial no longer works: the blob is authenticated, so any
  edit fails to open and reads as "no data".
- **What this does NOT do — stated plainly, because it is easy to overread.**
  It does not stop seat sharing. A license key shared between machines still
  yields working installs, because the API authorizes a validate on
  `(license_key, instance_id)` without checking which machine is asking. What
  encryption buys is tamper resistance, non-portability of what this SDK
  writes, and a drying supply of harvestable plaintext files. Closing seat
  sharing requires binding an instance to `machine_hash` server-side and is
  tracked separately.
- A machine whose stable id CHANGES — hardware swap, OS reinstall, regenerated
  `/etc/machine-id` — cannot open its existing store and sees a first run,
  costing the user one reactivation. That is the intended cost of the store not
  being portable.
- On a machine that exposes no stable id at all (a Linux image with neither
  `/etc/machine-id` nor the dbus fallback), the key derives from a constant.
  Tamper resistance still holds; machine binding degrades only there. Nobody is
  locked out.
- **A fake server can no longer grant an unlimited trial.** The signed `/config`
  response closes the cheapest attack there was: redirect the API host and
  answer with a 3650-day trial, no binary patching required. The same rule
  applies wherever those fields ride — a `validate` body cannot deliver
  unsigned settings past `requireSignedConfig`, or the check would be one route
  away from useless.
- **What that does NOT do, stated plainly.** It does not defeat a patched
  binary, which can remove the check like any other. It raises the cheapest
  attack from editing a hosts file to patching and re-signing an app. Stopping
  the patched case requires the server to refuse — a server-side trial ledger
  keyed to `machine_hash` — which is tracked separately and is not in this
  release.

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
