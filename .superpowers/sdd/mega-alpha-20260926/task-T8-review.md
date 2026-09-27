# Task T8 review: library v3 plus v2 fix round 2 (N1-N3)

Package: `review-T8.diff` (d5471776 v2', d3016d0b v3), read once in full.

Line references:
- gen3 = `atx-impl/strategies/generate_pv_fields_ic121_v3.py`
- gen2 = `generate_price_volume_ic96_v2.py`
- test = `test_generate_pv_fields_ic121_v3.py`
- lib3 = `pv_fields_ic121_v3.json`
- producer = `atx-engine/tools/prepare_research_fields.py`

All are at root HEAD `C:/atx-wt/pool-2`.

### Spec Compliance

- ✅ **Spec compliant.**
  - **Step 1, N1/N2:** recorded as pre-measurement notes in the v2 recipe `expected_overlap` plus REVISIONS rows tagged `round=2` (gen2 REVISIONS/EXPECTED_OVERLAP; diff 99-115, 894-922). No DSL changed, and gen2 asserts that every referenced template exists.
  - **Step 1, N3:** gen2:86 `idio_var = ts_var * abs(1 - rho^2)` hardens the numerator and denominator of both ivol_change templates. Exactly four dsl lines and four `dsl_sha256` values change (library hunk @@-742; recipe lineage hunk @@-905). `idio_share_252` is untouched.
  - **v3 families match the brief exactly:** 9 families and 25 new candidates.

    | Family | Variants |
    |---|---|
    | si_ratio | 3 |
    | dtc | 3 |
    | si_change | 3 |
    | iv_level | 2 |
    | iv_term | 4 |
    | iv_change | 4 |
    | iv_rv | 2 |
    | earn_car | 3 |
    | size | 1 |

    I checked this with a `Counter` over lib3 candidates[96:]. Every grid is window/smoothing only, with no data-driven additions. The 121 candidates in 25 families sit under the runner caps of 256 candidates and 32 families.
  - **v2' prefix byte-identical:**
    - gen3:402-403 assert that the candidates and lineage equal v2'.
    - The test compares per-candidate JSON encoding, DSL bytes and hashes.
    - My read-only check: lib3 candidates[:96] ids and DSL bytes equal `price_volume_ic96_v2.json`. The lib3 SHA is `5d164ea115c6…`.
  - **Provenance:** the recipe records the v2' library and recipe SHAs, `generator_sha256` (LF-normalized), a lineage row per candidate (family, template, s, prior_sign, fields, prior bars) and a template row per window parameter. load_v2 (gen3:98-102) asserts that the committed v2 files equal their generator.
  - **Only point-in-time fields:** every declared extra field is `point_in_time: True` in the current producer (mkt_ret, si_shares, si_dtc, iv_atm_21d/63d/126d, earn_recent, shares_out). I checked by regex over the producer FIELDS.
    - `is_common`, `mktcap_lagged` and `size_grp` are not declared.
    - gen3:408 asserts declared == referenced.
    - test:100 rejects `rank(mktcap_lagged)`.
  - **Causality:** every new candidate uses data up to and including the session-d close mark, which is the v2 convention. Per family:
    - **SI:** producer:102-115 is a strict `available_at < date(session)` as-of join on the official dissemination date, with a 45-day staleness NaN. Data disseminated on D is first used at row D+1. This is conservative.
    - **IV and earn_recent:** same-date vendor end-of-day row, known at 22:00 UTC, which is the close clock.
    - **earn_recent clock** (producer:67, 135-142): it maps earnFlag 0 (reaction session) and 1 (the session after) to 1, and N and -1 to 0. The -1 flag presumes a future date, so dropping it is correct. The gate `ts_sum(earn_recent * (ret - mkt_ret), H) / sign(ts_sum(earn_recent, H))` (gen3:134; lib3:1443) therefore sums only excess returns from sessions at or before d, selected by flags known at or before d. Whether the vendor sets flag 0 on the announcement day or on the reaction day, the [0,+1] pair captures the reaction and never reads a later session.
    - **shares_out:** A8 90-day lag, restated by a cumulReturnFactor ratio, so future actions cancel.
    - **mkt_ret:** row d uses closes at d-1 and d.
    - **Windows:** all are positive literals, enforced by the validator.
    - **Fixture:** it scrambles every field after t0 and checks that output up to t0 is unchanged (test:221-237).
  - **IV guard** `x + 0*log((x-0.02)*(5-x))` then `ts_backfill(., 5)`:
    - VM semantics are raw IEEE: `Log = std::log` and `Mul` is plain `p*q` (vm.hpp:47, 228-230, 244-246).
    - The parser folds only literal-with-literal operands (parser.cpp:230-236), so `0 * log(panel)` survives. Fast-math is banned repo-wide (CMakePresets.json:152).
    - In-domain x maps to exactly x. Out-of-domain values (1.2e16, 69, inf, <= 0) become NaN: the product goes negative or to -inf, and the log of that is NaN.
    - **`ts_backfill` is past-only.** `tsv_backfill` (ts_ops.hpp:357-366) scans x[t-i] for i < d. It returns the most recent non-NaN value in t-4..t, otherwise NaN, and never 0 or a future value.
    - A garbage cell is therefore never propagated. At most, it is replaced by the last in-domain value for four sessions. The typecheck charges d-1 = 4 lookback, which matches the recipe.
  - **Turnover:** every daily-input IV variant is decayed with s ∈ {21, 63}. The only undecayed variants are the slow SI/DTC levels (s1) and earnings_drift, whose base is piecewise constant. No new candidate is plausibly above tau_k 0.70 on average; see Minor 3 for the fastest one.
  - **Signs:** each base is written as prior-sign × quantity inside `rank()`, and `sign_policy: train-rank-ic21` re-orients every candidate on TRAIN. No hard-coded sign escapes the screen. The disputed literature signs are recorded in `prior_caveat` only (gen3 PRIOR_CAVEATS).
- ⚠️ **Cannot verify from the diff (controller checks):**
  1. **Native compiles.** Run native `--plan-only` for v3 (121 candidates, lookback 314 ≤ 336, about 8 slots) and for v2' `fd1e359b…`; the earlier `b871743e` result is stale. The runner now requires producer PIT flags on the fields manifest (strategy_ic_runner.cpp:501-517), so the `--train-fields` payload must be one written by the c099cade producer.
  2. **mkt_ret warm-up.** The TRAIN manifest reports `zero_contributor_sessions = 63`, per the report's concern 4. Confirm they are rows 0-62, which is below row 85 (score_begin 399 − 314). Otherwise the 29 mkt_ret candidates start as NaN.
  3. **Vendor vintage.** `iv_atm_*` is clean IV built from the vendor's forward-looking earnings calendar. That calendar, the earn_recent calendar and FINRA before 2021-06 all have unproven vintage. The producer flags these fields PIT and lists the caveat, and the recipe `vendor_caveats` records it. It cannot be verified here.
  4. **Root ratification of two pre-measurement decisions.**
     - The si_change grid is n ∈ {10, 21, 42}: three variants where the brief text names two, and the brief's "~2/~4 cycles = ~10/~21 sessions" is internally inconsistent.
     - The IV backfill constant is 5. It is justified: without it, one sporadic null blanks a name for 21-63 sessions under the full-window decay.
  5. **earn_car coverage.** earn_recent is NaN whenever the vendor earnFlag is null, and one NaN anywhere in the H window excludes the name. Measure coverage at admission.

### Strengths

- **Clean reuse of v2.** The v2 module is loaded and pinned by SHA, the committed v2 bytes are re-verified, the validator is table-extended rather than forked in behaviour, and the registry cross-check covers the two new operators. `log` = registry.cpp:24 and `ts_backfill` = :98, which is rolling in typecheck.cpp:38.
- **A guard that works with only the operators the VM supports**, with a precise written contract in recipe `iv_guard` (gen3:445-451).
- **Earnings gate designed correctly.** It gives NaN rather than 0 without an event (0/0), and the [0,+1] flag pair is robust to where the vendor places flag 0.
- **Strong synthetic fixture:**
  - future-scramble causality;
  - reversal of pre-lookback data (lookback sufficiency);
  - per-garbage-value equivalence to NaN across all 16 IV candidate-field pairs;
  - the bounded backfill;
  - exact earnings gating, including a never-reporting name;
  - 11 malformed-DSL rejections, including an undeclared non-PIT field and a negative delay.
- **Honest pre-measurement bookkeeping:** prior caveats, borrow exposure, expected overlaps and decisions are all recorded before measurement.

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

None.

#### Minor (Nice to Have)

1. **The IV guard's open interval disagrees with the root-declared closed domain.**
   - Where: gen3:50 and gen3:445-451; test:240 parametrizes `5.0` and `0.02` as garbage.
   - The producer keeps `float32(0.02) <= v <= float32(5.0)` (producer:70-74). The DSL NaNs exact 5.0, where 5 − x = 0 so log = −inf and 0·(−inf) = NaN. It also NaNs float32(0.02), whose f64 value 0.0199999995529651 is below 0.02, so the product is negative.
   - I verified both values: the producer keeps them and the DSL returns NaN.
   - Impact is negligible unless the vendor caps IV at exactly 5.0.
   - Fix: either record the endpoint exclusion as intentional in `iv_guard.admitted`, or align the constants. Aligning changes 12 dsl_sha256 values, which costs nothing before measurement.
2. **Stale field documentation against the root-HEAD producer.**
   - The iv_atm_21d basis (gen3:63-66; lib3:31) still says "vendor garbage remains (values far above 5)". Since c099cade the producer NaNs out-of-domain IV (producer:26, 70).
   - `unused_field_reasons` (gen3:480-481) cites only coverage for mktcap_lagged and size_grp. The producer now flags all three unused fields as non-PIT (producer:151-174), which is the stronger reason.
   - Doc-only. The runner does not read `basis` (checked: no `basis` use in strategy_ic_runner.cpp). A fix changes the library SHA but no dsl_sha256.
3. **The earnings_drift turnover prior is optimistic.**
   - Where: gen3:305-306 says "low … about 2/H". gen3:302 names `iv_change_5_s21` as "the fastest candidate".
   - Earnings cluster by season, so the covered set grows and shrinks each quarter and a plain rank (no decay) churns heavily at both ends of the season.
   - I ran a stylized synthetic simulation (N=1500, 75% of names announcing inside a 25-session window per quarter, rank → demean → gross 1, tau = Σ|Δw|):

     | Candidate | Mean tau | p95 tau | Covered set |
     |---|---|---|---|
     | earn_car_21_s1 | ≈ 0.22 | ≈ 0.52 | falls to about 9% of names off-season |
     | earn_car_42_s1 | 0.125 | not computed | not computed |
     | earn_car_63_s1 | 0.035 | not computed | not computed |

   - This is still below 0.70, but earn_car_21 is probably the fastest v3 candidate, not a "low" one.
   - Also, for H = 63, a quarterly event spaced fewer than 63 sessions after the previous one sums both events' CARs for a few sessions.
   - tau_k is measured at admission, so this is a label correction, not a gate risk.
4. **Near-verbatim duplication of the static validator.**
   - v3 `parse` (gen3:185-250), `validate` (261-267) and `registry_crosscheck` (270-292) are gen2:200-264, 295-300 and 303-324 with only the table source swapped. That is about 90 lines.
   - I am not rating it Important for three reasons:
     - each generator is a per-version frozen provenance artifact whose output bytes are pinned;
     - v3 already imports the rest of gen2's machinery (tokenize, peak_slots, Expr);
     - the brief barred v2 edits beyond N1-N3.
   - For v4, parameterize gen2's `parse`/`registry_crosscheck` on the tables. That change is output-neutral.
5. **Fixture coverage gaps.**
   - **The mirror does not use the runner's TsSum.** The numpy mirror (test:134-185, TsSum at :173-176) uses batch sums, but the runner evaluates in ResearchFast (strategy_ic_runner.cpp:683). That mode uses the Neumaier running TsSum (ts_ops.hpp:499-524).
     - The "NaN without event" gate depends on the running numerator returning to exactly 0.0 once the event leaves the window. Otherwise x/0 = ±inf, and CsRank ranks ±inf instead of excluding it, because cs_ops excludes only NaN.
     - I simulated the scalar TsvRunSum over H ∈ {21, 42, 63}: 694,765 all-zero windows, every one exactly 0.0. There is no defect, but nothing pins it.
   - **Untested path:** earn_recent NaN when the vendor row is absent.
   - A native test of earn_car on the ResearchFast path would close both gaps.

### Assessment

**Task quality:** Approved

**Reasoning:** N1-N3 are addressed exactly, with four DSL hashes changed and 92 cache hits kept. v3 adds exactly the brief's nine families. Every new candidate is causal against the producer clocks and uses only PIT fields. The IV guard plus the 5-session past-only backfill cannot turn a garbage cell into anything but NaN or the last in-domain value, for at most four sessions. What remains is documentation and label drift plus a boundary nuance in the guard.
