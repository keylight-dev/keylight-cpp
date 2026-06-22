# Keylight — JUCE Audio Plugin Adapter

Adds Keylight license management to your JUCE VST/AU/AAX plugin with zero
extra dependencies.  Networking goes through JUCE's own `juce::URL` and
`juce::InputStream`; no OpenSSL, no cpp-httplib, nothing extra to vendor.

> **Manual verification pending:** the adapter is written against the JUCE 7/8
> public API and has been self-reviewed for correctness, but it has not yet
> been compiled inside a real JUCE plugin project.  A manual Projucer/CMake
> build is required before shipping (see [Verification](#verification) below).

---

## Minimum requirements

| Requirement       | Value                               |
|-------------------|-------------------------------------|
| JUCE              | 7.x or 8.x                         |
| C++ standard      | C++17                               |
| JUCE modules      | `juce_core` (URL, File, Thread, MessageManager) |
| Keylight C++ SDK  | any version after 0.1.0             |
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
cfg.tenantId    = "your-tenant-id";
cfg.productId   = "your-product-id";
cfg.trustedKeys = { { "kid-1", "<base64-Ed25519-public-key>" } };

licensing_ = std::make_unique<keylight::juce_integration::Licensing>(cfg);

// Optional: react to state changes on the message thread
licensing_->onStateChanged = [this](keylight::State s) { /* repaint editor */ };

// Refresh from server on launch (non-blocking — runs on a background thread)
licensing_->checkOnLaunch();
```

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

### Auto-revalidation (optional)

```cpp
// Call once after construction (validates every 30 min in the background).
licensing_->startAutoValidation();
```

---

## Threading contract

| Method / event                          | Thread               |
|-----------------------------------------|----------------------|
| Constructor, `checkOnLaunch`, `activate`, `validate`, `deactivate` | Call from **message thread** |
| Completion callbacks (`activate`, etc.) | Delivered on **message thread** via `callAsync` |
| `onStateChanged`                        | Fires on **message thread** |
| `state()`                               | **Any thread** (atomic load) |
| `hasFeature(feature)`                   | **Any thread** — audio-thread safe (atomic load) |
| `hasEntitlement(feature)` (SDK mutex)   | Message thread only  |
| `JuceUrlTransport::request()`           | Background `std::thread` only — never the audio thread |

`JuceUrlTransport::request()` calls `juce::URL::createInputStream()` which
blocks the background thread synchronously.  The audio thread and message
thread are never blocked.

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
registered callback on the background network thread.  The callback:

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

> **Manual verification pending** — compile in a JUCE 7/8 plugin project.

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
