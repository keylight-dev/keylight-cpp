# Free tier & the keyless beacon — C++ SDK parity

- **Date:** 2026-09-03
- **Status:** design approved; implementation plan pending
- **Branch:** `claude/keylight-cpp-free-tier-keyless`, stacked on `claude/keylight-cpp-auth-trial-m2kizb` (PR #3)
- **Target version:** 0.1.6

## Motivation

A support ticket from an SDK integrator using the JUCE adapter raised three
problems. PR #3 fixes two of them — the unsent
`X-Keylight-SDK-Key` and the missing local trial. The third is untouched:

> I also notice a "free-tier" route in the dashboard's SDK traffic view, separate
> from validate/activate/deactivate, but no corresponding client method anywhere
> in the SDK to call it.

That is correct. The C++ SDK has no free-tier concept at all: no config flag, no
`State` value, no endpoint. This design closes that gap against the other SDKs.

## Reference implementations

| SDK | Availability | Role here |
|---|---|---|
| `keylight-rust` | full source | **authoritative.** `keylight/src/{client,state,machine,config}.rs`, `keylight/src/store/device.rs`, `keylight/tests/client_keyless.rs` |
| `keylight-swift` | **binary xcframework only** — the repo holds `Package.swift`, no sources | behavior only, read from CHANGELOG/README |

Byte-level parity claims in this document are verified against Rust. Swift
informs behavior (its CHANGELOG documents the free-tier semantics explicitly)
but cannot be diffed.

## Approved decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | Full parity including `machine_hash` | Server dedupes keyless devices by real hardware, not per-install ID |
| D2 | New PR stacked on #3, released as 0.1.6 | Keeps #3 reviewable; #3 unblocks the two reported bugs on its own |
| D3 | **Beacon is caller-driven in the core (Rust), auto-fired by the JUCE adapter (Swift)** | See below |
| D4 | Hardware ID via native OS APIs, not subprocesses | Sandboxed AU/VST3 hosts may block process spawn |
| D5 | Free tier outranks an elapsed trial | Matches Rust `resolve_state`; an expired trial should drop to free tier, not the paywall |
| D6 | Force the 64-bit registry view on Windows | Otherwise a 32-bit host silently hashes a different GUID |
| D7 | Accept new link dependencies | IOKit/CoreFoundation on Apple, advapi32 on Windows |

### D3 in detail

Rust and Swift genuinely diverge, and this is the one place the design is not a
straight port.

- **Rust** declares `pub fn report_keyless_state(&self, state: KeylessState)`
  and never calls it internally — the only call sites in the repo are tests.
- **Swift** fires it automatically from `LicenseManager` on state transitions.

C++ follows Rust in `keylight::Client`, because PR #3 established — and tests —
that `checkOnLaunch()` performs **zero** network calls when no license is
stored. Auto-reporting on transitions fires exactly there, silently revoking a
guarantee a JUCE integrator now relies on during DAW plugin scans.

The funnel still needs data, so `KeylightJuce::Licensing` auto-reports on its
**existing** background dispatch, off both the message and audio threads. The
core stays faithful to Rust and side-effect-free; the platform adapter does what
Swift's `LicenseManager` does. No third core behavior is invented.

## Wire contract

```
POST {apiBaseUrl}/{tenantId}/{productId}/keyless
Headers: Content-Type: application/json, X-Keylight-SDK-Key: <sdkKey>
Body:    {"instance_id": "<uuid-v4>",
          "state": "trial" | "free_tier" | "expired",
          "machine_hash": "<64-hex>",        // omitted when no hardware ID
          ... existing device telemetry (app_version, cpu_cores, memory) }
```

The response body is ignored. Only `status == 200` matters: it gates whether the
debounce markers are persisted.

Two existing routes gain fields, for conversion attribution (Rust `client.rs:257`, `:330`):

- `activate` — `free_tier_instance_id` (when one exists) and `machine_hash`
- `validate` — `machine_hash`

Note the asymmetry, which is deliberate and matches Rust: the keyless route
spells the field `instance_id`, `activate` spells the same value
`free_tier_instance_id`.

## Components

### 1. `include/keylight/machine_id.hpp` (new)

`detail::read_hardware_id() -> std::optional<std::string>`

| Platform | Source | Notes |
|---|---|---|
| macOS | IOKit — `IOPlatformExpertDevice` → `IOPlatformUUID` | via `IOServiceGetMatchingService` + `IORegistryEntryCreateCFProperty` |
| Windows | `RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", …)` | **must** pass `RRF_SUBKEY_WOW6464KEY` (D6) |
| Linux | `/etc/machine-id`, then `/var/lib/dbus/machine-id` | trimmed; empty → `nullopt` |
| other | — | `nullopt` |

Returns `nullopt` rather than any fallback. A random per-install ID must never
be substituted — it would defeat the cross-install dedup the hash exists for
(Rust `store/device.rs`, `DeviceIdentity::hardware_id` doc comment).

### 2. `machine_hash`

```
sha256_hex("keylight-keyless-machine-v1|" + tenantId + "|" + productId + "|" + stableId)
```

`keylight::sha256_hex` already exists (`sha256.hpp:189`) and returns 64-char
lowercase hex — no new crypto code. Asserted against the cross-SDK canonical
vector Rust pins in `machine.rs`:

```
machine_hash("testco", "testapp", "hardware-1")
  == "8e8871112f28cabda180ada131d0b4f4f07c72fb47c5d884edbe32812885b22a"
```

### 3. `Config`

```cpp
bool freeTierEnabled = false;   // parity: Rust free_tier_enabled
```

### 4. `State::FreeTier`

Appended **after** `Invalid` so existing enumerator values do not renumber.
This is still a new value in a public enum: exhaustive `switch` statements in
integrator code will warn. Called out in the CHANGELOG as the one breaking-ish
change in 0.1.6.

### 5. `KeylessState`

```cpp
enum class KeylessState { Trial, FreeTier, Expired };
// wire: "trial" | "free_tier" | "expired"
```

### 6. Client API

```cpp
void        reportKeylessState(KeylessState state);  // errors swallowed
std::string freeTierInstanceId();                    // mint-on-demand, persisted
```

`reportKeylessState` (Rust `client.rs:525`):

1. Skip when the state is unchanged **and** the last ping is under 86400s old.
2. Resolve or mint `freeTierInstanceId`; bail out if it cannot be persisted.
3. Build the body; attach `machine_hash` only when a hardware ID resolves.
4. POST. **Persist `keylessLastState` and `lastKeylessPingAt` only on HTTP 200**,
   so a failed beacon cannot suppress reporting for a day.
5. Swallow every error — anonymous best-effort.

`startTrial()` also mints the free-tier instance ID (Rust `client.rs:483`); PR #3's
version does not, and that gap breaks trial→paid attribution.

### 7. Persistence

Four fields added to the single JSON blob, all written through PR #3's
`build_blob_locked_()` so no path can drop them:

`freeTierInstanceId`, `keylessLastState`, `lastKeylessPingAt`, `cachedHardwareId`.

`cachedHardwareId` mirrors Rust's `cached_hardware_id()`: read live, write
through on success, fall back to the cached value when a live read fails. This
keeps the hash stable across transient `ioreg`/registry failures — Rust pins the
behavior with a `FlakyOnceDevice` test.

### 8. State resolution

Extends PR #3's resolver, mirroring Rust `state.rs:60-74`:

| Condition | Resolved state |
|---|---|
| trusted active lease, current | `Licensed` |
| lease `fallback` or `expired` | `Expired` *(unchanged — C++ has no `Limited`)* |
| a license key is stored | `Expired` |
| trial active | `Trial` |
| **`freeTierEnabled`** | **`FreeTier`** |
| otherwise | `Invalid` |

Two consequences, both matching Rust and Swift:

- An **elapsed** trial with free tier enabled resolves `FreeTier`, not `Expired`
  (D5). This changes behavior PR #3 just introduced.
- `deactivate()` on a free-tier product resolves `FreeTier` rather than the
  paywall — this falls out of the table, no special case. Swift documents the
  same fix in its 0.7.0 CHANGELOG.

### 9. Test seam

A new five-argument constructor **overload**, mirroring the existing `now_fn`
idiom. Both current constructors remain and delegate to it with
`detail::read_hardware_id`, so no existing call site changes:

```cpp
Client(Config, Transport&, LicenseStore&,
       std::function<int64_t()> now_fn,
       std::function<std::optional<std::string>()> hardware_id_fn);
```

Required to assert the canonical vector with a fixed `"hardware-1"`, and to
model "no hardware ID available" and flaky reads.

### 10. JUCE adapter

- `Licensing::reportKeylessState(KeylessState)` — background dispatch, never `processBlock`.
- Auto-report on state transitions from the existing `onStateChanged` path (D3).
- `citest/main.cpp` gains compile+link coverage.

## Testing

Ported from Rust `tests/client_keyless.rs`, plus C++-specific persistence cases:

- **Hash** — canonical vector; `machine_hash` omitted when no hardware ID; cached ID survives a read that succeeds once then fails.
- **Debounce** — unchanged state within 24h sends nothing; a changed state sends immediately; >24h sends; a non-200 response leaves the markers unwritten so the next call retries.
- **Wire** — body carries `instance_id` + `state` with the right wire strings; the SDK-key header is present; the URL is `/{tenant}/{product}/keyless`.
- **State** — `FreeTier` when enabled and no trial; free tier beats an *elapsed* trial; a paid license still wins; `deactivate()` lands on `FreeTier`; disabled free tier still resolves `Invalid`.
- **Attribution** — `startTrial()` mints the instance ID; `activate` carries `free_tier_instance_id` and `machine_hash`; `validate` carries `machine_hash`.
- **Persistence** — all four fields survive activate / validate / lease refresh / revoke, per PR #3's serializer.

Each new behavior gets a mutation check: break the line, confirm the intended
tests — and only those — fail.

## Build and packaging

- CMake: link `IOKit` + `CoreFoundation` on Apple, `advapi32` on Windows, on the
  `keylight` INTERFACE target. The header currently advertises **"zero external
  dependencies"**; that comment and the README must change.
- `keylight_single.hpp` regenerated; `tools/amalgamate.py` must pick up `machine_id.hpp`.
- `test_amalgamation.cpp` gains a keyless case so the shipped single header is exercised.
- Version 0.1.6 in `version.hpp`, `vcpkg.json`, `conanfile.py`; README free-tier section.

## Risks

- **New link dependencies** (D7) — the biggest consumer-visible change. JUCE apps already link IOKit, so it is close to free for the reporting integrator, but plain CMake consumers see a new requirement.
- **WOW64 redirection** (D6) — silent, and only reproduces in a 32-bit host. Needs an explicit comment at the call site; CI cannot catch it.
- **`State::FreeTier`** — new public enum value; integrator `switch` statements warn.
- **D5 changes PR #3 behavior** — an elapsed trial now resolves `FreeTier` when free tier is on. Only affects products that opt in.

## Out of scope

- `State::Limited` — Rust has it for `fallback` leases; C++ maps those to `Expired`. Preserved as-is; changing it is unrelated to free tier.
- Retry/backoff — Rust routes the beacon through a retrying `post()`. C++ has no retry layer at all; the beacon fires once and gives up. Pre-existing gap, flagged not fixed.
- `X-Keylight-Request-Id` — pre-existing C++ gap noted in PR #3.
