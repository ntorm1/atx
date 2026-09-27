# Task T8 report: library v3 (short interest, implied vol, earnings drift, size) on fixed v2

Status: DONE_WITH_CONCERNS. The concerns are prior-sign caveats and integration notes, not defects.

Worktree `C:/atx-wt/pool-7`, branch `feat/mega-alpha-library-v3-20260927`, base `6b4a21fe` (v2 fix round 1).

| Commit | Subject |
|---|---|
| `51b01e61` | fix(research): v2 library fix round 2 before TRAIN measurement |
| `a20002f5` | feat(research): alpha-DSL library v3 with SI, IV, earnings and size families |

No C++ changed, so nothing needs building or CMake registration. Everything was checked with pure Python on synthetic data only. No real-data payload was read: I opened only the TRAIN fields `manifest.json`, and never the validation fields.

## Step 1: v2 fix round 2 (root ruling, before any v2 measurement)

File: `atx-impl/strategies/generate_price_volume_ic96_v2.py`. Regenerated deterministically, and `--check` reproduces the same bytes.

- **N1:** added a recipe `expected_overlap` block and a REVISIONS row. It records a prior |rank corr| of about 0.85-0.9 between `resid_sharpe_6_1` and v1 `risk_scaled_6_1`, with the re-review's arithmetic as the basis. This is a note only; no DSL changed.
- **N2:** the same block records `ivol_change_21_252` against v1 `vol_expansion_21_126` (about 0.8-0.85) and against v2 `vol_term_63_252` (about 0.7). Note only.
- **N3:** `idio_var = ts_var(ret, n) * abs(1 - rho^2)`, following the M7 pattern. The abs applies to both the numerator and the denominator of both ivol_change templates, so neither can flip sign when rho rounds past 1. `idio_share_252` is a level, not a denominator, so it was left unchanged and keeps its dsl_sha256.
- **Bookkeeping:**
  - REVISIONS rows now carry `round` (1 or 2).
  - `generation.revision` is `fix-round-2`.
  - `trials.pre_measurement_template_revisions` is 2.
  - The docstring and `family_fixing.statement` mention round 2.

**New v2 SHAs.** The v2 library SHA was `b871743e…e2ed` before this round.

| File | SHA-256 | Size |
|---|---|---|
| v2' library | `fd1e359b316842eeb04551f91135e2f1fb5e13bdc8e888d0450ddac509fe1389` | 36376 B |
| v2' recipe | `b7bdf06dd68aa26858802b7a7edceebe26fb38b827a89789c83e77992d5b4c21` | 51986 B |

**Changed dsl_sha256: exactly four candidates.** `ivol_change_21_252_s21`, `ivol_change_21_252_s63`, `ivol_change_63_qoq_s21` and `ivol_change_63_qoq_s63`. The other 92 are unchanged, so they still hit the cache. Ids, families, fields and lookbacks are unchanged, with max prior bars still 314.

## Step 2: library v3

### Files

All four files are in `atx-impl/strategies/`.

| File | Contents |
|---|---|
| `generate_pv_fields_ic121_v3.py` | Generator plus static validator. |
| `pv_fields_ic121_v3.json` | Library `pv_fields_ic121_v3`. SHA `5d164ea115c633677dae59de975c24c4882ee05430d03a7e1bafa6bc591cb9f8`, 48052 B. |
| `pv_fields_ic121_v3.recipe.json` | Recipe. SHA `ff1b0922edbecb22bbea19d46b6fbb93637ac3a4e38775319d6c9c85007a84b2`, 84688 B. |
| `test_generate_pv_fields_ic121_v3.py` | Synthetic pytest fixture. v2 has no separate fixture, but brief requirement 4 asks for one. |

### Provenance

- The generator loads the v2 generator (the same pattern v2 uses to load v1).
- It pins the v2' library and recipe SHAs above.
- It asserts that the committed v2 files equal the v2 generator's output.
- It writes `generation.generator_sha256`, the SHA of its own LF-normalized source. This equals the committed blob, `f3cf7a64…a30a`.
- The recipe lineage keeps each row's origin (`frozen_v1`, `new_v2` or `new_v3`).

### Counts and checks

- **Size:** 121 candidates (96 frozen v2 + 25 new) in 25 families. The runner allows at most 32 families and 256 candidates.
- **v2 prefix:** the frozen prefix is byte-identical: the same ids, JSON encoding, DSL bytes, dsl_sha256 and lineage rows, and the v2 families and fields form the prefix.
- **Lookback:** the library maximum is 314 (unchanged, from v1). The new candidates' maximum is 87 (`iv_change_21_s63`).
- **DAG and slots:** max DAG nodes 26, estimated peak slots 8. The new candidates' estimated maximum is 7 slots. Native `--plan-only` is authoritative.
- **Declared fields:** v2's four plus `si_shares`, `si_dtc`, `iv_atm_21d`, `iv_atm_63d`, `iv_atm_126d`, `earn_recent` and `shares_out`. They follow the v2 `{name, basis}` structure exactly, like `mkt_ret`.
  - Only referenced fields are declared, and the generator asserts declared equals referenced.
  - `mktcap_lagged` and `size_grp` are not used because their coverage is lower. `is_common` is not used because it is static, not point-in-time.
- **Candidates referencing extra fields (51 in total):**

  | Field | Candidates |
  |---|---|
  | `mkt_ret` | 29: 26 from v2 plus the 3 earnings candidates |
  | `si_shares` | 6 |
  | `si_dtc` | 3 |
  | `iv_atm_21d` | 12 |
  | `iv_atm_63d` | 2 |
  | `iv_atm_126d` | 2 |
  | `earn_recent` | 3 |
  | `shares_out` | 7 |

  The per-field id lists are in recipe `static_validation.extra_field_users` and are printed by `--check`.
- **Registry:** the validator adds `log` (Log) and `ts_backfill` (TsBackfill, rolling class) to the v2 table. `--check` prints "registry cross-check ok (19 operators vs registry.cpp/typecheck)".
- **Operator semantics:** both new operators are in the VM dispatch (`vm.hpp`), and `analyze()` treats them as ordinary F64 ops. No DAG simplification removes `0 * x`; only pow2 strength reduction exists.

### Family table

Every new base is written prior-positive: a negative prior is multiplied by -1, as in v1's `low_vol_63`. A TRAIN sign of +1 therefore means the prior was confirmed. This is cosmetic for the runner, which still fits the sign. Unless the table says otherwise, a candidate is `decay_linear(rank(base), s)`. When s = 1, the candidate is plain `rank(base)`.

| Ids (97-121) | Family | Formula (base) | Prior (sign on the raw quantity) | Citation | Windows / smoothing |
|---|---|---|---|---|---|
| `si_ratio_s1/s21/s63` | short_interest_ratio | `-1 * si_shares/shares_out` | high SI → lower returns (-) | Asquith, Pathak & Ritter 2005; Boehmer, Huszar & Jordan 2010 | s ∈ {1, 21, 63} |
| `dtc_s1/s21/s63` | days_to_cover | `-1 * si_dtc` | high DTC → lower (-) | Hong, Li, Ni, Scheinkman & Yan 2015 | s ∈ {1, 21, 63} |
| `si_change_{10,21,42}_s21` | short_interest_change | `-1 * (sir - delay(sir, n))`, where `sir = si_shares/shares_out` | rising SI → lower (-) | brief prior; same SI literature | n ∈ {10, 21, 42}, s = 21 |
| `iv_level_21_s21/s63` | implied_vol_level | `-1 * IVg21` | high IV → lower (-) | Ang, Hodrick, Xing & Zhang 2006 (IV analogue) | s ∈ {21, 63} |
| `iv_term_{63,126}_21_s{21,63}` | implied_vol_term_slope | `IVgL / IVg21` | inverted term structure → lower (+ on the ratio) | Vasquez 2017; Jiang & Tian-style slope | L ∈ {63, 126} × s ∈ {21, 63} |
| `iv_change_{5,21}_s{21,63}` | implied_vol_change | `-1 * IVg21 / delay(IVg21, n)` | IV rise → lower (-) | An, Ang, Bali & Cakici 2014 | n ∈ {5, 21} × s ∈ {21, 63} |
| `iv_rv_21_s21/s63` | implied_minus_realized_vol | `-1 * (IVg21 - stddev(ret,21) * 15.874507866387544)` | high IV-RV → lower (-) | Bali & Hovakimian 2009; Goyal & Saretto 2009 | s ∈ {21, 63} |
| `earn_car_{21,42,63}_s1` | earnings_drift | `ts_sum(earn_recent * (ret - mkt_ret), H) / sign(ts_sum(earn_recent, H))`, with no decay | positive drift (+) | Chan, Jegadeesh & Lakonishok 1996; Brandt, Kishore, Santa-Clara & Venkatachalam 2008 | H ∈ {21, 42, 63} |
| `size_mcap_s21` | size | `-1 * log(shares_out * raw_close)` | small > big, weak (-) | Banz 1981 | s = 21 (one variant) |

Here `IVgT = ts_backfill(guard(iv_atm_Td), 5)` (see the IV guard section) and `ret = (close / delay(close, 1)) - 1` (v2 spelling).

Candidate prior bars:

| Candidate | Prior bars |
|---|---|
| `si_ratio`, `dtc` | 0 / 20 / 62 (s1 / s21 / s63) |
| `si_change` | 30 / 41 / 62 (n = 10 / 21 / 42) |
| `iv_level` | 24 / 66 (s21 / s63) |
| `iv_term` | 24 / 66 (s21 / s63) |
| `iv_change` | 29 / 71 (n = 5) and 45 / 87 (n = 21), each s21 / s63 |
| `iv_rv` | 41 / 83 (s21 / s63) |
| `earn_car` | 21 / 42 / 63 (H = 21 / 42 / 63) |
| `size` | 20 |

### IV guard used

`guard(x) = x + 0 * log((x - 0.02) * (5 - x))` works as follows:

- For x in the open interval (0.02, 5) the log is finite, and `x + 0*finite` equals x exactly.
- Outside that interval the product is negative, so the log is NaN.
- At the two endpoints the log is -inf, and `0 * -inf` is NaN.
- A NaN input gives NaN.

So 1.2e16, 69.3, 5.0, 0.02, 0.01, 0 and negative values all become missing.

`ts_backfill(·, 5)` then carries the last valid guarded value for at most four further sessions (window t-4..t), and otherwise leaves NaN. It never fills zero.

**Why the backfill (a decision):** the producer caveat says vendor IV is "often null (~6.5% of high-volume rows)". Every full-window operation downstream (decay, delay ratio, rank-decay) turns one NaN into NaN for the whole window, so sporadic nulls would blank names for 21-63 sessions. The constant 5 is a data-hygiene staleness bound declared before measurement, not a signal grid. Root can drop it by setting `IV_BACKFILL`, which changes only the 12 IV DSLs.

Ranks are taken after the guard, so no single cell can dominate. Only `iv_rv` mixes in a non-IV term, and it annualizes it with sqrt(252) to IV units.

### Expected turnover class (per-alpha ceiling tau_k <= 0.70)

These are priors; tau_k is measured at admission.

| Family | Expected turnover | Reason |
|---|---|---|
| si_ratio, dtc | very low | Values change only on FINRA dissemination, about every 10.5 sessions. The s1 variants step on those sessions. |
| si_change | low | The change refreshes once per cycle, and the s21 decay spreads it out. |
| iv_level | low | Persistent level. |
| iv_term | low to moderate | Daily ratio of two persistent tenors. |
| iv_change | moderate, fastest v3 family | `iv_change_5_s21` is the fastest candidate. decay_linear 21 bounds even a white-noise base at lag-1 autocorrelation about 0.93 (one-way tau about 0.37). |
| iv_rv | moderate | Daily inputs, smoothed. |
| earnings_drift | low | The base is constant between events. Names enter on the reaction session and leave after H sessions: about 2/H of the covered names per session, so earn_car_21 is fastest. |
| size | very low | Slow-moving market-cap rank. |

### Fields referenced per family

| Family | Fields |
|---|---|
| si_ratio | si_shares, shares_out |
| dtc | si_dtc |
| si_change | si_shares, shares_out |
| iv_level | iv_atm_21d |
| iv_term | iv_atm_21d, plus iv_atm_63d or iv_atm_126d |
| iv_change | iv_atm_21d |
| iv_rv | iv_atm_21d, close |
| earnings_drift | earn_recent, close, mkt_ret |
| size | shares_out, raw_close |

### Causality

This follows the v2 convention: signal row d uses data known by the session-d close mark (the clock is modeled-session+22h-mark+23h-decision) and is scored on returns after d. Checked against `prepare_research_fields.py`:

- **`si_*`:** as-of joins with available_at (the official dissemination date) strictly before date(d), and NaN when older than 45 days.
- **`iv_atm_*` and `earn_recent`:** the same-date vendor end-of-day row, which is the close clock. earn_recent flags the reaction session (earnFlag 0, public by its close) and the session after (earnFlag 1). The producer maps -1, the flag for a presumed future event, to 0.
- **`shares_out`:** vendor count dated at or before d - 90 calendar days (A8 lag).
- **`mkt_ret`:** row d uses closes at d-1 and d.

Every window and delay is a positive integer literal, and the validator rejects zero, negative, fractional and non-literal windows. The fixture also checks this empirically: scrambling all fields after t0 leaves every new candidate unchanged up to t0.

### Borrow exposure (swap-fin-v1 special tier)

These families lean into special-tier names; nothing in the DSL works around it.

| Family | Exposure |
|---|---|
| short_interest_ratio, days_to_cover | High: the prior short leg is exactly SI/shares_out > 10% or small names. |
| iv_level, iv_rv | Moderate: high-IV names are small, low-priced or young. |
| si_change | Moderate. |
| iv_term, iv_change | Low to moderate. |
| earnings_drift | Low. |
| size | Low if the prior sign holds; high if TRAIN flips it. |

These are recorded per family in recipe `family_priors[].borrow_exposure`.

## Decisions (small ambiguities, recorded)

1. **si_change windows.** The brief says "~2 and ~4 FINRA cycles (~10 and ~21 sessions)", but one cycle is about 10.5 sessions. I used n ∈ {10, 21, 42} (1, 2 and 4 cycles) so both readings are covered. n = 10 can see no dissemination on some sessions, which gives tied zero changes; the s21 decay carries the previous cycle through those. This is recorded in recipe `data.short_interest_change` and `prior_caveat`.
2. **Grids, principle-based, 2-4 variants per family.**
   - Slow SI levels: s ∈ {1, 21, 63}.
   - Daily IV inputs: s ∈ {21, 63}; decay is required for the tau bound.
   - si_change: s = 21.
   - earnings_drift: no decay. Its base is NaN outside the hold window, and a full-window decay would blank it; H is its grid.
   - size: one variant. The brief allows one or two, and a second decay of a near-constant rank would be a duplicate trial.
   - Total: 25 new, within the brief's 24-32 and the ≤ 32 cap.
3. **Earnings without an event is NaN, not 0.** Per "never fill with 0", such a name is excluded that session. Coverage is roughly 1/3 of names for H = 21 and most names for H = 63.
4. **Size spelled literally as the brief** (`-1 * log(shares_out * raw_close)`). This is rank-equivalent to `-mcap`. `mktcap_lagged` is unused.
5. **Naming mirrors the v2 trio:** `generate_pv_fields_ic121_v3.py`, `pv_fields_ic121_v3.json` and `.recipe.json`. The library id is `pv_fields_ic121_v3`.

## Concerns

1. **Prior-sign caveats.** These are recorded pre-measurement in recipe `family_priors[].prior_caveat`. The DSL keeps the brief's declared signs, which does not matter to the runner's sign fit.
   - **implied_minus_realized_vol:** as I recall it (unverified here), Bali & Hovakimian 2009 find that (RV − IV) predicts returns negatively. That means high IV−RV → higher returns, the opposite of the brief. Goyal & Saretto 2009 is about option returns.
   - **implied_vol_change:** An, Ang, Bali & Cakici 2014 split the effect: a call-IV rise predicts higher returns and a put-IV rise lower. For ATM IV the direction is ambiguous a priori.
   - **implied_vol_term_slope:** Vasquez 2017 is evidence on option returns, so the stock-return direction is an extrapolation.
   - **Action for root:** ratify these before reading the TRAIN signs, so that a "prior confirmed/refuted" label is not judged against a questionable prior.
2. **Expected overlaps.** These are implementer priors, recorded in recipe `expected_overlap` and unmeasured:

   | v3 template | Overlaps with | Prior |
   |---|---|---|
   | iv_level_21 | v1 low_vol_63 | 0.7-0.85 |
   | size_mcap | v1 dollar_liquidity_63 | 0.85-0.95, plus the logADV63 neutralization |
   | dtc | si_ratio | 0.6-0.8 |
   | iv_rv_21 | iv_level_21 | 0.5-0.7 |
   | iv_change_21 | v1 vol_expansion_21_126 | 0.3-0.5 |

   The T11 greedy |rho| <= 0.70 screen culls actual duplicates.
3. **Data caveats**, which the DSL cannot fix and which are recorded in recipe `data.vendor_caveats`:
   - The vendor "clean" IV removes the earnings effect using a forward-looking earnings calendar, and its vintage is unproven.
   - The earn_recent calendar vintage is unproven.
   - SI settlements before 2021-06 come from a later FINRA republication.
   - A split between SI settlement and the session misstates si/shares_out by the split factor for at most about one cycle.
   - DTC is floored at 1 by FINRA, so about 40% of rows tie.
4. **mkt_ret warm-up sessions (integration, from the TRAIN manifest only).** `mkt_ret.stats.contributors_per_session.zero_contributor_sessions = 63`, not the 1 the T5 re-review hoped for. `score_window_min = 2757`, so none of these sessions falls in the score window.
   - They are presumably rows 0-62, the 63-session ADV warm-up.
   - Root should confirm they all lie before row 85 = score_begin 399 − 314. Otherwise the 29 mkt_ret candidates start NaN.
5. **Runtime.** Cold candidates for the first v3 TRAIN pass are 25 new + 4 changed ivol_change = 29. The warm 92 hit the cache.
   - The new candidates are cheap. Their lookback is at most 87; ts_backfill is O(5) per cell and log is element-wise.
   - The 121-candidate composition admission (`ic_composition_working_bytes`) grows compared with 96, so root should check memory on the plan-only run.
6. **T7 dependency.** The current root runner rejects any library whose declared fields are not exactly {close, raw_close, volume}. Both v2' and v3 need T7's relaxed field check plus `--train-fields`, which supplies all 8 extra fields; all of them are present in the TRAIN fields manifest.

## Tests (synthetic, no build, no real data)

- `python -B atx-impl/strategies/generate_price_volume_ic96_v2.py --check`: ok (fd1e359b / b7bdf06d, 17-operator registry cross-check).
- `python -B atx-impl/strategies/generate_pv_fields_ic121_v3.py --check`: ok (5d164ea1 / ff1b0922; 121 = 96 + 25; 25 families; max prior bars 314, new 87; 19-operator registry cross-check).
- `python -B -m pytest -q atx-impl/strategies/test_generate_pv_fields_ic121_v3.py -p no:cacheprovider`: 25 passed (about 8-13 s). The tests cover:
  - deterministic documents equal to the committed bytes;
  - a byte-identical v2 prefix (JSON text, DSL bytes, dsl_sha256, lineage, families, fields, pins);
  - new candidates parse, with prior bars and native prior bars matching lineage, and declared fields equal referenced;
  - 11 malformed-DSL rejections, including an undeclared field, the undeclared `mktcap_lagged`, fractional, zero, negative and non-literal windows, an unknown op, wrong arity and non-canonical text;
  - a numpy mirror of the pinned VM semantics, used for:
    - **causality:** scrambling all fields after t0 leaves every candidate at ≤ t0 unchanged;
    - **lookback sufficiency:** changing data before t0 − prior_bars leaves the t0 value unchanged;
    - **the IV guard:** 1.2e16, 69.3, 5.0, 0.02, 0.01, 0 and −0.3 each behave exactly as NaN in all 12 IV candidates (16 candidate-field pairs);
    - **backfill:** it is bounded at t-4 and admits an in-range value exactly;
    - **earnings gating:** NaN without an event, and exactly the flagged excess sum otherwise;
    - **orientation:** SI and size are prior-positive.
  - A mutation check (cap 5 changed to 100) makes the guard test fail, so the test has teeth.

## Root commands

```
cd <root tree>   # after cherry-picking 51b01e61 a20002f5
python -B atx-impl/strategies/generate_price_volume_ic96_v2.py --check
python -B atx-impl/strategies/generate_pv_fields_ic121_v3.py --check
python -B -m pytest -q atx-impl/strategies/test_generate_pv_fields_ic121_v3.py -p no:cacheprovider
```

Native compile check (after T7 lands): run the root's usual `equity-strategy-ic ... --plan-only` invocation with
`--library atx-impl/strategies/pv_fields_ic121_v3.json --library-sha256 5d164ea115c633677dae59de975c24c4882ee05430d03a7e1bafa6bc591cb9f8`,
the TRAIN role manifest and SHA, and T7's `--train-fields C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v1-fields-v1/manifest.json`.

Expected result:
- 121 candidates compile.
- `required_lookback` is 314. It must be ≤ score_begin − 63 = 336 on TRAIN.
- max compiled slots is about 8.
- 25 families.

Re-run v2' `--plan-only` the same way with `--library-sha256 fd1e359b316842eeb04551f91135e2f1fb5e13bdc8e888d0450ddac509fe1389`; the earlier `b871743e` result is stale.

No C++ build targets and no CMake registration are needed from this lane.
