#pragma once

// ---------------------------------------------------------------------------
// The only file you need to edit to make this plugin yours.
//
// Every value here comes from your Keylight dashboard at
// https://app.keylight.dev — Product settings, then the SDK tab.
// ---------------------------------------------------------------------------

namespace starter_config
{
    // Your tenant and the product you are licensing. Both are visible in the
    // dashboard URL and on the product's SDK tab.
    inline constexpr const char* kTenantId  = "your-tenant-id";
    inline constexpr const char* kProductId = "your-product-id";

    // The SDK key authenticates your plugin's calls to the Keylight API.
    // It is not a secret in the "must never ship" sense — it goes in the
    // binary — but treat it as a per-product value, not a shared one.
    inline constexpr const char* kSdkKey = "your-sdk-key";

    // The Ed25519 public key your plugin verifies leases against, as
    // (key id -> base64 public key). Compile this in rather than fetching it:
    // a keyset fetched over the same connection an attacker controls can be
    // forged by that same attacker.
    inline constexpr const char* kTrustedKeyId = "k1";
    inline constexpr const char* kTrustedKey   = "your-base64-ed25519-public-key";

    // How long the plugin keeps working without reaching the server. The
    // dashboard is the real owner of licensing policy; this is the offline
    // window your build is willing to honour.
    inline constexpr int kMaxOfflineDays = 7;

    // Seed values used by a fresh install before its first server check.
    // Once the plugin talks to Keylight, the dashboard's values win — which
    // is what lets you change the trial length without shipping a new build.
    inline constexpr int  kSeedTrialDurationDays = 14;
    inline constexpr bool kSeedFreeTierEnabled   = true;

    // The feature flag that separates the demo path from the paid path.
    // Signed into the lease, so it is readable offline.
    //
    // Keep this as "pro". The adapter's audio-thread-safe hasFeature() has a
    // dedicated lock-free flag for that exact string; any other name falls
    // through to the generic entitlement path, which is not what you want on
    // the audio thread. Gate richer tiers off hasEntitlement() from the
    // message thread instead.
    inline constexpr const char* kProFeature = "pro";
}
