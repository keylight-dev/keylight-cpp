# Server-owned trial length — porting handoff for Rust, Swift, JS and C#

**Status:** implemented in `keylight-cpp` 0.2.0. Not yet in any other SDK.

**Who this is for:** a session working in `keylight-rust`, `keylight-swift`,
`keylight-js` or `keylight-dotnet`. Ownership is split by repo, so each of those
is a separate piece of work; this document is the shared contract so four
independent ports do not drift.

**Reference implementation:** `keylight-cpp`, `include/keylight/client.hpp` and
`include/keylight/config_payload.hpp`. Every symbol named below exists there and
is covered by tests in `tests/test_client.cpp` and `tests/test_auth_trial.cpp`.

---

## Why this needs doing in every SDK

The server can expose the trial length all day; an SDK that does not read it
keeps using its compiled-in value and **the dashboard setting silently does
nothing for apps built with that SDK**. There is no server-side shortcut for
this particular gap — it is client code in each SDK or it does not work.

The concrete failure a tenant hits: they ship a Swift app and a C++ plugin for
one product, set 14 days in the dashboard, and get 14 days in the plugin and
whatever-was-compiled-in in the app. Nothing surfaces as an error.

---

## 1. The core change

Trial length stops being a config value the integrator sets and becomes a
setting the server owns.

**Resolution order: server value → local seed → 0 (trials off).**

The local config field is demoted to a *seed*, used only before this install has
ever reached the server. Do not remove it — a brand-new install genuinely has
nothing else, and removing it makes first-launch behaviour depend on the network.

C++ names, for reference:

| Concept | C++ |
|---|---|
| The seed | `Config::trialDurationDays` |
| The seed (free tier) | `Config::freeTierEnabled` |
| What is actually in force | `Client::effectiveTrialDurationDays()` |
| What is actually in force | `Client::effectiveFreeTierEnabled()` |
| Explicit refresh | `Client::fetchConfig()` |

Every place that previously read the config field directly must read the
effective value instead. In C++ that was three trial functions plus state
resolution; expect the same shape elsewhere.

---

## 2. Wire format

`GET {base}/{tenant}/{product}/config`:

```json
{
  "trial_duration_days": 14,
  "free_tier_enabled": true
}
```

The same two fields also ride on responses to calls that are already being made:

- **`validate`** — covers every licensed install.
- **the keyless beacon** — covers every unlicensed install.

**Read them from those responses too.** This is what keeps launch-time I/O at
zero. Do not add a config fetch to your launch path: in C++ that would mean one
network call per plugin instance per DAW scan, and the equivalent cost exists in
any host that constructs and discards clients.

Persist both values so an offline launch uses the last known settings rather
than falling back to the seed.

---

## 3. Three traps that cost real time in the C++ port

### 3.1 Absent is not zero

`0` and `false` are legitimate server values — "trials off", "no free tier".
If your persistence layer uses the common "0 means the field was absent"
shortcut, a never-fetched config reads as a deliberate no-trial setting, and
worse, a tenant who turned trials off server-side gets them re-enabled from the
seed on next launch.

Test presence explicitly. C++ checks the parsed key set.

### 3.2 The trial clock must be stamped even at a zero duration

This is the actual bug that made a dashboard-set trial do nothing, and it is
not obvious.

`startTrial()` used to return early when the duration was `<= 0`. Once the
duration is server-owned, `0` is indistinguishable from *"the config has not
arrived yet"* — so bailing out left **no start timestamp for the later-arriving
duration to measure**, and the user never got a trial at all.

Stamp unconditionally. The stamp grants nothing on its own: status and
days-remaining still report no trial while the effective duration is 0. It only
fixes *when* the window starts if a duration later arrives.

Two consequences to carry over deliberately:

- Something is now persisted where nothing was before. C++ had a test asserting
  the opposite; it was inverted with the reasoning written into it.
- The anonymous instance id is minted at the same point, so a trial that later
  converts can be attributed. Minting only when the duration is already non-zero
  loses that — an install starting offline would stamp without an id, and
  nothing calls `startTrial()` again once the duration lands.

### 3.3 An old stamp is honoured, never restarted

A tenant enabling a 14-day trial 60 days after an install already stamped must
not hand that install a fresh window. One field, one rule — otherwise it can be
farmed by reinstalling.

---

## 4. The signature — optional per SDK, but the format is not

C++ also verifies an Ed25519 signature on the config. **This part is additive
and you can skip it**: the extra fields are ignored by any SDK that does not
check them, and nothing breaks.

What you cannot do is invent a different payload. Once a verifying client is in
the wild, changing these bytes means every shipped client rejects valid configs,
and it cannot be fixed from the server.

```
cfg1|{kid}|{tenantId}|{productId}|{issuedAt}|{expiresAt}|{trialDurationDays}|{freeTierEnabled}
```

Pipe-delimited, fixed order, `freeTierEnabled` as the literal `true`/`false`.
Same shape as the `v3` lease payload, verified against the same tenant keyset.

```json
{
  "trial_duration_days": 14,
  "free_tier_enabled": true,
  "issued_at": 1781076246,
  "expires_at": 1781680000,
  "kid": "k1",
  "signature": "<base64>"
}
```

Four rules, if you do implement it:

1. **Tenant and product come from your own config, not from the body.** That is
   what makes a config signed for another product fail rather than validate
   against its own claim.
2. **A config that does not verify is never cached.** Fall back to the seed,
   never to what the server claimed.
3. **Freshness applies to the wire, not the cache.** A response off the network
   must be inside `issued_at`/`expires_at` (300s skew, same as the lease). A
   *cached* config stays usable past its window — it was verified once, and
   expiring it would cut an offline user's trial short.
4. **The same rule applies wherever the fields ride.** If `/config` verifies but
   a `validate` body caches unsigned settings, the check is one route away from
   useless. Authentication is a property of the fields, not of the endpoint.

The equivalent of C++'s `requireSignedConfig` should default to *off* until the
worker signs, or you break every tenant's dashboard setting on upgrade.

---

## 5. Do not build a client-value cross-check

An earlier design had the SDK send its configured trial length for the server to
compare against its own. **It does not work and should not be built.** A patched
client sends whatever its author wants, so a match proves nothing about the
client — and shipping it would look like a control while being none, which is
worse than not having it.

Sending the value as **telemetry** is worth doing. C++ sends
`sdk_trial_duration_days` on activate and validate — the value the *build* was
compiled with, not the effective one, since echoing the server's own number back
diagnoses nothing. It catches the ordinary mistake: a 30-day build running
against a 14-day dashboard setting, found in a minute instead of a week of
support tickets.

The server must never gate on it.

---

## 6. Tests worth porting

These are the ones that caught real bugs, rather than the ones that restate the
implementation:

- A server duration grants a trial when the seed is 0.
- A server duration of 0 turns off a seed-enabled trial (the reverse direction —
  this is the one a tenant notices).
- `startTrial()` stamps before the config lands; the trial then runs from first
  launch once a duration arrives.
- An old stamp is honoured, not restarted, when trials are enabled later.
- A server `0` survives a relaunch as `0`, not as the seed.
- An absent config falls through to the seed, not to `0`.
- A server `free_tier_enabled: false` survives against a seed of `true`.
- A response with no config fields leaves the cache alone (an older worker must
  not wipe what is known).
- Launch resolution still makes no network call on an unlicensed device.

If you implement the signature, add: a tampered duration is ignored and the seed
governs; a signature valid for another product is rejected; an expired config
from the network is rejected but one from the cache is still used; an unsigned
body is ignored when signing is required.

Fixtures need a signing key, which the SDKs deliberately do not have — they
verify only. C++ generates them with `tools/gen_config_fixtures.py` from a fixed,
published, test-only seed. Reuse that script's payload construction so the
fixtures agree across repos.

---

## 7. Open items that are not client work

- **The worker must sign `/config`** before any SDK's "require signed" default
  can flip. Additive, so it can ship before or after any client.
- **A server-side trial ledger** — recording trial start against `machine_hash`
  and answering "already used". This is the only thing that stops a patched
  client, it needs no client changes (`machine_hash` is already on the wire),
  and it protects every SDK at once. It needs a grace policy decided first: a
  device whose `machine_hash` changes looks new and gets a second trial, and a
  legitimate reinstall looks old and gets none.
