#pragma once

// atx::impl — shape_book: deploy a raw combined-weight cross-section as a book.
// Dollar-neutralize (Sigma w = 0 over live names) -> gross-normalize (Sigma|w| =
// gross) -> name-cap clip with budget REDISTRIBUTION to the unclipped names so the
// total gross is restored without inflating capped names. Deterministic: fixed pass
// count, canonical order, no RNG. This is the signal-as-position deploy step — NO
// mean-variance optimization.
//
// Why redistribute-to-free rather than renorm-all: a plain "clip then rescale every
// name" loop oscillates (rescaling pushes the just-clipped names back over the cap),
// so it never settles. Rescaling ONLY the sub-cap (free) names by the remaining gross
// budget (gross - sum|pinned|) converges: feasible caps reach Sigma|w| = gross with
// every |w_i| <= cap; an INFEASIBLE cap (name_cap * n_live < gross) pins every name at
// the cap and leaves Sigma|w| = n_live*cap < gross — the cap wins (documented
// degenerate). A final clip guarantees no name exceeds the cap after the last rescale.
//
// CONTRACT NOTE: exact dollar-neutrality (Sigma w = 0) holds only immediately after
// step 2. An asymmetric binding cap (more longs clipped than shorts, or vice versa)
// can leave a small net residual, so downstream MUST NOT assume Sigma w == 0 after a
// cap binds. Step 3's gross target is likewise best-effort under an infeasible cap.

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
    // 1. Zero dead / NaN cells; count live names.
    atx::usize n_live = 0;
    for (atx::usize i = 0; i < n; ++i) {
        const bool ok = (i < live.size() && live[i] != 0) && !std::isnan(w[i]);
        if (!ok) { w[i] = 0.0; } else { ++n_live; }
    }
    if (n_live == 0) return;

    // 2. Dollar-neutralize (subtract the mean over live cells; Sigma w = 0).
    atx::f64 mean = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
        if (i < live.size() && live[i] != 0) mean += w[i];
    }
    mean /= static_cast<atx::f64>(n_live);
    for (atx::usize i = 0; i < n; ++i) {
        if (i < live.size() && live[i] != 0) w[i] -= mean;
    }

    // 3. Gross-normalize to Sigma|w| = gross.
    {
        atx::f64 g = 0.0;
        for (atx::f64 x : w) g += std::abs(x);
        if (g > 0.0) {
            const atx::f64 s = gross / g;
            for (atx::f64& x : w) x *= s;
        }
    }

    // 4. Name-cap clip with budget redistribution to the unclipped (free) names.
    //    Fixed 8 passes (deterministic; mirrors WeightPolicy::finalize_truncation).
    if (name_cap > 0.0) {
        for (int pass = 0; pass < 8; ++pass) {
            bool any_clip = false;
            atx::f64 pinned = 0.0; // Sigma|w| over names at the cap
            atx::f64 free   = 0.0; // Sigma|w| over names below the cap
            for (atx::f64& x : w) {
                if (x > name_cap)       { x =  name_cap; any_clip = true; }
                else if (x < -name_cap) { x = -name_cap; any_clip = true; }
                if (std::abs(x) >= name_cap) pinned += std::abs(x);
                else                         free   += std::abs(x);
            }
            if (!any_clip) break; // nothing exceeded the cap this pass -> settled
            const atx::f64 budget = gross - pinned; // gross remaining for free names
            if (budget > 0.0 && free > 0.0) {
                const atx::f64 s = budget / free;
                for (atx::f64& x : w) {
                    if (std::abs(x) < name_cap) x *= s;
                }
            } else {
                break; // infeasible cap: names pinned, cap wins (Sigma|w| < gross)
            }
        }
        // Final guarantee: no name exceeds the cap after the last rescale.
        for (atx::f64& x : w) {
            if (x > name_cap)       x =  name_cap;
            else if (x < -name_cap) x = -name_cap;
        }
    }
}

} // namespace atx::impl
