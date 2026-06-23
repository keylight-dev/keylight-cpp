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
