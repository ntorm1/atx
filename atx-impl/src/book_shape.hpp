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
//
// SIGN CONTRACT (S6-0): for a dollar-neutral combined cross-section (Sigma w = 0 in)
// this transform is EXACTLY sign-preserving per name — demean is a no-op on a
// zero-mean input, and rescale + clip are both positive scalings.  For a non-neutral
// input the demean step CAN flip a name whose weight lies strictly between 0 and the
// cross-sectional mean, so the guarantee degrades to directional/book-level.  The
// combiner's output is dollar-neutral by construction, so per-name sign preservation
// holds in the deployed case; the MVO path re-weights by 1/dvar and re-centers, which
// can INVERT the book vs realized returns (see stage_optimize.cpp ROOT CAUSE S6-0).

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
    //    Fixed 8 passes (deterministic; same spirit as WeightPolicy::truncate_renorm).
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

// book_turnover_per_day (p9 S5-1): convert a per-rebalance-period L1 turnover
// series into a book-level DAILY rate: mean per-period turnover divided by the
// average trading-day spacing between rebalances. A schedule with < 2 periods
// (or non-advancing periods) has no spacing to divide by -- reports the raw
// per-period mean instead (an honest degenerate: there is no "day" yet to
// normalize against). This mirrors stage_report.cpp's step-spacing convention
// (rebalance-day gaps read off the same RebalanceSchedule.periods axis) rather
// than inventing a new normalization. Pure, order-fixed reduction over an
// already-deterministic MultiPeriodResult.turnover / MetaBook turnover_net
// series -- no RNG, no parallelism, so it is bit-identical across runs.
//
// Ownership: this is a neutral house helper (no sprint owns it exclusively);
// both stage_optimize.cpp and stage_metabook.cpp call it. turnover and
// sched_periods must have the SAME per-period cardinality when both are
// non-empty (turnover[s] is the L1 move at rebalance sched_periods[s]); the
// function only reads sched_periods.front()/.back()/.size() for the spacing.
[[nodiscard]] inline atx::f64
book_turnover_per_day(std::span<const atx::f64> turnover,
                      std::span<const atx::usize> sched_periods) {
    if (turnover.empty()) return 0.0;
    atx::f64 sum = 0.0;
    for (atx::f64 t : turnover) sum += t;
    const atx::f64 mean = sum / static_cast<atx::f64>(turnover.size());
    if (sched_periods.size() < 2 || sched_periods.back() <= sched_periods.front()) return mean;
    const atx::f64 span_days = static_cast<atx::f64>(sched_periods.back() - sched_periods.front());
    const atx::f64 step_days = span_days / static_cast<atx::f64>(sched_periods.size() - 1);
    return mean / step_days;
}

} // namespace atx::impl
