#pragma once
// keylight/clock.hpp — heuristic detection of system-clock manipulation.
// Ported from keylight-rust keylight/src/clock.rs.

#include <cstdint>

namespace keylight {

// How far the clock may move backward before we call it manipulation rather
// than drift. NTP corrections and suspend/resume routinely move it a little.
inline constexpr int64_t CLOCK_BACKWARD_TOLERANCE = 3600; // 1h

// True when `now` is more than the tolerance behind `last_seen` — the clock
// was rolled back since the last recorded contact.
//
// This deliberately OMITS the forward-jump component of Rust's
// clock_manipulated(), so it can gate the read-only state() resolver without
// governing offline duration — that stays with maxOfflineDays. Conflating the
// two would fail-close on every user who simply went offline for a while.
//
// Operates on UTC epoch seconds, so a timezone change never trips it.
inline bool clock_rolled_back(int64_t last_seen, int64_t now) {
    return last_seen - now > CLOCK_BACKWARD_TOLERANCE;
}

} // namespace keylight
