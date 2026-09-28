# Task V6.1 review: sv_ratio126 / sv_flow (FINRA short volume)

Reviewed b279555a, db1a6384, cb76a6a3 against v4-prereg.md "## v6.1 sub-alpha". Read-only.

## Findings

No Critical findings.

**Important — `cnms_to_si` (prepare_research_fields.py, marker rewrite used at map_symbols:2126) does an
unanchored `str.replace()` of lowercase p/r/w anywhere in the symbol, not just a trailing suffix marker.**
Multi-marker symbols are correctly handled (tested: `"FTFrw" -> "FTFRTWI"`), and a base ticker that happens
to carry an embedded lowercase p/r/w not part of a suffix marker would be corrupted before canonicalisation.
`parse_cnms`'s `SV_SYMBOL_PATTERN` (`^[A-Za-z0-9./\-]+$`) does not restrict lowercase characters to a single
trailing marker, so this isn't ruled out structurally. In practice the risk is bounded, not open: a bad
rewrite almost always lands on "no role key" (safe drop) rather than a wrong instrument, because a false
positive requires the corrupted string to exactly equal a *different* real instrument's canonical spelling,
and the exact-match collision rule (`sv_resolve_collisions`) plus the TickerHistory ambiguity check
(`SvTickerMap.ucol == -1`) both apply on top. Real-data spot check in the report (421/427 preferreds,
39/40 class shares, 2/2 rights match after rewrite) supports low real-world incidence. Not blocking, but the
6+1 unexplained non-matches were never root-caused — worth a follow-up pass, not a re-run blocker.

## Checklist

1. **Look-ahead / lag-1 / window alignment (sv_field, :2185-2231):** correct. The write for session `d`
   (`ext[e]`, e>=e0) happens before `ext[e]`'s own file is loaded into the ring; the file dated `d` is loaded
   only after the write, so it first affects `d+1`'s ratio. Ring slot `e % W` is cleared right before loading
   day `e`, evicting exactly day `e-126`, giving window `[e-125, e]` for the write at `e+1` = `[d-126, d-1]`.
   The role's last session's file is provably never read (`on_ext = ext[:-1]`, loop breaks before load at the
   final iteration). Lag-1 test (change file at `SESSIONS[30]`, rows 0-30 unchanged, row 31 changes) matches
   this mechanism. FINRA file dates are matched to role sessions by direct calendar-date equality against
   `role.days` (the trading calendar) inside the window proper; only the *prefix* (days before the role's
   first session) is built from raw file dates without an independent trading-calendar filter, but this can
   only add non-trading noise to the warm-up window, not create look-ahead.

2. **Ratio definition (sv_field:2188-2191):** `sum(short)/sum(total)` over the window, not a mean of ratios —
   confirmed (`ring_s.sum(axis=0) / ring_t.sum(axis=0)`, guarded by `total > 0`). Minimum 63 sessions enforced
   via `count >= SV_MIN_SESSIONS` where `count` is mapped-row sessions, not calendar slots. Zero-total -> NaN
   via the same `ok` mask.

3. **Symbol map / wrong-instrument risk:** ambiguity (same canonical ticker held by >1 securityID that day,
   role or non-role) is detected in `SvTickerMap.__init__` (pass 2) and dropped (`col == -1`), not silently
   resolved. The producer's exact-match collision rule is ported faithfully and unit-tested against the
   producer's own functions (`test_prepare_research_fields_sv.py::SymbolMapReuse`). The p/r/w -> PR/RT/WI
   rewrite is verified against the actual short-interest/ORATS spelling and is the reason "CpK" (Citigroup
   preferred K) does not collide with Chesapeake Utilities' common "CPK" (asserted in
   `test_canonicaliser_and_spelling_match_the_producer`). See Important finding above for the residual gap.

4. **`sv_flow` DSL / prior sign:** `rank((-1 * group_neutralize(sv_ratio126, grp_ff12)))`, `prior_sign` +1,
   `raw_prior_direction` -1 — this is exactly the house convention used by `si_ratio`, `dtc`, `si_change` and
   the group-demean idiom of `ind_adj_rev_5` (rank(-1*group_neutralize(x, grpN))), confirmed by reading
   `fund_industry_ic_v61.json`. Consistent with "high shorting flow -> short" and with si_ratio/dtc/si_change.

5. **Run script gate and nav flags (v61_train.sh):** `do_p1` reads `$W/admission.json`, keys on `sv_flow`,
   requires `status == "admitted" and sign_agrees is True`, exits 10 otherwise; `do_w` calls `do_p1` first and
   refuses (returns 10) if it doesn't pass — matches "stop before w on failure." `do_nav`'s flags
   (`--rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage $LEV
   --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes
   1073741824 $XF` with `XF="--order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache"`) are
   byte-for-byte the same flags as `v6u_train.sh do_nav` (diffed directly), with `LEV=1.247` fixed and
   `REFN=build-equity/mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`, which is exactly the directory
   name `v6u_train.sh` produces for `LEV=1.247` (`N="$N-L$LEV"` when `LEV != 1`). Confirmed match.

## Verdict

PASS
