# Task R1 review: platform-v7, cdc9c2a8..5b958dd2 (read-only, Opus 5.5)

Scope: the four merged lanes (L1 sha256 + IC cache v2, L2 research_cycle / --reuse / integrity stats + fitter/pitch v2
readers, L3 holdings + decide, L4 risk model + cost v2 + aim-partial-v6 + capacity). The W1 SPO path is not in this
range, so this review has nothing on it. No build, no run: every finding comes from reading the code.
Coverage (no finding in these): SHA-NI schedule/rounds; v2 key text and lookups, including a v1 hit with a changed DSL
(ignored, then a miss); the KO and FIM formulas against the brief; the capacity scale argument for v5; CSCV (checked
against the paper and the literal oracle); ONC recursion (checked against MLAM 4.2); PSR/MinTRL/SR0; risk-model
timing (X_{t-1}, prior forecasts for the bias and the VRA); the holdings-to-decide timing.

| id | sev | file:line | finding | fix |
|---|---|---|---|---|
| I-1 | I | strategy_nav_v7.cpp:38,281-287,513 | `--capacity-curve` + `--rule aim-partial-v6` is accepted, but the capacity books price the v6 c_i with the fixed base-scale S2 law, so each curve row is not the NAV-m book | in the capacity pass price c_i with `cfg.scenario` (scaled Y is the NAV-m law), or refuse the combination; add a gtest |
| M-1 | M | strategy_live.cpp:495-506,799-805 | positions with a `session_ns` column and no row at the as-of, plus `--nav`, decide from an all-flat book (orders rebuild the whole book) | refuse 0 selected rows when a session column exists (or unless `--flat-book`); add a test |
| M-2 | M | strategy_live.cpp:860-869 | decision.json does not pin its real inputs: positions and locates have no path or SHA-256, and `--nav` is only a value | add positions/locates path + sha256 (and the --nav source) to `pins_verified` |
| M-3 | M | strategy_live.cpp:44-50,564-576 | decide TC = corr(signal/σ², target after the locate block); nav-v7 TC = corr(desired/σ, plan before the block). Both are labelled R2.5 but differ; decide's version is off from GK by 1/σ. The L3-F2 values (.089/.185/.262) use the decide version | one shared TC (desired/σ on the final target) in both paths; recompute L3-F2 |
| M-4 | M | strategy_cost_v2.cpp:236-250 | v6 rescales each side back to its entry gross after the shrink. This is not in the L4 brief or v7-prereg (target = aim/(1+κc/c̄)), and C1-C3 ran with it | write a prereg addendum (the rule as run) before reading C1-C3 |
| M-5 | M | strategy_risk_model.cpp:781-797,829-834 | the bias harness silently drops exposure to unforecast factors (<63 obs) from x'Fx, which understates vol and pushes b up. The book also drops uncovered names from the realized return. No counts for the random/factor families | count and report the exposure left out; mark a series not-live above a threshold; realize the book on every held name |
| M-6 | M | prepare_research_fields.py:494,2436,2580 | `--reuse` keys on spec text + a hand-bumped `FORMULA_REVISION` ({}). A code change reuses stale payloads, and the new manifest's `code_sha256` (a decide pin) claims them. The prior `code_sha256_lf` is recorded but never compared | refuse reuse when the prior code SHA differs unless a field is explicitly allowlisted; record which code produced each payload |
| M-7 | M | mega_report/pitch.py:173-196 | the cache-scan fallback takes a lone v2 entry by candidate_id without checking the DSL SHA (and does not re-hash), so sig_corr can use another library's DSL | filter scanned entries by the library's per-candidate dsl_sha256 (+ field payload SHAs), else n/a |
| m-1 | m | strategy_ic_runner.cpp:2301 | the IC summary `candidate_cache` block changed (entries/layout/legacy_hits) with no schema bump; v1 readers fail as "missing entry" | bump the summary schema or add a `candidate_cache.schema` |
| m-2 | m | strategy_ic_runner.cpp:2189 | legacy hits write IC-result files into v1 directories (ROOT/<old manifest>/ic1_*), which contradicts "v1 never written"; with a `cp -al` cache these new files land in the shared tree | document this, or key legacy IC results under the v2 dir |
| m-3 | m | strategy_cost_v2.cpp:220-235; strategy_nav_v7.cpp:288 | v6 with no usable ADV/volume on a date (c̄ NaN) silently becomes uniform-band v5 for that date; TC CSV shows NaN c_bar and nothing refuses | count unpriced decisions in the summary; refuse if > 0 on a run with volume |
| m-4 | m | strategy_nav_v7.cpp:225-254,544-560 | the TC CSV, capacity_curve.csv and v7_extras.json are written after the published summary.json, non-atomically, with no completion marker | write extras before or inside the summary, or add `status` to v7_extras.json |
| m-5 | m | strategy_risk_verb.cpp:99-146 | the `risk` verb has no TRAIN/seal guard: any role or book file (VAL included) is accepted | refuse a role or book whose sessions reach 2023-01-01 without an owner flag |
| m-6 | m | strategy_risk_model.cpp:314-319; hpp:145 | `rolling_bias` windows are raw entries, but the header says "finite z" | fix the comment or skip NaN |
| m-7 | m | strategy_nav_v7.cpp:305; strategy_nav_replay.cpp:833-842 | the replay TC uses `planned` before the locate block, but the holdings/targets are post-block | pass the post-block weights (or name the column "rule TC") |
| m-8 | m | strategy_live.cpp:625-631 | health gross/net/turnover bands read `dec.plan`, the rule plan before the locate block, not the orders sent | compute health on `dec.target` |
| m-9 | m | backtest_integrity.py:444-453,569-582 | effective-N via a ledger ignores `count`, mixes kinds, and `align_many` silently shrinks every series to the common sessions | report the dropped sessions per series; refuse below a minimum overlap; state that `count` is ignored |
| m-10 | m | backtest_integrity.py:531-543 | `ledger_append` read-check-append is unlocked, so parallel lanes can duplicate a trial_id | lock the file (O_EXCL lockfile) around the read and append |
| m-11 | m | strategy_ic_runner.cpp:1207-1222 | a v1 field sidecar whose `research_fields` order differs refuses instead of missing; there is no gtest for a cross-manifest v1 field entry with a changed DSL | compare as sets; add that test |
| m-12 | m | strategy_nav_v7.cpp:57 | the v7 hook is a thread-local global (`active_state`) read by free functions at 10 seams: hidden coupling, and a stray extension on a thread changes every replay on it | pass the extension explicitly through NavReplayConfig/Ctx in the next refactor |
| m-13 | m | nav_v7.cpp:450-527; live.cpp:833-893; risk_model.cpp:584-664; risk_verb.cpp:561-633 | functions of 61-81 lines (agent.md section 3 says ≤ ~60) | split parse/validate and forecast steps |
| m-14 | m | strategy_nav_replay.cpp:2627 | decide's cadence phase is `decision_begin` of the loaded role, so a live role with another start shifts rebalance days (cadence > 1) | pin the cadence anchor session in the deploy manifest |
| m-15 | m | strategy_risk_verb.cpp:194-207 | cap = issuer-level `me_company` per share class, so multi-class issuers get double WLS weight and size | split the issuer cap by class share count, or dedupe by issuer |
| m-16 | m | strategy_risk_verb.cpp:596-625 | the risk manifest has no executable/code identity (not reproducible from the record) | add exe SHA + build provenance |

## I-1: the capacity curve is wrong for aim-partial-v6 (strategy_nav_v7.cpp)
```cpp
: options(o), s2(fixed_nav_scenarios()[nav_primary_scenario_index]) {}          // :38
const f64 q = members ? cfg.target.trade_fraction * cfg.target.aim_leverage * nav_post /
                            static_cast<f64>(members) : nan;                     // :281
costs[i] = cost_v2::marginal_cost_s2(s2, q, liquidity.adv[i], liquidity.sigma[i]); // :287
if ((o.aim_v6 || o.capacity) && rate && *rate != "fixed")                       // :513 (only guard)
```
The capacity pass replays S2 at the initial NAV with `impact_y * m^delta` and `max_participation / m`. It is exact
only if nothing else reads the NAV. That is the stated reason per-name-v1 is refused ("per-name-v1 reads the NAV"). The
v6 rule reads the NAV through q, and `State::plan` (v6 && !observe branch) prices c_i with the fixed S2 law at the
base-scale `nav_post`. The NAV-m book's cost is c_i(m) = (hs+comm) + (1+δ)Yσ(m·q/ADV)^δ. The constant spread term does
not scale with the impact term, so c_i/c̄, band_i, θ_t and target_i all change with m, and each capacity row replays a
different construction from the NAV-m book. `capacity_declaration` (:105-111) still says "scale invariant in every
dollar except the impact law and the participation cap". CostV2Capacity (strategy_cost_v2_test.cpp:195) covers v5 only.
Fix: in `NavV7Pass::Capacity`, call `marginal_cost_s2(cfg.scenario, q, ...)`. Its Y·m^δ at base q is exactly the
NAV-m marginal cost. Keep `s2` for the main pass. Add a v6 variant of the x2-at-V == S2-at-2V/2 gtest. Until then,
refuse `--capacity-curve` with `--rule aim-partial-v6`. The measured v6.1 curve is a v5 cell and is unaffected.

## M-1: a positions selection with no rows decides from a flat book (strategy_live.cpp)
```cpp
if (session < width) { ... if (key != asof) continue; }                 // :497-502
...
if (!out.rows) out.has_expected = false; return co::Ok(std::move(out)); // :505-506
if (!cfg.nav && !p.has_nav) return Err(...);  in.nav = cfg.nav ? *cfg.nav : p.nav;   // :799-802
```
Any id missing from the file counts as +0. If a dated export has no row at the as-of (wrong date, truncated file) and
the caller passes `--nav`, the book is taken as flat. The targets become θ × full aim for every member, and orders.csv
is a book-building order set. Health raises at most a gross-band warning (exit 0). Fix: refuse `rows == 0` when a
session column is present, and require an explicit flag for a genuinely flat book. Add a test.

## M-2: decision.json has no input pins (strategy_live.cpp:860-869)
`{"positions", {{"rows", ...}, {"nav_post", ...}, {"nav_source", ...}}}` and `{"locates", {{"supplied", ...},
{"names_listed", ...}}}`: nothing records which positions or locates bytes produced targets.csv/orders.csv. The
decision therefore cannot be audited or reproduced from its own record, although every other input is SHA-pinned. Fix:
hash both files (`co::sha256_file`) into `pins_verified`, with their paths, and record the `--nav` literal.

## M-3: two transfer coefficients under one name
```cpp
const f64 alpha = x.signal[d * n + i], sigma = dec.sigma[i];                 // strategy_live.cpp:569
score.push_back(alpha / (sigma * sigma)); weight.push_back(dec.target[i]);   // :572-573
sa += desired[i] / sigma[i];                                                 // strategy_cost_v2.cpp:313
record.tc = cost_v2::transfer_coefficient(member, desired, liquidity.sigma, planned); // nav_v7.cpp:305
```
Grinold-Kahn gives α = IC·σ·z, so α/σ² = IC·z/σ, which is the nav-v7 form. decide uses the combined z-score as α, and
the extra 1/σ tilts its TC toward low-vol names. decide also correlates with the post-locate target; nav-v7 uses the
pre-block rule plan. The ledger's L3-F2 (.089/.185/.262, cited as evidence for the SPO lane) uses the decide definition,
so it cannot be compared with v7_transfer_coefficient.csv. Fix: have decide call `cost_v2::transfer_coefficient`
(desired/σ) on the final target, and recompute L3-F2.

## M-4: aim-partial-v6 is not the pre-registered formula (strategy_cost_v2.cpp:236-250)
```cpp
const f64 t = std::isfinite(r) ? desired[i] / (1.0 + p.kappa * r) : 0.0;
out.scale_long = long_out > 0 ? long_in / long_out : 1.0;   // then target *= scale by side
```
The L4 brief and v7-prereg define target_i = aim_i/(1+κc_i/c̄). The per-side renormalisation (probably needed for the
R6' gross gate, since a κ=1 median shrink is ~50%) turns the GP shrink toward zero into a gross-preserving cost tilt,
and it changes which names gain weight. It is declared in the code (v6_declaration) but not in the prereg, and C1-C3
are running on it. Fix: before any C1-C3 statistic is read, add a v7-prereg addendum stating the rule exactly as run
(or add a flag and name the variant).

## M-5: silent omissions in the bias harness (strategy_risk_model.cpp)
```cpp
if (slot == no_exposure || !std::isfinite(d)) { ++uncovered; continue; }           // :784
if (x[a] == 0 || !std::isfinite(day.covariance[a * factor_count + a])) continue;   // :791
if (... no_exposure || !std::isfinite(day.specific_variance[i])) { ++s.uncovered_names; continue; } // :830
```
A factor is forecast only after 63 observed returns (`min_factor_history`). Industries are pooled per date by
`min_industry_names`, so an industry that has just crossed 10 names has a return series but no variance. Its exposure
drops out of x'Fx, the forecast vol is too low, and z and b are biased up. The random and factor families report no
count of this. For the book, uncovered names leave both the forecast and the realized return, so b describes a
sub-portfolio. The descriptive "b mean .995" is unaffected only if such exposure is small, and that is unmeasured. Fix:
per series, report the exposure mass on unforecast factors and the uncovered weight. Mark a series not live above a
threshold. Realize the book on every held name.

## M-6: `--reuse` trusts a hand-bumped revision (prepare_research_fields.py)
```python
FORMULA_REVISION: dict = {}                                    # :494
want = formula_id(name, spec_definition(name, lag))            # :2580 (spec text + revision only)
**code_identity(Path(__file__)),                               # :2436 new manifest = current code
```
A fix to a field's computation that leaves its registry text alone (for example, a staleness bug in a loader) copies the
old payload. The new manifest's `code_sha256` then says the current code built it, and decide pins
`fields.code_sha256` from that manifest (strategy_live.cpp:383). `reused_from.code_sha256_lf` is written but never
compared. Fix: refuse reuse when the prior `code_sha256_lf` differs from the current one, unless the field is named in
an explicit allowlist flag (recorded in `reuse`). Record the producing code per entry, which is already available in
`reused_from`.

## M-7: the pitch sig_corr fallback can load another DSL (mega_report/pitch.py:173-196)
```python
own = [h for h in hits if lib_sha and h[1].get('library_sha256') == lib_sha]
pick = own if own else hits
if len(pick) == 1: metas[i] = pick[0]
```
v2 sidecars record only the first library that wrote them (the key is content-based). When the summary entries are
missing, a single v2 entry for the id written by another library with a different DSL is taken as this library's signal.
The payload is memory-mapped without re-hashing (`not re-hashed`), so the correlations in the pitch are silently wrong.
Fix: read the report library's `dsl_sha256` per candidate and require `m['dsl_sha256']` to equal it (and
`field_payload_sha256` to match the pinned fields manifest); otherwise report the id as missing or ambiguous (n/a).

## Test gaps against the lane acceptance lists (covered by the fixes above)
- L4: no v6 + capacity test (I-1); no FIM/KO book test with a fallback-vol row; no test that a v6 cell's
  `v7_transfer_coefficient.csv` matches decide's TC (M-3).
- L3: no test for positions with no selected rows (M-1), a --nav/positions sum mismatch, or a decide on a
  neutralization-skipped non-rebalance day after a capped fill (working orders not modelled, as documented).
- L1: no cross-manifest v1 *field* entry with a changed DSL (only base, test :1510) (m-11).
- L2: no test that `--reuse` refuses on a builder code change (M-6); no concurrent `ledger_append` test (m-10).
