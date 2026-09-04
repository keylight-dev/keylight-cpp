# Keylight — JUCE Audio Plugin Adapter

Adds Keylight license management to your JUCE VST/AU/AAX plugin with zero
extra dependencies.  Networking goes through JUCE's own `juce::URL` and
`juce::InputStream`; no OpenSSL, no cpp-httplib, nothing extra to vendor.

> **Compile-verified in CI; live round-trip still manual.** Every push builds
> this adapter against real JUCE (7.0.12 **and** 8.0.6) on Linux/macOS/Windows
> via `.github/workflows/juce.yml`, and runs an offline smoke test of the
> audio-thread-safe API. What CI does *not* yet cover: building a real VST/AU
> plugin with it and a live `activate → Licensed` round-trip against the API
> (see [Verification](#verification) below).

---

## Minimum requirements

| Requirement       | Value                               |
|-------------------|-------------------------------------|
| JUCE              | 7.x or 8.x                         |
| C++ standard      | C++17                               |
| JUCE modules      | `juce_core` (URL, File, Thread), `juce_events` (MessageManager) |
| Keylight C++ SDK  | 0.1.5 or newer (trial API + SDK-key auth) |
| Target platforms  | macOS, Windows, Linux (any platform juce::URL supports) |

---

## Installation

### 1. Get the C++ SDK headers

The adapter uses the Keylight C++ SDK headers.  The SDK is header-only at
this boundary (no static lib to link):

```
git clone https://github.com/keylight-dev/keylight-cpp \
    <YourPlugin>/ThirdParty/keylight-cpp
```

Expected layout:

```
<YourPlugin>/
├── Source/
│   ├── PluginProcessor.h
│   ├── PluginProcessor.cpp
│   └── KeylightJuce.h          ← copy from integrations/juce/
└── ThirdParty/
    └── keylight-cpp/
        └── include/
            └── keylight/
                ├── client.hpp
                ├── transport.hpp
                └── ...
```

### 2. Copy the header

```
cp integrations/juce/KeylightJuce.h  <YourPlugin>/Source/KeylightJuce.h
```

### 3. Add the SDK include path

**Projucer:** In Module Settings → Header Search Paths add:
```
../../ThirdParty/keylight-cpp/include
```

**CMake** (JUCE 7+ CMake API):
```cmake
target_include_directories(YourPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/keylight-cpp/include)
```

### 4. Include and use

```cpp
#include "KeylightJuce.h"

using Licensing = keylight::juce_integration::Licensing;
```

---

## Usage

### Construct in AudioProcessor

```cpp
// PluginProcessor.h
#include "KeylightJuce.h"

class MyAudioProcessor : public juce::AudioProcessor
{
    std::unique_ptr<keylight::juce_integration::Licensing> licensing_;
    // ...
};
```

```cpp
// PluginProcessor.cpp constructor
keylight::Config cfg;
cfg.tenantId          = "your-tenant-id";
cfg.productId         = "your-product-id";
cfg.sdkKey            = "sdk_live_...";   // sent as X-Keylight-SDK-Key
cfg.trialDurationDays = 14;               // 0 (default) disables trials
cfg.trustedKeys = { { "kid-1", "<base64-Ed25519-public-key>" } };

licensing_ = std::make_unique<keylight::juce_integration::Licensing>(cfg);

// Optional: react to state changes on the message thread
licensing_->onStateChanged = [this](keylight::State s) { /* repaint editor */ };

// Refresh from server on launch (non-blocking — runs on a background thread).
// This resolves an already-started local trial offline; it never starts one,
// so a DAW scanning your plugin cannot consume the user's trial.
licensing_->checkOnLaunch();
```

### Start a trial (local, offline-first)

Trials are entirely local: `startTrial()` writes a start timestamp next to the
lease and the window is measured against the local clock — no API call, and no
dependency on the free-tier/keyless feature. Call it only when the user asks
for a trial (a "Start free trial" button), never on load.

```cpp
// In your PluginEditor button handler (message thread):
processor.licensing().startTrial(
    [this](keylight::Result<keylight::State> result)
    {
        // Back on the message thread. State::Trial while the window is open,
        // State::Expired once it has elapsed, State::Licensed if a paid
        // license is already active (paid licensing always wins).
        if (result.is_ok())
            statusLabel.setText(juce::String(processor.licensing().trialDaysLeft())
                                    + " days left",
                                juce::dontSendNotification);
    });
```

`startTrial()` is idempotent — an existing trial start is never overwritten, so
an elapsed trial cannot be restarted, and activating then deactivating a paid
license leaves the original trial clock untouched. Trial state changes flow
through `onStateChanged` and the `state()` snapshot like every other
transition, so `processBlock` keeps reading a single atomic.

`trialStatus()` (`NotStarted` / `Active` / `Expired`) and `trialDaysLeft()` are
UI queries — call them from the message thread, not from `processBlock`.

### Free tier (optional)

Set `cfg.freeTierEnabled = true` and a device with no licence and no active
trial resolves `keylight::State::FreeTier` instead of `Invalid`. An **elapsed**
trial resolves `FreeTier` too — a lapsed trial drops to the free tier rather
than the paywall — and `deactivate()` lands there as well.

```cpp
cfg.freeTierEnabled = true;
...
switch (licensing.state())
{
    case keylight::State::Licensed: /* everything */             break;
    case keylight::State::Trial:    /* everything, time-boxed */ break;
    case keylight::State::Limited:  /* degraded, not locked */   break;
    case keylight::State::FreeTier: /* reduced feature set */    break;
    case keylight::State::Expired:
    case keylight::State::Invalid:  /* paywall */                break;
}
```

**`State::FreeTier` and `State::Limited` are new enumerators.** An exhaustive
`switch` over `keylight::State` compiled with `-Werror=switch` will stop
building until you add both cases. They are appended after `Invalid`, so the
existing values do not renumber.

#### The keyless beacon

The adapter reports an anonymous funnel signal (`trial` / `free_tier` /
`expired`) on every state transition. You do not call
it. It carries a random per-install id and, where the OS exposes one, a one-way
hash of a machine identifier — never a licence key, and never a raw device id.
It is debounced to one request per 24 hours per state, and a failed request is
never recorded, so it retries rather than going quiet for a day.

`keylight::Client` itself never sends it. That is deliberate: `checkOnLaunch()`
makes no network request when no licence is stored, so a DAW scanning your
plugin does not phone home through the SDK. The adapter opts into reporting on
your behalf; use `keylight::Client` directly if you would rather it did not.

`Licensing::reportKeylessState()` exists for hosts that resolve state
themselves. It is a blocking network call dispatched to the background thread —
never call it from `processBlock`, and do not call it from inside
`onStateChanged`, since that transition already reports itself.

### Gate a feature in processBlock

```cpp
void MyAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer&)
{
    // hasFeature reads a std::atomic<bool> — no lock, no allocation.
    // Safe on the audio real-time thread.
    if (!licensing_->hasFeature("pro"))
    {
        buffer.clear();   // free-tier limitation
        return;
    }
    // ... pro processing ...
}
```

### Activate from the editor

```cpp
// In your PluginEditor button handler (message thread):
processor.licensing().activate(licenseKeyField.getText(),
    [this](keylight::Result<keylight::State> result)
    {
        // Callback fires on the message thread — safe to update UI.
        if (result.is_ok() && result.value() == keylight::State::Licensed)
            statusLabel.setText("Licensed!", juce::dontSendNotification);
        else
            statusLabel.setText("Activation failed", juce::dontSendNotification);
    });
```

### Deactivate

```cpp
processor.licensing().deactivate(
    [this](keylight::Result<void> result)
    {
        // Back on the message thread.
        (void)result;
    });
```

### Auto-revalidation — call this

```cpp
// Call once after construction (validates every 30 min in the background).
licensing_->startAutoValidation();
```

Without a periodic tick the SDK cannot notice **anything** mid-session: a
dashboard revoke, a lease expiring, or a system clock rolled back. All three
are detected on a refresh, and a plugin instance can live for days.

The clock-rollback guard is the one worth spelling out. `Licensing::state()`
and `hasFeature()` read atomics that are updated from the SDK's state-change
events, and a clock that moves changes no underlying state — so the SDK raises
that event from `refreshIfNeeded()` or `validate()`, and `startAutoValidation()`
ticks the former. Skip the tick and a mid-session rollback reaches
`underlying().state()` but never reaches the audio thread, which keeps
reporting the state it last saw.

We do not start it for you: DAWs instantiate plugins while scanning, and the
SDK deliberately makes no network call on that path.

---

## Threading contract

| Method / event                          | Thread               |
|-----------------------------------------|----------------------|
| Constructor, `checkOnLaunch`, `activate`, `validate`, `deactivate`, `startTrial` | Call from **message thread** |
| Completion callbacks (`activate`, etc.) | Delivered on **message thread** via `callAsync` |
| `onStateChanged`                        | Fires on **message thread** |
| `state()`                               | **Any thread** (atomic load) |
| `hasFeature(feature)`                   | **Any thread** — audio-thread safe (atomic load) |
| `hasEntitlement(feature)` (SDK mutex)   | Message thread only  |
| `trialStatus()` / `trialDaysLeft()` (SDK mutex) | Message thread only  |
| `JuceUrlTransport::request()`           | Background `std::thread` only — never the audio thread |
| `~Licensing()`                          | **Message thread** — may block; see below |

### Destruction can block the message thread

`~Licensing()` waits on **two** threads, and the likelier one is not the SDK's.

**Your in-flight request (the one you will actually hit).** `activate()`,
`validate()` and `deactivate()` run on a worker thread this class owns, and the
destructor joins it. If the user taps *Activate* and then closes the session
before the response arrives, teardown waits out the whole HTTP round trip on
the message thread. Nothing cancels it — `JuceUrlTransport` has no interrupt.
That is a button press away, not a timing coincidence, so if teardown latency
matters, do not let a session close with a request in flight.

**The SDK's auto-validation worker.** `~keylight::Client` joins it, which is
what guarantees no worker outlives the client. This one is **usually free**:
the destructor wakes the worker before it joins, so one parked in its interval
wait exits without another cycle, and on the default 30-minute interval it is
parked essentially all the time. It costs only when that worker is mid-cycle —
up to one network round trip, plus every listener callback for every queued
event if it happens to be the thread delivering. **Your listeners set that
ceiling.** Nothing in the SDK caps how long one may take, so a slow listener
stalls teardown for as long as it likes. Keep them short, or hand off to your
own thread.

`stopAutoValidation()` does **not** shorten either wait. Since 0.2.0 it retires
the worker and returns immediately; whatever waiting is left moved to the
destructor.

`JuceUrlTransport::request()` calls `juce::URL::createInputStream()`, which
blocks synchronously on whichever thread runs it. The audio thread never runs
it. The message thread never runs it either — but it does *join* it during
teardown, per the first paragraph above.

---

## Audio-thread safety — how the atomic snapshot works

The audio thread (`processBlock`) must never block, allocate, or lock.
`hasFeature("pro")` must therefore not touch any mutex or heap allocation.

The adapter achieves this with two `std::atomic` fields inside `Licensing`:

```
std::atomic<keylight::State>  state_snapshot_
std::atomic<bool>             pro_enabled_
```

These are updated by an SDK event subscription registered in the `Licensing`
constructor.  Whenever the `keylight::Client` transitions state — after
activate, validate, auto-validation, or offline-grace expiry — it fires the
registered callback on whichever thread is draining the SDK's event queue —
usually this class's background thread, but under concurrency it can be another
one, and it is never the audio thread.  The callback:

1. Stores the new state into `state_snapshot_` (relaxed atomic store).
2. Calls `client_->hasEntitlement("pro")` (with mutex, off the audio thread)
   and stores the result into `pro_enabled_` (relaxed atomic store).
3. Posts a `juce::MessageManager::callAsync` to deliver `onStateChanged` to
   the message thread for UI updates.

The audio thread only ever executes:
```cpp
return pro_enabled_.load(std::memory_order_relaxed);
```
No lock.  No allocation.  No JUCE string construction in the hot path.

### Gating on multiple entitlements

`hasFeature("pro")` uses the pre-cached `pro_enabled_` atomic.  For other
feature keys, it falls back to `generic_entitlement_enabled_` (also atomic).
If your plugin needs multiple independent feature flags, extend `Licensing`
(or subclass it) by adding your own `std::atomic<bool>` fields and populating
them inside a custom `onStateChanged` lambda before passing it to the base
subscription.  The pattern is:

```cpp
// After constructing licensing_:
licensing_->onStateChanged = [this](keylight::State)
{
    // These calls happen on the message thread — NOT the audio thread.
    myProAtomic_.store(licensing_->hasEntitlement("pro"), std::memory_order_relaxed);
    myUltraAtomic_.store(licensing_->hasEntitlement("ultra"), std::memory_order_relaxed);
};
```

Then in `processBlock`:
```cpp
if (myProAtomic_.load(std::memory_order_relaxed)) { ... }
```

---

## License store location

The `.lease` file is written atomically (temp-file → rename) by the SDK's
`FileStore`.  Default paths:

| Platform | Path |
|----------|------|
| macOS    | `~/Library/Application Support/Keylight/<tenantId>-<productId>.lease` |
| Windows  | `%APPDATA%\Keylight\<tenantId>-<productId>.lease` |
| Linux    | `~/.local/share/Keylight/<tenantId>-<productId>.lease` (via `userApplicationDataDirectory`) |

Override by passing a `storePath` to the `Licensing` constructor.

---

## Verification

> **Compiled in CI** (`.github/workflows/juce.yml`) against JUCE 7.0.12 and
> 8.0.6 on Linux/macOS/Windows, with an offline smoke test of the query API
> (`integrations/juce/citest/`). A live plugin round-trip is still manual.

The adapter code is written against the JUCE 7/8 public API
(`juce::URL`, `juce::URL::InputStreamOptions`, `juce::File::getSpecialLocation`,
`juce::MessageManager::callAsync`, `juce::MemoryBlock`)
and the Keylight C++ SDK public API (`keylight::Client`, `keylight::Transport`,
`keylight::Config`, `keylight::FileStore`, `keylight::Subscription`).

No JUCE toolchain is present in the keylight-cpp CI environment.  Before
shipping to end users, a developer with JUCE installed must:

1. Copy `KeylightJuce.h` into the plugin project and add the SDK include path.
2. Add a `Licensing` member to the `AudioProcessor`, call `checkOnLaunch()`.
3. Build (Projucer or JUCE CMake) for at least one target (VST3 or AU).
4. Confirm: compiles cleanly with no warnings, round-trip `activate → state()`
   returns `Licensed`, `hasFeature("pro")` returns `true` in `processBlock`
   without any thread-safety warnings from TSan.
