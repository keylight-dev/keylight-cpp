# keylight-cpp 0.2.0 — client parity with Rust and Swift

- **Date:** 2026-09-03
- **Status:** design approved; implementation plan pending
- **Target version:** 0.2.0 (single release — no interim 0.1.7)
- **Depends on:** `keylight/docs/superpowers/specs/2026-09-03-server-owned-trial-config-design.md`
  (the `GET /config` route must ship first)
- **Also changes:** `keylight-rust` — two fixes, listed under "Rust changes"

## Motivation

An audit of the C++ SDK against the Rust SDK, the Swift SDK and the worker's
zod contracts found the cryptography in exact parity — the Ed25519 verifier,
the `machine_hash` formula and the `cpu_cores`/`memory` buckets are all pinned
to shared vectors and green — but the **client** well behind.

The headline finding is a blocking bug:

> `deactivate()` never releases the seat. `client.hpp:376` sends
> `{"instance_id": …}`; `DeactivateBodySchema`
> (`worker/src/routes/deactivate.ts:16`) requires `license_key` **and**
> `instance_id`. Zod rejects the body, no mutation runs, and the client — which
> treats the call as best-effort — clears its local cache and reports success.
> The app believes it is deactivated; the dashboard keeps the seat consumed;
> the customer eventually hits their activation limit and cannot activate
> anywhere.

It went unnoticed because the live test prints "seat released" without ever
checking the response (`tests/test_live.cpp:138`).

Beyond that: three fields the dashboard renders are never sent, there is no
`Limited` state, no retry, no clock guard, the local store is plaintext, and
roughly half a dozen functions present in Rust and Swift are missing.

## Reference implementations

| SDK | Availability | Role here |
|---|---|---|
| `keylight-rust` | full source | **authoritative** for wire shape, retry policy, clock guard and state resolution |
| `keylight-swift` | full source under `keylight/Sources/KeylightSDK` | authoritative for `activeRevalidate` / `refreshAfterUpgrade` semantics |
| `keylight` worker | full source | authoritative for every request/response contract |

Where this design deviates from Rust it says so explicitly and why.

## Approved decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | One 0.2.0 release, no interim hotfix | Chosen over shipping the seat fix early |
| D2 | `State::Limited` appended **after** `FreeTier` | Renumbering would break anyone who persisted a `State` as an integer — same reasoning as the existing note at `client.hpp:52` |
| D3 | Vendored ChaCha20-Poly1305, header-only | Keeps the zero-dependency, single-include promise; the SDK already vendors SHA-256 and Ed25519 |
| D4 | Store key = `sha256("keylight-store-v1" ‖ machine_id())` | Machine-bound, reuses `machine_id.hpp`, mirrors Rust's BLAKE3-derived device key |
| D5 | Migrate plaintext on read **unconditionally**, then unlink it | An expiry gate was considered and rejected: it does not close seat sharing (see 4.3) and it strands honest users who were offline past the lease TTL |
| D6 | Import `trial_start` unconditionally | Gating it buys nothing: deleting *any* store mints a fresh trial, encrypted or not. Rust has the same property |
| D7 | OS version read **without spawning a process** | Precedent D4 of the free-tier spec: sandboxed AU/VST3 hosts may block process spawn. Diverges from Rust deliberately |
| D8 | `HttpResponse` gains a `headers` map | `Retry-After` is unreadable today; a trailing member keeps aggregate init source-compatible |
| D9 | Effective trial duration = cached server value → `Config` → 0 | `Config` becomes a pre-first-contact seed only |
| D10 | `startTrial()` stamps `trial_start` unconditionally; a stored start is honored as-is | One field, one rule; removes the first-launch race |
| D11 | `upgradeUrl()` targets the **authenticated** portal route | The standalone form is retired (`worker/src/portal/index.ts:42`); hitting a sign-in wall is acceptable |
| D12 | Key-format validation is driven by `Config::keyPrefix` | Finally reads a field declared at `config.hpp:16` and never used |
| D13 | The C++ keyless heartbeat lands here, on by default, **lazily spawned** | Ownership is split by repo, not by feature — see below. Parity with Swift's 6h default and Rust's new `enabled: true` |

## 1. Wire fixes

### 1.1 `deactivate` — the blocker

Add `license_key` from `cached_license_key_` (`client.hpp:770`), which is
already cached and already used by `validate()`.

Behavior change: the local store is still cleared unconditionally, but the
server outcome is **no longer swallowed**. `deactivate()` returns the failure so
an integrator can retry rather than believing a seat was freed. The trial-start
preservation logic at `client.hpp:385-400` is unchanged.

### 1.2 `instance_name`

`client.hpp:214` hardcodes `"device"`, so every C++ install shows up under that
name in the dashboard device list. Replace with a real machine name:
`gethostname()` on POSIX, `GetComputerNameW` on Windows, sanitized and clamped
to the server's cap, falling back to `"device"` when the read fails.

### 1.3 `os_version` and `arch`

`build_json_` (`client.hpp:840`) sends `sdk_version`, `platform`, `sdk`,
`app_version`, `cpu_cores` and `memory`. The worker accepts `os_version` and
`arch` on activate, validate and keyless (`worker/src/routes/activate.ts:36-38`),
and the dashboard renders cards for both — empty for every C++ tenant today.

New in `device_info.hpp`:

- `current_arch()` → `"arm64"` / `"x86_64"` / empty. The server allow-lists
  exactly these two; anything else is omitted rather than bucketed.
- `detect_os_version()` → dotted-numeric string or empty, normalized by the
  same `\d+(\.\d+)*` rule Rust applies, with the server's 32-char cap enforced
  before sending (an over-long value 400s the whole request).

**Deviation from Rust (D7).** Rust shells out to `sw_vers` and `cmd /c ver`.
This SDK ships inside JUCE plugins and Unreal; forking a process during plugin
init is not acceptable and the free-tier spec already established the rule for
hardware IDs. Same output, no fork:

| Platform | Mechanism |
|---|---|
| macOS | `sysctlbyname("kern.osproductversion")` |
| Windows | `RtlGetVersion` |
| Linux / other | `uname()` release field |

Both functions are pure/`noexcept`-ish and return empty on any failure; the
caller omits the field entirely rather than guessing.

### 1.4 `X-Keylight-Request-Id`

Added in `json_headers_()`, a fresh random hex value per request. The worker
echoes the client's header (`worker/src/index.ts:211`) so an app log line and a
server log line can be correlated. Rust and Swift already send it; C++ support
is blind without it.

### 1.5 Server error messages

`activate()` returns `"activate HTTP 422"` (`client.hpp:227`), discarding the
worker's `errorMessage` — "License key not found", "Activation limit reached",
"License expired". Parse the JSON error field from a non-200 body and surface
it in the `Result` error, falling back to the status line when the body is
unparseable. This is the string an integrator's UI actually shows.

## 2. State machine

### 2.1 `State::Limited`

```cpp
enum class State {
    Licensed, Trial, Expired, Invalid, FreeTier,
    Limited,   // appended: see D2
};
```

`resolve_from_lease_` maps lease status `"fallback"` to `Limited` instead of
`Expired`, matching `resolve_state` in `keylight-rust/keylight/src/state.rs`.
The switch at `client.hpp:82` gains an arm.

**This is a behavior change.** A server signing-key incident currently locks the
application; after this it degrades it. Integrators switching exhaustively over
`State` will get a compiler warning for the new value, which is the intent.
CHANGELOG must call it out.

### 2.2 Clock-rollback guard

Port `clock_rolled_back` from `keylight-rust/keylight/src/clock.rs`: flag when
the clock has moved backward more than 1h since the last recorded contact.
Checked in `state()`. Deliberately excludes the forward-jump component so it
gates state resolution without governing offline duration — that stays with
`maxOfflineDays`.

### 2.3 `maxOfflineDays` applied at launch

`refresh_state_from_store_()` (`client.hpp:1001`, called from the constructor)
resolves from the lease without consulting `lastValidatedOnline`. Rust checks it
on every `state()` and fails closed when the anchor is missing. Today a tenant
setting `maxOfflineDays = 2` still gets 7 days offline, bounded only by the
lease TTL.

## 3. Retry

New pure header `include/keylight/retry.hpp`, a direct port of
`keylight-rust/keylight/src/http/retry.rs`:

- 3 attempts, 500ms base, exponential, capped at 4s, with jitter added by the
  caller.
- Retryable statuses: 408, 429, 5xx.
- `Retry-After` honored on 429, clamped to a safe maximum.

Applied in a `request_with_retry_()` helper used by activate / validate /
deactivate / keyless / config. Today a 429 from the rate limiter is a flat
activation failure for the end user.

**Transport change (D8).** `HttpResponse` (`include/keylight/transport.hpp:17`)
has `status` and `body` only, so `Retry-After` cannot be read. It gains a
trailing `std::map<std::string, std::string> headers`. Existing aggregate
initialization `{status, body}` keeps compiling, and a custom transport that
does not populate headers degrades gracefully to plain exponential backoff.

Sleep is injected as a seam alongside `now_fn_` so the policy is unit-tested
without sleeping.

## 4. Encrypted store

### 4.1 Cipher

New `include/keylight/chacha20poly1305.hpp` — vendored, header-only, pinned to
the RFC 8439 test vectors. `THIRD_PARTY_NOTICES.md` updated.

### 4.2 `EncryptedFileStore`

- Key: `sha256("keylight-store-v1" ‖ machine_id())`.
- Layout: `nonce(12) ‖ ciphertext ‖ tag(16)`.
- Atomic write via temp file + rename, as today's `FileStore`.
- Becomes the default store. `FileStore` stays public and supported for
  integrators supplying their own storage.

Today's store is plaintext JSON at `~/.keylight/<tenant>-<product>.lease`, not
bound to the machine.

### 4.3 Migration

```
first read on 0.2.0:
  encrypted file exists?  -> use it; never look at plaintext
  plaintext exists?       -> import lease + license_key + instance_id + trial_start
                             write encrypted, unlink plaintext
```

**What this does and does not buy — stated plainly, because an earlier draft of
this design overclaimed it.**

A lease binds to `instanceId` (`include/keylight/lease.hpp:18`, signed into the
v3 payload) and nothing client-side checks that the instance belongs to this
machine. Worse, the server does not check either: `validate` confirms only that
the activation exists and is `active`
(`worker/src/services/runtime-licensing.ts:206-209`). So N machines sharing one
`(license_key, instance_id)` pair validate indefinitely against **one seat**,
and the server cannot distinguish them.

Encrypting the store therefore buys:

- **Tamper resistance.** The blob can no longer be edited in a text editor.
- **Non-portability of what this SDK writes.** A 0.2.0 store is bound to
  `machine_id()` and is useless if copied.
- **A drying supply.** Once an install upgrades, its plaintext file is deleted
  and never regenerated, so there is nothing left to harvest.

It does **not** stop seat sharing. A blob harvested from a pre-0.2.0 install —
or simply a shared license key — still yields a working extra install. That is
a protocol gap, not a storage gap, and its fix is `machine_hash` binding (see
Out of scope).

**An earlier draft gated the import on `lease.expiresAt > now`, claiming it
closed transplanting.** It does not: a freshly harvested blob passes the gate
and becomes a permanent machine-bound install. All the gate actually does is
reject stale blobs, while stranding an honest user who was offline longer than
the 7-day lease TTL — dropping their `license_key` and `instance_id` would force
a re-activation, consuming another seat against their activation limit
(`runtime-licensing.ts:304` mints a fresh instance on every activate). The gate
is dropped.

`trial_start` imports unconditionally per D6: gating it buys nothing either,
since deleting any store mints a fresh trial whether it is encrypted or not.

## 5. Trial configuration

Consumes `GET /config` from the worker spec.

- **Not fetched at launch.** `tests/test_free_tier.cpp:211-224` asserts that
  `checkOnLaunch()` makes zero network calls on an unlicensed device — a DAW
  scanning plugins instantiates a `Client` per plugin, so a launch-time request
  would mean one call per plugin per scan. The settings ride on responses to
  calls that already happen instead: `validate` covers licensed installs, and
  the keyless beacon (fired by the heartbeat, off the calling thread) covers
  unlicensed ones. `GET /config` stays available as an explicit opt-in call.
- Cached in the encrypted store alongside the lease, so an offline launch uses
  the last known values. A brand-new install that has never reached the server
  uses `Config::trialDurationDays` until its first beacon — which is exactly
  what the seed is for.
- **Effective trial duration** = cached server value → `Config::trialDurationDays`
  → 0. Same resolution for `free_tier_enabled`.
- `Config::trialDurationDays` stays at `0` (`config.hpp:17`) and is now
  documented as a pre-first-contact seed, not a setting.

`trialDurationDays <= 0` is currently a hard gate at the top of three functions
— `startTrial()` (`client.hpp:501`), `checkTrial()` (`client.hpp:529`) and
`trialDaysLeft()` (`client.hpp:546`) — which is why a server value of 14 with a
local 0 would otherwise do nothing at all: `startTrial()` returns before
persisting anything, so there is no clock for the server value to measure.

Per D10:

- `startTrial()` drops the gate and stamps `trial_start` whenever one is not
  already set.
- `checkTrial()` / `trialDaysLeft()` / `days_left_from_` measure against the
  **effective** duration; an effective 0 still reports `NotStarted`.
- A stored start is honored as-is. A tenant who enables trials later does not
  retroactively grant one to installs that already stamped a start — see D4 of
  the worker spec, which requires the dashboard to disclose this.

`resolve_with_trial_` (`client.hpp:1169`) switches from `cfg_.freeTierEnabled`
to the effective value.

## 6. New API surface

| Added | Shape / semantics |
|---|---|
| `activeRevalidate()` | Force revalidation on foreground, debounced 60s. Downgrades only on a definitive server rejection; a transient failure never downgrades a live session. Mirrors `LicenseManager.activeRevalidate` (`Sources/KeylightSDK/LicenseManager.swift:458`) |
| `refreshAfterUpgrade(timeout, pollInterval)` | Bounded poll until entitlements or state change, default 30s / 2s. Sync plus a `std::future` variant alongside the existing `activateAsync` / `validateAsync`. Mirrors `LicenseManager.swift:418` |
| `upgradeUrl()` | `https://portal.keylight.dev/t/{tenant}/license/{normalizedKey}/upgrade` |
| Lifecycle events | `Renewed` / `Cancelled` / `Expired` / `Restored`, ported from `state.rs::lifecycle_event`. Wires up the `event` parameter that `on()` already accepts and ignores (`client.hpp:712`) |
| `cachedLease()` / `hasStoredLicense()` / `cachedLicenseKey()` | Thin reads over existing cache fields |
| Key-format validation | Client-side, driven by `Config::keyPrefix` (D12) |

**`upgradeUrl()` is constructible.** The portal's `licenseId` path segment *is*
the normalized license key (`worker/src/portal/services/license-view.ts:46`),
and `normalizeKey` strips `[\s-]` and uppercases
(`worker/src/types.ts:567`) — a pure function the SDK can reproduce from the
cached key with no new server data. The page requires a magic-link session;
landing on a sign-in wall is accepted (D11).

### 6.7 Keyless heartbeat

Swift has had a default-on keyless heartbeat (`keylessHeartbeatInterval`, 6h,
opt **out** by passing nil); Rust just gained one
(`HeartbeatOptions::default()` → `enabled: true`). C++ and JS are the two
remaining producers — there is no `heartbeat` anywhere in `include/` or
`integrations/`, so C++ under-reports keyless devices in the dashboard.

**C++ does not need Rust's `enabled` / `revalidate` split.** That split exists
because Rust's old heartbeat also called `refresh_if_needed()` on every tick, so
switching it on would have forced network revalidation into every consumer app.
In C++ the two mechanisms are already separate: `startAutoValidation()`
(`client.hpp:588`) *is* the revalidating loop, and it is opt-in — the integrator
calls it. The heartbeat is a distinct, beacon-only mechanism.

- `Config::keylessHeartbeatIntervalMs`, default 6h. `0` disables it.
- **Beacon only.** It calls `reportKeylessState` and never revalidates.
- The state argument is derived internally from `state()`; an automatic tick has
  no caller to supply one.
- The existing 24h debounce inside `reportKeylessState`
  (`client.hpp:460-467`) still applies, so a 6h tick is usually a no-op.
- Reuses the `startAutoValidation()` thread pattern: interruptible
  `condition_variable` wait, joined in the destructor.

**Lazily spawned, not constructor-spawned.** On by default would otherwise mean
a thread per `Client`, and AU/VST3 hosts instantiate and discard plugin
instances repeatedly during a scan. The thread starts on the first state
resolution instead, so a discarded instance never pays for one.

## 7. Rust changes

Two fixes in `keylight-rust`, both surfaced by this audit:

- **`upgrade_url()` points at a retired route.** `keylight/src/client.rs:694`
  builds `portal.keylight.dev/p/{tenant}/upgrade/{product}?key=…`; the
  standalone upgrade form is retired (`worker/src/portal/index.ts:42`). Retarget
  it at the authenticated route, matching C++. Its "parity with Swift
  `upgradeURL`" comment is also stale — Swift has no such function.
- **`trial_duration_days` default 14 → 0.** With the server owning the number,
  trials become opt-in everywhere and a non-zero value comes from one place.
  Breaking change; needs a loud CHANGELOG entry.

## 8. Testing

- **ChaCha20-Poly1305** against RFC 8439 vectors.
- **Store**: encrypt/decrypt round-trip; a blob written under a different
  machine id fails to decrypt rather than silently returning empty.
- **Migration matrix**: plaintext only; encrypted only; both present; plaintext
  with an expired lease (lease dropped, trial kept); plaintext with trial only;
  plaintext unlinked after import.
- **`deactivate` body asserted by a fake transport** — the exact JSON keys. The
  current live test prints "seat released" without checking anything
  (`test_live.cpp:138`), which is precisely why this shipped.
- **Retry**: policy unit tests ported from `retry.rs`, plus `Retry-After`
  parsing and the clamp.
- **Trial**: effective-duration resolution order; server 14 with local 0 grants
  a trial; a start stamped while the effective duration was 0 resolves as
  expired once a duration arrives (D4/D10).
- **State**: `"fallback"` resolves to `Limited`; clock rollback; `maxOfflineDays`
  enforced at launch with a missing anchor failing closed.
- **New API**: debounce behavior for `activeRevalidate`; `refreshAfterUpgrade`
  returns on change and on timeout; `upgradeUrl` normalization.
- The existing 123 tests / 556 assertions stay green. Conformance vectors
  (`tests/fixtures/vectors.json`) are untouched.
- Amalgamation regenerated; `test_amalgamation.cpp` covers the new headers.

## 9. Breaking changes for 0.2.0

1. `State` gains `Limited`; lease status `"fallback"` now resolves to it rather
   than `Expired`.
2. `deactivate()` returns the server error instead of always succeeding.
3. The default store is now encrypted and machine-bound; plaintext stores are
   migrated once and deleted.
4. `HttpResponse` gains a trailing `headers` member (source-compatible).
5. Effective trial duration is server-owned; `Config::trialDurationDays` is
   demoted to a seed.

## Out of scope

- **Binding an instance to `machine_hash`.** This is the permanent fix for seat
  sharing, and section 4.3 explains why no amount of client-side storage work
  substitutes for it. `machine_hash` already reaches the worker on activate and
  validate, but is used only as an analytics `identity`
  (`worker/src/routes/validate.ts:181`) and never enforced. Recording it at
  activation and rejecting a validate from a different machine would close the
  gap for all five SDKs at once.
  Re-activating on migration is not a workaround — every `activate` mints a
  fresh instance and consumes a seat
  (`worker/src/services/runtime-licensing.ts:304`), so honest users would burn
  their activation limit. Needs its own design across all five SDKs.
- Swift / JS / C# consumption of `GET /config`.
- The **JS** keyless heartbeat. Same gap as C++ (`reportKeylessState` with no
  timer machinery), but a different repo and a different concurrency model.
- **C#** keyless support generally: it has no beacon at all, only
  `free_tier_instance_id` on activate, so there is nothing for a heartbeat to
  pace. Porting the beacon comes first.
- Per-key-type trial lengths and grace periods.
