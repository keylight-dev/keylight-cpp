#pragma once
// keylight/lifecycle.hpp — license state and lifecycle transitions.
// lifecycle_event is ported from keylight-rust keylight/src/state.rs.
//
// The transition table is cross-SDK. Changing an arm here diverges C++ from
// Rust and Swift silently, so it is pinned by tests rather than reasoned about
// at each call site.
//
// `State` lives here rather than in client.hpp because two headers now need
// it and it has no dependencies of its own. client.hpp includes this.

#include <optional>

namespace keylight {

// ---------------------------------------------------------------------------
// State — high-level license state (C++ subset of Rust/C# states)
// ---------------------------------------------------------------------------
enum class State {
    Licensed,   // trusted, unexpired active lease
    Trial,      // no license; within trial window
    Expired,    // trusted lease expired, or license status "expired"
    Invalid,    // no trusted lease, no active trial
    FreeTier,   // no license and no trial, but the product offers a free tier.
                // Appended last on purpose: renumbering the values above would
                // break any integrator that persisted a State as an integer.
    Limited,    // trusted lease with server status "fallback": the server could
                // not mint a full lease, so the app runs degraded rather than
                // locked. Appended last for the same reason as FreeTier —
                // renumbering would break any integrator that persisted a
                // State as an integer.
};

// ---------------------------------------------------------------------------
// LifecycleEvent — the subset of transitions worth telling a customer about.
//
// Distinct from a state-change subscription, which fires on EVERY transition.
// "Your licence renewed" is an event; "we re-resolved to the same state" is
// not, and a customer-facing notification driven off the latter is noise.
// ---------------------------------------------------------------------------
enum class LifecycleEvent {
    Renewed,    // still licensed, expiry moved later
    Cancelled,  // was licensed, now expired or degraded
    Expired,    // reached Expired from anything that was not already Expired
    Restored,   // reached Licensed from a non-licensed state
};

// `expiry_moved_later` is true when the newly observed expiry is later than the
// previous one, including the case where there was no previous expiry.
inline std::optional<LifecycleEvent> lifecycle_event(State prev,
                                                     State next,
                                                     bool  expiry_moved_later) {
    if (prev == State::Licensed && next == State::Licensed) {
        if (expiry_moved_later) return LifecycleEvent::Renewed;
        return std::nullopt;
    }
    if (prev == State::Licensed &&
        (next == State::Expired || next == State::Limited)) {
        return LifecycleEvent::Cancelled;
    }
    if (next == State::Licensed &&
        (prev == State::Expired || prev == State::Limited || prev == State::Invalid)) {
        return LifecycleEvent::Restored;
    }
    if (next == State::Expired && prev != State::Expired) {
        return LifecycleEvent::Expired;
    }
    return std::nullopt;
}

} // namespace keylight
