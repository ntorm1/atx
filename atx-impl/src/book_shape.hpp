#pragma once

// atx::impl — shape_book: deploy a raw combined-weight cross-section as a book.
// Dollar-neutralize (Sigma w = 0 over live names) -> gross-normalize (Sigma|w| =
// gross) -> name-cap clip-renorm (|w_i| <= name_cap, redistributing to unclipped
// names to restore gross). Deterministic: fixed pass count, canonical order, no RNG.
// This is the signal-as-position deploy step — NO mean-variance optimization.

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::impl {

inline void shape_book(std::vector<atx::f64>& w,
                       std::span<const std::uint8_t> live,
                       atx::f64 gross, atx::f64 name_cap) {
    const atx::usize n = w.size();
    // 1. Zero dead / NaN cells.
    atx::usize n_live = 0;
    for (atx::usize i = 0; i < n; ++i) {
        const bool ok = (i < live.size() && live[i] != 0) && !std::isnan(w[i]);
        if (!ok) { w[i] = 0.0; } else { ++n_live; }
    }
    if (n_live == 0) return;

    // 2. Dollar-neutralize (subtract mean over live cells).
    atx::f64 mean = 0.0;
    for (atx::usize i = 0; i < n; ++i) if (w[i] != 0.0 || (i < live.size() && live[i] != 0)) mean += w[i];
    mean /= static_cast<atx::f64>(n_live);
    for (atx::usize i = 0; i < n; ++i)
        if (i < live.size() && live[i] != 0) w[i] -= mean;

    // 3. Gross-normalize to Sigma|w| = gross.
    auto renorm_gross = [&]() {
        atx::f64 g = 0.0; for (atx::f64 x : w) g += std::abs(x);
        if (g > 0.0) { const atx::f64 s = gross / g; for (atx::f64& x : w) x *= s; }
    };
    renorm_gross();

    // 4. Name-cap clip-renorm: fixed 8 passes (mirror WeightPolicy::truncate_renorm).
    if (name_cap > 0.0) {
        for (int pass = 0; pass < 8; ++pass) {
            bool any_clip = false;
            for (atx::f64& x : w) {
                if (x > name_cap)  { x = name_cap;  any_clip = true; }
                else if (x < -name_cap) { x = -name_cap; any_clip = true; }
            }
            if (!any_clip) {
                break;
            }
            renorm_gross();
        }
    }
}

} // namespace atx::impl
