## [0.1.0] - unreleased

### Added
- Unreal Engine plugin (`integrations/unreal/Keylight/`): `UKeylightSubsystem`
  (Blueprint-callable `Activate`/`Validate`/`Deactivate`/`HasEntitlement`/`GetState`
  with `FOnKeylightResult` async delegates), `FHttpTransport` (UE `FHttpModule`
  adapter for `keylight::Transport` — blocks a background thread via `FEvent`,
  never the game thread), and `UELicenseStore` (lease persistence under
  `Saved/Keylight/`).  The plugin compiles against UE 5.x headers with zero
  extra dependencies beyond `Core`, `CoreUObject`, `Engine`, `HTTP`, and `Json`.
  **Manual editor build pending:** no UE toolchain is available in CI; a
  developer with UE 5.x installed must compile and smoke-test before shipping.
