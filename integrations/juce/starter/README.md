# Keylight JUCE starter plugin

A complete, buildable VST3/AU/Standalone plugin with Keylight licensing and a
demo mode already wired in. Clone it, paste four values from your dashboard,
and you have a licensed plugin you can sell.

The DSP is deliberately trivial — a gain trim on the free path and a soft clip
on the pro path — so the licensing structure is the only interesting thing in
the file. Replace `applyFreePath` and `applyProPath` with your own processing
and none of the licensing wiring changes.

> **Status: not yet compiled against a real JUCE toolchain.** The API surface
> it uses is taken from `../KeylightJuce.h` and the SDK headers in
> `../../../include/`, but this template has not been through the JUCE CI
> matrix the adapter itself has. Treat the first build as the verification.

---

## What it demonstrates

| Pattern | Where |
|---|---|
| Licensing member owned by the processor | `Source/PluginProcessor.h` |
| Lease verified offline at construction, never blocking | `KeylightStarterProcessor()` |
| Trial started automatically on first run | `checkOnLaunch` callback |
| Audio-thread feature gate — one lock-free atomic read | `processBlock` |
| Sample-counted demo burst, no clock and no allocation | `applyDemoBurst` |
| Activation UI with the callback on the message thread | `Source/PluginEditor.cpp` |
| Every licensing state given its own UI, including `Limited` | `refreshLicensingUI` |

---

## Setup

### 1. Fill in your dashboard values

Everything you need to change lives in `Source/KeylightConfig.h`:

```cpp
inline constexpr const char* kTenantId  = "your-tenant-id";
inline constexpr const char* kProductId = "your-product-id";
inline constexpr const char* kSdkKey    = "your-sdk-key";

inline constexpr const char* kTrustedKeyId = "k1";
inline constexpr const char* kTrustedKey   = "your-base64-ed25519-public-key";
```

All five are on your product's SDK tab at
[app.keylight.dev](https://app.keylight.dev).

Compile the trusted key in rather than fetching it at runtime. A keyset
fetched over a connection an attacker controls can be forged by that same
attacker.

### 2. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

JUCE 8.0.6 is fetched automatically. If your `keylight-cpp` checkout is not
the grandparent of this directory, point at it:

```bash
cmake -B build -DKEYLIGHT_SDK_DIR=/path/to/keylight-cpp
```

Prefer to vendor a single file instead? Drop the amalgamated
`keylight_single.hpp` into `Source/` and include that — the rest of the
template is unchanged.

---

## The two rules that matter for audio

**Never block the audio thread.** `processBlock` runs under real-time
constraints. The only licensing call in it is `hasFeature()`, which is a
single lock-free atomic load — no allocation, no mutex, no filesystem, no
network. Everything expensive already happened on a background thread.

**Never let a DAW wait on the network.** `checkOnLaunch()` returns
immediately and resolves in the background. A host scanning plugins at
startup instantiates and destroys yours repeatedly; a blocking constructor
turns that scan into a hang.

---

## Tuning the demo

The interruption timing lives at the top of `PluginProcessor.cpp`:

```cpp
constexpr double kSecondsBetweenBursts = 45.0;
constexpr double kBurstSeconds         = 0.35;
```

Everything is counted in samples so the interval stays correct at any sample
rate, and no clock is read on the audio thread.

The **trial length** is deliberately not here. It lives in your Keylight
dashboard, and every SDK reads it at launch — so you can change 14 days to 7
after you have real conversion data, and every existing install picks it up on
its next launch with no rebuild and no recall. The seed values in
`KeylightConfig.h` only apply to a fresh install before its first server check.

---

## Further reading

- [Licensing for Audio Plugins (VST/AU)](https://keylight.dev/licensing-for-audio-plugins)
- [Offline Licensing for Your VST/AU Plugin Without a Backend](https://keylight.dev/blog/audio-plugin-licensing-cpp/)
- [The JUCE adapter reference](../README.md)
