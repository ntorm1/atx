#pragma once
// stage_discover_detail.hpp — internal helpers for stage_discover.cpp.
//
// Exposed in atx::impl::detail so unit tests can call the real implementation
// (Fix 1, W2 review). Do NOT include this header from production callers other
// than stage_discover.cpp, stage_sweep.cpp, and discover_test.cpp.

#include <cmath>    // std::isfinite (cardinality scan NaN guard)
#include <set>      // std::set (distinct-value counter in classify_typed_fields)
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/typecheck.hpp" // alpha::detail::is_group_field
#include "atx/core/types.hpp"

namespace atx::impl::detail {

// Apply the W2 capacity universe screen to `panel`.
//
// Builds a derived Panel whose universe mask is:
//   original_universe AND (close > min_price) AND (adv{adv_window} >= min_adv)
//
// Fail-closed: missing 'close' or 'volume' => Err(InvalidArgument).
// When screen is ACTIVE (called only when min_adv>0 || min_price>0):
//   - adv_window out of [1, 65535]  => Err(InvalidArgument) naming the bad value.
//   - post-screen kept-cell count == 0 => Err(InvalidArgument) with diagnostic.
//
// Predicate mirrors single_alpha_capacity_test.cpp::capacity_universe exactly.
[[nodiscard]] atx::core::Result<atx::engine::alpha::Panel>
apply_capacity_screen(const atx::engine::alpha::Panel& panel,
                      atx::f64 min_price, atx::f64 min_adv, long adv_window);

// Build the W4a robust-factor weak/holdout sub-universe Panel (Deliverable 2).
//
// Returns a derived Panel = `panel` with the SAME field columns but its universe
// restricted to a DETERMINISTIC seeded instrument sub-sample: instrument i stays
// in-universe iff it was in the original universe AND it is SELECTED by a SplitMix64
// mix of (master_seed, i) falling under `frac`. Seeded by `master_seed` only (never
// thread/time) -> same seed + frac + panel => same weak universe (seed-stable, F1).
// The candidate's WQ is re-scored on this held-out universe to form the robust factor.
//
// `frac` MUST be in (0, 1). Fail-closed: frac out of range, or a sub-sample that
// retains zero in-universe cells, => Err(InvalidArgument) with a diagnostic. Exposed
// so discover_test.cpp can verify the determinism + masking directly.
[[nodiscard]] atx::core::Result<atx::engine::alpha::Panel>
build_robust_holdout_panel(const atx::engine::alpha::Panel& panel, atx::f64 frac,
                           atx::u64 master_seed);

// Mean in-universe instrument count per date over `panel` (sum of in_universe cells
// over all (date, inst) divided by dates()). 0.0 when the panel has no dates. PURE;
// reads ONLY the universe mask. Used to RECORD the capacity-screen universe size as a
// discovery admission metric (W5). When `panel` is the post-capacity-screen panel this
// equals apply_capacity_screen's stderr `names_per_day` by construction (same mask).
// Exposed so discover_test.cpp can verify the count directly on a hand-built mask.
[[nodiscard]] double mean_names_per_day(const atx::engine::alpha::Panel& panel);

// R1 typed-fields cardinality scan.
//
// Performs ONE deterministic pass over each numeric panel field (fields whose name is
// NOT already classified as a group field by is_group_field()) and computes its
// distinct non-NaN / finite cardinality (number of unique finite values across all
// dates*instruments cells). Early-out once cardinality exceeds `cardinality_max` to
// bound cost on dense continuous fields. A field is added to `numeric_excluded_out` iff:
//   (a) its cardinality <= cardinality_max, OR
//   (b) its name is in the binary/count backstop list {earnFlag, nEarnCnt_5d, gics}.
// Additionally, if the field's name is "gics" (raw integer GICS column, which is not
// automatically caught by is_group_field() since it lacks the "sector" / "IndClass."
// prefix) it is ALSO added to `extra_group_fields_out` so the grammar routes it to the
// GROUP pool.
//
// Group fields (is_group_field() == true, e.g. "sector", "IndClass.*") are SKIPPED: the
// SearchDriver's partition already handles them correctly; scanning them would be
// redundant and could accidentally re-add them to numeric_excluded_out.
//
// DETERMINISM: stable iteration over `fields` (caller supplies them in panel order).
// No RNG. Called ONLY when --typed-fields is set; when absent both output vectors stay
// empty and SearchDriver's partition is byte-identical to today (kGoldenDigest pin).
//
// Exposed (not static) so discover_test.cpp can unit-test it directly (brief §Tests.4).
inline void classify_typed_fields(
    const atx::engine::alpha::Panel& panel,
    const std::vector<std::string>& fields,
    int cardinality_max,
    std::vector<std::string>& numeric_excluded_out,
    std::vector<std::string>& extra_group_fields_out)
{
    // Hard-coded backstop: binary event flags and low-cardinality counts that are
    // ALWAYS excluded regardless of the panel's realized cardinality (e.g. earnFlag
    // may have cardinality 2 which is below any reasonable K, but we name it here
    // as a safety net for cases where K is set too high).
    static const std::vector<std::string> kBackstop = {"earnFlag", "nEarnCnt_5d", "gics"};

    for (atx::usize fi = 0; fi < fields.size(); ++fi) {
        const std::string& fname = fields[fi];

        // Skip classifier fields — they're already correctly partitioned.
        if (atx::engine::alpha::detail::is_group_field(fname)) continue;

        // Check backstop list first (O(k), k tiny).
        bool in_backstop = false;
        for (const auto& b : kBackstop) {
            if (fname == b) { in_backstop = true; break; }
        }

        // Cardinality scan: count distinct finite values, early-out at K+1.
        // Use field_all(FieldId) to get the full date-major column span (dates*insts).
        bool low_cardinality = false;
        if (!in_backstop) {
            const auto fid = static_cast<atx::engine::alpha::FieldId>(fi);
            const std::span<const atx::f64> col = panel.field_all(fid);
            std::set<atx::f64> seen;
            bool exceeded = false;
            for (const atx::f64 v : col) {
                if (!std::isfinite(v)) continue; // skip NaN / inf
                seen.insert(v);
                if (static_cast<int>(seen.size()) > cardinality_max) {
                    exceeded = true;
                    break;
                }
            }
            low_cardinality = !exceeded;
        }

        if (in_backstop || low_cardinality) {
            numeric_excluded_out.push_back(fname);
            // Route raw "gics" integer column to the group pool (it is categorical but
            // not named "sector" / "IndClass.*", so is_group_field() misses it).
            if (fname == "gics") {
                extra_group_fields_out.push_back(fname);
            }
        }
    }
}

} // namespace atx::impl::detail
