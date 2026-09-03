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
 * the audio-thread-safe query API (state(), hasFeature()) plus the local
 * trial API (trialStatus(), trialDaysLeft(), startTrial()).  It never calls
 * activate()/validate()/deactivate(), so no network and no demo credentials
 * are required.  With no lease on disk and no trial configured, the expected
 * state is Invalid and hasFeature("pro") is false.
 */

#include "KeylightJuce.h"

#include <iostream>
#include <string>

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

    // Trial query API (compile + link check).  cfg.trialDurationDays is 0 here,
    // so trials are disabled: NotStarted with zero days left, and nothing
    // implicitly starts one.
    const keylight::TrialStatus trial = licensing.trialStatus();
    const int                   days  = licensing.trialDaysLeft();

    std::cout << "KeylightJuce smoke: state=" << static_cast<int>(st)
              << " hasFeature(pro)=" << pro
              << " hasEntitlement(pro)=" << entitled
              << " trialStatus=" << static_cast<int>(trial)
              << " trialDaysLeft=" << days << "\n";

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

    if (trial != keylight::TrialStatus::NotStarted || days != 0)
    {
        std::cerr << "FAIL: expected no trial when trialDurationDays == 0\n";
        return 5;
    }

    // Licensing::startTrial() is a compile + link check only: it dispatches to
    // a background thread that finishes on juce::MessageManager::callAsync,
    // and this headless console app runs no message loop to deliver into.
    using StartTrialFn = void (keylight::juce_integration::Licensing::*)(
        std::function<void(keylight::Result<keylight::State>)>);
    const StartTrialFn startTrialFn =
        &keylight::juce_integration::Licensing::startTrial;
    (void) startTrialFn;

    // The underlying SDK call is safe to run here — it is purely local (no
    // network, no callAsync) and a no-op while trials are disabled.
    const auto trialResult = licensing.underlying().startTrial();
    if (!trialResult.is_ok() || trialResult.value() != keylight::State::Invalid
        || licensing.trialStatus() != keylight::TrialStatus::NotStarted)
    {
        std::cerr << "FAIL: startTrial must be a no-op when trials are disabled\n";
        return 6;
    }

    // Keyless beacon: compile + link check ONLY, on both the adapter method and
    // the SDK method underneath.
    //
    // Neither is invoked. This test is offline by contract and Licensing owns a
    // real transport, so calling reportKeylessState() would attempt a live
    // request against a bogus tenant. The beacon is not gated on
    // freeTierEnabled — keylight-rust does not gate it either, since trial and
    // expired are keyless states that exist without a free tier — so there is
    // no configuration that makes a real call safe here. Behaviour is covered
    // by tests/test_free_tier.cpp against a recording transport.
    using ReportFn = void (keylight::juce_integration::Licensing::*)(
        keylight::KeylessState, std::function<void()>);
    const ReportFn reportFn =
        &keylight::juce_integration::Licensing::reportKeylessState;
    (void) reportFn;

    using SdkReportFn = void (keylight::Client::*)(keylight::KeylessState);
    const SdkReportFn sdkReportFn = &keylight::Client::reportKeylessState;
    (void) sdkReportFn;

    // The wire strings are pure and safe to assert offline.
    if (std::string(keylight::keyless_state_wire(keylight::KeylessState::FreeTier))
        != "free_tier")
    {
        std::cerr << "FAIL: keyless wire string mismatch\n";
        return 7;
    }

    std::cout << "KeylightJuce smoke test OK\n";
    return 0;
}
