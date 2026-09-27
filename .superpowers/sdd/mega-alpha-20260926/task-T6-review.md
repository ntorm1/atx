# Task T6 review: PIT research-field producer (commit cf36d83c)

Package: `review-T6.diff` (1 commit, 2 new files, +1393). Line references are file lines at cf36d83c:
P = `atx-engine/tools/prepare_research_fields.py`, T = `atx-engine/tools/test_prepare_research_fields.py`.
Evidence also comes from the two published field manifests (TRAIN `recent-fast-train-2020-2022-v1-fields-v1`,
VAL `recent-fast-validation-2023-2024-v1-fields-v1`). Only their JSON was read, not the `.f64` matrices.

### Spec Compliance

- ❌ **One issue found, and the brief mandated it.** `is_common` (P:132-138, P:725-744) is built from a
  2026-09-18 directory snapshot plus whole-history vendor earnings evidence. It is still on by default
  (`DEFAULT_FIELDS = tuple(FIELDS)`, P:148). That breaks the global constraint "no look-ahead from ... snapshot data" in
  **both** roles, validation included. The brief asked for this field. The implementer disclosed the problem, but only as prose
  (clock string `static-line-classification;NOT-point-in-time`). See Important 2.
- ✅ **Everything else the brief asked for is present:**
  - The output directory is exclusive, and an existing empty directory is refused too (P:839; T:389-398).
  - Fields align to the role axes, with sessions/ids/member sha pins, a re-pin at the end of the run, and no re-projection (P:245-291, P:886).
  - FINRA uses strict `available_at < date(session)`, a 45-day staleness cap, and "visible NaN stays NaN" (P:445-452).
  - A role that reaches 2025 is refused before `mkdir` (P:279-280). Each source also drops its 2025+ rows and counts them (P:429-431, P:518-519, P:680).
  - The manifest has everything the brief listed (P:868-906): role pins, per-field sources + sha, clock, staleness, units, per-year member coverage, and per-file bytes/sha.
  - The CLI matches the brief (P:912-925).
  - The deviations are justified with evidence in `excluded_source_columns` (P:150-159): `hv_*` dropped, A8-lagged `shares_out`, added `size_grp`, corrected lake path, and `mkt_ret` per the root addendum.
- ⚠️ **Cannot verify from the diff:**
  - **Vendor vintage of `atmCenI_*` and `earnFlag`.** The IV looks cleaned using the vendor's earnings calendar. This is disclosed, and this code cannot prove it either way. Read IV and earnings families' validation results with that caveat.
  - **How many IV cells are garbage.** The manifest only has min/max/mean. Before T8 settles its IV guard, the controller should count member cells outside (0.01, 10] per tenor and role in the published `.f64` files.
  - **Commit trailer.** The brief asked for `Co-Authored-By: Claude Opus 5.5 (1M context)`. The package has no commit body, so this is unchecked.

### Strengths

- **The C++ as-of join is replicated faithfully.**
  - I checked it against `atx-impl/src/asof_field.hpp:17-24`: strict `<`, NaN when the age *exceeds* `max_stale_days`, latest row wins with no skip-back, and unknown ids are ignored.
  - The vectorised lookup at P:445-452 is correct: a (col<<32|day) key, a `searchsorted` "left"−1 for strict `<`, and a −1 sentinel that is never visible.
  - Tests cover the 45/46-day boundary, same-day invisibility and visible-NaN (T:248-271).
- **The `mkt_ret` guard matches `strategy_target_replay.cpp:164-172` term for term** (P:782-788). I checked it. The test compares the output against an independent loop (T:166-185) and covers both guard paths, the member-at-d−1 rule and the present-only broadcast (T:338-366).
- **Pinning is tight, and refusals fail closed.**
  - The TickerHistory sha must equal the role's `source_sha256` (P:491-493).
  - The FINRA CSVs must match the producer receipt (P:419-422).
  - Lake files are hashed from the exact bytes that get parsed (P:625-634).
  - Role close/raw/present are hashed as they are streamed (P:765-771, P:807-809).
  - Every refusal is tested to leave no manifest (T:419-448).
- **Deterministic, publish-last manifest.** No wall-clock values, `xb` plus fsync plus `os.link`, and byte-identical reruns are tested (T:368-387).
- **Look-ahead exclusions are backed by evidence:** `earnFlag −1` → 0, `nEarnCnt_*` and `atmCenH_*` dropped, and an A8 90-day lag on vendor shares.
- **Checked in the real manifests:** TRAIN and VAL used the same lake manifest (sha `c3a21f39…`). No FINRA row was published on or after 2025 (404k rows dropped). There are no off-calendar vendor rows, and 197/199 duplicate keys were quarantined. Spine formations are contiguous (72 / 60) and end at 2022-12-30 / 2024-12-31.
- The test file sits next to `test_prepare_recent_research.py`, as the brief asked.

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

**1. IV fields have no plausibility check, so garbage values ship under a field that claims to be "annualized decimal IV".**

The code (P:537-541) only turns null, non-finite and ≤ 0 values into NaN. Evidence from the published manifests:

- VAL `iv_atm_126d` has a member max of **1.1872e16**. The manifest mean for that field is therefore 4.96e9, so the summary statistic itself is useless.
- `iv_atm_21d` and `iv_atm_63d` both have max **69.3011** in both roles. The same value in two tenors points to a vendor garbage row, not a genuine extreme.
- Minimum values are 1e-4 to 3e-4, which is also implausible.
- TRAIN `iv_atm_126d` peaks at 51.5, but VAL reaches 1.19e16. So IV candidates frozen on TRAIN would meet a different kind of garbage out of sample.

This is a producer defect, not something to leave to the DSL:

- **The producer owns units and the NaN = "not usable" meaning.** It already applies domain guards to its other fields: the A9 ceiling (P:557-559) and the rough_return guard (P:787-788).
- **The DSL cannot turn a value into missing.** Its registry has `min`/`max`/`winsorize`/`rank` but no op that maps a value to NaN (`atx-engine/src/alpha/registry.cpp`, checked). T8's "[0.02, 5.0] → missing" can therefore only be written as a clamp. A clamp turns a 1e16 garbage cell into a believable 5.0, which then feeds rank and ts ops.
- **A single cell poisons mean/std ops.** `zscore`, `scale`, `vec_avg`, `ts_mean` and `ts_zscore` all exist (registry.cpp:35-97). For example, `ts_mean(iv_atm_126d, 63)` carries one garbage cell for 63 sessions, and `zscore` flattens that whole session's cross-section.
- **Otherwise every consumer has to re-implement the same guard.**

Fix:

- Declare a documented domain in FIELDS, for example 0.01 < IV ≤ 10. Justify the ceiling: meme-stock 21d ATM spikes reach about 5-8.
- At P:539-541, set values outside the domain to NaN (do not clamp), and count them per tenor in `source_checks` (for example `iv_out_of_domain`).
- Add p1/p50/p99 to `coverage` (P:338-350), so tails show up without opening the matrices.
- Add a fixture cell, for example id 303 at t=13 with IV = 1e16 → NaN (T:64-68 area).

The re-run is cheap (about 22 s for TRAIN). Regenerate before T7/T8 pin the field shas. Rank/winsor in the DSL stays useful as a second layer of defence.

**2. `is_common` is not point-in-time and is on by default, and the manifest only marks this in prose. The brief mandated the field** (P:132-138, P:148, P:725-744, P:905).

I checked the spine classifier (`C:/atx/atx-db/src/atx_db/research/spine.py:819-852`). It uses the 2026-09-18 directory type only for lines that are still listed then. Every other line becomes `common_unverified` (it had earnings at some point) or `unknown`. So the type encodes whether a line survived to 2026:

- `ETF`, `ADR`, `REIT` and `LP` (all → 0) are lines that survived.
- `unknown` (→ 0) are lines that died without ever reporting earnings.
- "`is_common`==0 with earnings events" singles out surviving ADR, REIT and LP lines.

This is look-ahead in the **validation** role, not only in TRAIN selection, so it can inflate the out-of-sample verdict itself. The T8 brief lists `is_common` as a DSL field, so it would reach the alpha search.

Fix:

- Add a machine-readable `"point_in_time": true|false` to every field entry.
- Take `is_common` out of `DEFAULT_FIELDS` so it becomes opt-in.
- T7 should refuse `point_in_time: false` fields for DSL panels unless they are explicitly allowed, and T8 should not build families on this field.

#### Minor (Nice to Have)

**1. The FINRA republication before 2021-06 in TRAIN cannot be masked by a machine.** The brief said to record it, and the implementer did.

- The TRAIN manifest reports `vintage_risk.finite_member_cells` = 2,070,589. That covers the warm-up plus 2020-01..2021-06, which is roughly 40% of the SI cells in the score window. VAL has 0.
- The out-of-sample verdict is unaffected. But SI families are signed and weighted on partly republished values, which the global constraint forbids in principle.
- The cutoff exists only inside the prose `rule` string (P:462-464).

Fix: emit a structured `vintage_safe_from: "2021-06-10"`, and optionally an opt-in flag that sets earlier cells to NaN. The controller can then choose to select SI on 2021-06..2022-12 only.

**2. The manifest does not say what a NaN in `mktcap_lagged` means. Guidance for the borrow-tier consumer follows.**

- **Why coverage is about 73%:** coverage is 0.751 in TRAIN and 0.729 in VAL. That matches the `is_common` member fraction (0.753 / 0.731) to within 0.2 pp. So a NaN means "the line is outside the spine universe" (non-common types, by construction). It does not mean "large", and it is not a data gap. The `staleness` text (P:119-125) only says "a line without a spine row".
- **What that does to `estimate_borrow_tier`:** I checked `atx-engine/src/cost/borrow_tiers.cpp:24-27`. It returns `InvalidArgument` if `available` is set while any predictor is NaN. A consumer that uses `mktcap_lagged` alone would therefore push about 27% of member cells (ETFs, ADRs, unknown types) to Unavailable.
- **The T10 plan reads this correctly:** fall back to `shares_out*raw_close` (99.7% coverage), then treat a still-missing value as warm, count those cells, and declare the policy before validation.
- **Smaller points for the consumer:**
  - `available_at_ns` can be set to the session's 22:00 UTC mark for every T6 field. Every finite cell is known by then, which is before the 23:00 decision.
  - `si_shares/shares_out` understates the SI-to-float predictor, because it divides by shares outstanding rather than float. T10 already declares this.
  - The role warm-up (399 / 401 sessions, about 1.6 years) is longer than 365 days. So "first present in the role" is not truncated anywhere in the score window.

Fix in T6: state the NaN meaning in `staleness`/caveats.

**3. The `shares_out` restatement lacks the spine's artifact-break guard (C-79)** (P:599-617).

- The spine restates with repaired factors and withholds values across breaks (spine.py:999-1011, checked). T6 uses the raw `cumulReturnFactor` ratio instead.
- Evidence: member minimum of **20 shares** in both roles, and a VAL maximum of 3.12e10.
- T10 uses `shares_out` for the SI ratio and for the market-cap fallback, so one bad restatement can flip a borrow tier.

Fix: set a cell to NaN when the restated count is implausible (for example `|log(crf_t/crf_lag)|` beyond a split-range bound, or a restated count below 1e4), and count those cells.

**4. C-81 withholding uses rows after the session** (P:557-559, P:612).

- One above-ceiling row anywhere in [first−490d, last] sets the line's whole history to NaN. That is look-ahead in which cells are present, not in their values.
- Scale: 1 line in VAL, 0 in TRAIN.
- It follows house ruling C-81, and the `staleness` text says so. Optional fix: withhold only from the first offending row onward.

**5. Nothing bounds how old the spine formation can be** (P:718).

- Month contiguity is checked (P:700). What is not checked is that each session's latest formation is within about a month.
- If the final lake year were only partly written, the tool would silently serve a stale formation for months. The real runs are fine.

Fix: refuse when `role.days[t] - fdays[f] > 35`.

**6. `code_sha256` depends on line endings** (P:904).

- Both manifests record `892bd33f…`. I verified that this is the CRLF form of the committed blob `b506bd81…` (the pool-8 LF working copy hashes to `b506bd81…`). So the code that ran is the committed code, but the pin itself changes with line endings.

Fix: hash LF-normalised bytes, or record `git hash-object`.

### Checks run (named risk → check)

| Risk | What I checked | Result |
|---|---|---|
| As-of fidelity | `atx-impl/src/asof_field.hpp:1-27` | Matches |
| `mkt_ret` guard fidelity | `atx-impl/src/strategy_target_replay.cpp:164-172` | Matches |
| Restatement direction and break handling | `atx-db/src/atx_db/research/spine.py:84-100, 999-1011` | Direction matches; C-79 guard absent (Minor 3) |
| `is_common` semantics | `spine.py:812-853` | Encodes survival to 2026 (Important 2) |
| Can the DSL absorb IV garbage? | `atx-engine/src/alpha/registry.cpp` op list | No op turns a value into NaN; mean/std ops present (Important 1) |
| Borrow-tier NaN handling | `atx-engine/include/atx/engine/cost/borrow_tiers.hpp`, `src/cost/borrow_tiers.cpp:14-39` | NaN with `available` set is an error (Minor 2) |
| Provenance of the executed code | sha of the pool-8 file, LF vs CRLF, vs manifest `code_sha256` | Same code, CRLF bytes (Minor 6) |
| Real-output sanity | Both field `manifest.json` files: coverage, min/max/mean, `source_checks` | Findings above |

### Assessment

**Task quality:** Needs fixes

**Reasoning:** The joins, clocks, pinning and refusals are correct and well tested, and the FINRA and `mkt_ret` semantics match the C++ they replicate exactly. Two things must be fixed before downstream pins these fields:

- The IV fields pass through vendor garbage (up to 1.19e16) under an "annualized decimal" contract that the DSL cannot turn back into missing.
- `is_common` is a default-on field that encodes survival to 2026 and leaks into the validation role.

Both fixes are small: a declared IV domain with NaN outside it, and a machine-readable PIT flag with `is_common` made opt-in. After them, a re-run takes about a minute.
