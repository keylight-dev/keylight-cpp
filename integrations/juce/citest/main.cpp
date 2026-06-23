/*
 * main.cpp — JUCE adapter compile + smoke test (CI only)
 *
 * This is NOT a shippable plugin.  It exists so CI can prove that
 * integrations/juce/KeylightJuce.h actually compiles and links against a real
 * JUCE toolchain (juce_core + juce_events) on Linux/macOS/Windows, across both
 * JUCE 7 and JUCE 8 — closing the "manual verification pending" gap for the
 * JUCE adapter without anyone needing JUCE installed locally.
 *
 * It runs entirely OFFLINE: it constructs a Licensing instance and exercises
 * the audio-thread-safe query API (state(), hasFeature()).  It never calls
 * activate()/validate()/deactivate(), so no network and no demo credentials
 * are required.  With no lease on disk and no trial configured, the expected
 * state is Invalid and hasFeature("pro") is false.
 */

#include "KeylightJuce.h"

#include <iostream>

int main()
{
    keylight::Config cfg;
    cfg.tenantId  = "ci-compile-test-tenant";
    cfg.productId = "ci-compile-test-product";
    // A syntactically valid 32-byte base64 Ed25519 public key. It is never
    // used to verify a signature in this offline smoke test — it only has to
    // satisfy the Config shape.
    cfg.trustedKeys = {
        { "kid-1", "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=" }
    };
    cfg.maxOfflineDays = 7;

    // Explicit store path in the current working directory so the test is
    // deterministic and never touches the real user-application-data dir.
    // The file does not exist → Client loads no lease → State::Invalid.
    keylight::juce_integration::Licensing licensing(
        cfg, juce::String("keylight-juce-citest.lease"));

    // onStateChanged is part of the public surface — assigning it must compile.
    licensing.onStateChanged = [](keylight::State) {};

    // Audio-thread-safe reads (the only API processBlock is allowed to call).
    const keylight::State st  = licensing.state();
    const bool            pro = licensing.hasFeature("pro");

    // Message-thread entitlement query (compile + link check).
    const bool entitled = licensing.hasEntitlement("pro");

    std::cout << "KeylightJuce smoke: state=" << static_cast<int>(st)
              << " hasFeature(pro)=" << pro
              << " hasEntitlement(pro)=" << entitled << "\n";

    if (st != keylight::State::Invalid)
    {
        std::cerr << "FAIL: expected State::Invalid for a fresh offline client\n";
        return 2;
    }
    if (pro)
    {
        std::cerr << "FAIL: expected hasFeature(pro)==false with no lease\n";
        return 3;
    }
    if (entitled)
    {
        std::cerr << "FAIL: expected hasEntitlement(pro)==false with no lease\n";
        return 4;
    }

    std::cout << "KeylightJuce smoke test OK\n";
    return 0;
}
