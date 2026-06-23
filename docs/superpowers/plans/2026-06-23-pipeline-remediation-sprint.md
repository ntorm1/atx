# Pipeline Remediation Sprint — Reject Economically-Vacuous Alphas

**Date:** 2026-06-23
**Origin:** `atx-impl/research/2026-06-22-pipeline-degenerate-alpha-failure-analysis.md` (CORRECTED 2026-06-23).
**Branch:** `main` (direct, per user). NEVER push.

## Why (corrected diagnosis)
The strict acceptance sweep admitted exactly one alpha: `((ts_min(earnFlag, 45) ^ atmCenI_21d) / close)`.
Forensics (library catalog `oos_sharpe=2.27`, `turnover=0.27`; empirical panel measurement of `earnFlag`
values `{-1,0,1}` dense-in-universe) established this alpha is a **REAL trading book that holds out-of-sample**,
NOT a degenerate all-zero book. The original "fail-open / all-NaN" smoking gun is RETRACTED. The true failure:
the pipeline admits **economically-vacuous-but-statistically-fit** alphas because (a) flag/count/categorical
fields leak into numeric arithmetic, (b) `/close` is a price-scale (≈ 1/price size) bet with no edge, and (c)
admission has no economic/structural prior and weak statistical gates. A coverage/cardinality floor would NOT
have rejected this alpha — so it is demoted to an optional safety net, not a headline fix.

## Global Constraints (BINDING — every task)
- **Default (flag-absent) path BYTE-IDENTICAL** to today's bytes. Prove it per task (golden digests + a fresh
  off-path byte-identity test).
- **`oracle.hpp` UNTOUCHED.** No edit to any pinned golden digest literal.
- **F1 search digest sacred** on the flag-absent path. The opt-in flag-on path MAY produce a new deterministic
  digest, and MAY re-baseline the library manifest `version_id` (this is the ONLY sanctioned re-baseline).
- Determinism: no wall-clock / RNG / filesystem-order in any digest-affecting path. Field classification,
  basis factors, and gates are computed deterministically from the panel.
- **NEVER `git add -A`** — stage explicit paths. Commit trailer EXACTLY:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Each task ships three test classes: (1) off-path byte-identity (flag absent ⇒ digest/version_id unchanged),
  (2) on-path behavior change (flag on ⇒ digest/admission set diverges, with a genuine RED before the wiring),
  (3) twice-run reproducibility (flag on, run twice ⇒ identical).
- Pre-existing failures OUT OF SCOPE: `AtxImplPanel.BuildsPanelFromSegments`, `Alpha101Orats.*`,
  `AlphaSlotPoolDeathTest.*`, `AlphaVm_ZeroAlloc.*`, `RegimeCombineDeathTest.*` (Release/NDEBUG death tests).
- Build: `build-rel` (Ninja/Release). Test exes under `build-rel/bin/`.

## Sequence
R1 → R2 → R3 → R4. (P2 search-affordability / date-subsample DEFERRED to a follow-up sprint.)
Model policy: implementer = sonnet (anchored specs); reviewer = opus (determinism/admission-sensitive).

---

## R1 — Field-type discipline (opt-in `--typed-fields`) [the direct fix]

**Objective.** Keep binary-flag / low-cardinality / categorical fields (`earnFlag`, `nEarnCnt_5d`, `gics`, and
any field with tiny cross-sectional cardinality) OUT of the numeric leaf pool the grammar draws from, and route
the GICS classifier to the GROUP pool. This removes the structural hole that lets `ts_min(earnFlag,45)` form.

**Design.**
- Engine (`factory/search_driver.*`, `factory/config.hpp`/equivalent FactoryConfig): add two defaulted-empty
  members that the field-pool builder honors:
  - `std::vector<std::string> numeric_excluded_fields` — names removed from the numeric F64 leaf pool
    (`search_driver.cpp:37-41` partition, fed to `generate.hpp:108`).
  - `std::vector<std::string> extra_group_fields` — names added to the group pool (so `gics` is usable by
    group ops, not numeric arithmetic). Also fix `is_group_field` (`typecheck.hpp:124-128`) to accept these.
  - Both empty by default ⇒ today's partition EXACTLY ⇒ byte-identical F1 digest.
- Stage (`stage_discover.cpp` + `stage_sweep.cpp`): add CLI `--typed-fields` (bool, default false) and
  `--field-cardinality-max <K>` (int, default 12). When `--typed-fields`:
  - Scan the panel once (deterministic, single pass) computing per-field distinct-non-NaN cardinality.
  - A numeric field whose cardinality ≤ K, OR whose name matches a categorical/flag set
    (`earnFlag`, `nEarnCnt_5d`, `gics`), is added to `numeric_excluded_fields`.
  - `gics` (the real classifier) is added to `extra_group_fields`.
  - Pass these into FactoryConfig before `rd.run` / `mine_into`.
- Default (`--typed-fields` absent) ⇒ both vectors empty ⇒ identical search + digest + version_id.

**Files.** `factory/config.hpp` (or wherever FactoryConfig lives), `factory/search_driver.cpp/.hpp`,
`factory/typecheck.hpp`, `atx-impl/src/config.hpp/.cpp`, `atx-impl/src/stage_discover.cpp`,
`atx-impl/src/stage_sweep.cpp`, + the relevant `*_test` files. `oracle.hpp` UNTOUCHED.

**Tests.** (1) off-path: existing factory golden + a new `TypedFieldsOffByteIdentical` (no flag ⇒ pinned
research_digest + version_id). (2) on-path: `TypedFieldsExcludesFlagAndShiftsDigest` — RED pre-wiring (digest
equal) → GREEN (digest diverges; assert an excluded field cannot appear as a numeric leaf). (3)
`TypedFieldsDeterministic` twice-run identical.

**Acceptance.** With `--typed-fields`, a search over a panel containing `earnFlag`/`gics` never emits those as
numeric leaves; default path byte-identical (proven).

---

## R2 — Price-scale / dimensional prior at admission (opt-in `--reject-price-scale`)

**Objective.** Reject candidates whose holdout edge is explained by a trivial price-scale basis (≈ `1/price` /
low-price / size tilt) rather than genuine signal — directly rejecting the `/close` leak.

**Design.**
- CLI `--reject-price-scale <corr>` (double, default 0.0 = OFF). When > 0:
  - At admission, on the HOLDOUT window, build the cross-sectional price-scale basis factor `b = 1/raw_close`
    (per date, in-universe; z-scored cross-sectionally, NaN→excluded). Use `raw_close` (the level), consistent
    with `data/universe.cpp` notional.
  - Measure the candidate's cross-sectional book exposure to `b`: per date, correlation between the candidate's
    target weights and `b`; take the time-series mean |corr| (or the magnitude of the time-averaged
    cross-sectional regression loading). Call it `price_scale_loading`.
  - If `price_scale_loading ≥ threshold`, REJECT the candidate (do not admit).
  - Record `price_scale_loading` as additive telemetry on the FactoryReport/library entry regardless (so the
    metric is visible even when the gate is off — additive, not folded into any digest).
- Determinism: basis factor + correlation are deterministic functions of the panel + the candidate book. Flag
  default 0 ⇒ no rejection ⇒ admission set + version_id unchanged. Flag on ⇒ rejects some ⇒ version_id
  re-baselined (sanctioned).
- Anchor the admission insertion at the holdout gating site (`factory.cpp:1071-1120`); reuse the holdout book
  that already exists there. Keep the basis construction local/static; do NOT touch `oracle.hpp`.

**Files.** `factory/factory.cpp`, FactoryConfig, `atx-impl/src/config.hpp/.cpp`, stage wiring
(discover+sweep), `*_test`. Telemetry additive only.

**Tests.** (1) off-path byte-identity (threshold 0 ⇒ pinned digest/version_id unchanged + telemetry additive).
(2) on-path: a synthetic candidate whose book ∝ `1/price` is REJECTED at a low threshold but ADMITTED with the
gate off (RED→GREEN); a genuine market-neutral signal is NOT rejected. (3) twice-run identical.

---

## R3 — Gate tightening: per-window DSR AND + blocking PBO (opt-in)

**Objective.** Make the existing statistical gates actually bite. Two independent opt-in knobs.

**Design.**
- `--dsr-all-windows` (bool, default false). Today the holdout DSR gate checks a single aggregate window
  (`factory.cpp:~1120`). When set and `oos_windows > 1`, require `min_dsr` cleared on EVERY walk-forward
  window (logical AND across windows); a candidate failing any window is rejected. Default false ⇒ today's
  single-window check EXACTLY.
- `--pbo-blocks` (bool, default false). Today PBO is advisory and fail-opens at `n_candidates<2`
  (`factory.cpp:83-155,143-154`). When set, a candidate whose PBO ≥ `max_pbo` is REJECTED (un-admitted).
  Preserve the advisory behavior when the flag is absent (byte-identical) and keep the `n<2` fail-open only
  when the flag is OFF (when ON, an undefined PBO at n<2 must NOT silently admit — document the chosen
  semantics in the brief and test it).
- Determinism: both default false ⇒ identical admission + version_id. On ⇒ stricter ⇒ version_id re-baselined.

**Files.** `factory/factory.cpp`, FactoryConfig, `atx-impl/src/config.hpp/.cpp`, stage wiring, `*_test`.

**Tests.** (1) off-path byte-identity (both flags absent ⇒ pinned digests + the existing PBO golden digests
unchanged). (2) on-path: a candidate clearing aggregate-DSR but failing one window is rejected under
`--dsr-all-windows` (RED→GREEN); a high-PBO candidate is rejected under `--pbo-blocks`. (3) twice-run identical.

---

## R4 — Deflate during selection (opt-in `--deflate-selection`)

**Objective.** Make the GA optimize DEFLATED edge during search, not raw in-sample `wq`. Today
`FitnessCfg.trial_count = 1` during search (`fitness.hpp:236`); deflation only bites once at admission against
the cumulative N. The search therefore spends its whole budget chasing in-sample fit.

**Design.**
- CLI `--deflate-selection` (bool, default false). When set, the search-time fitness uses the running trial
  count (the count of candidates evaluated so far this run, or the cumulative library N if available) as the
  DSR deflation N inside the selection fitness, so selection pressure tracks deflated DSR. When absent,
  `trial_count` stays 1 ⇒ identical search ⇒ identical F1 digest.
- Determinism: the running count must advance deterministically (no wall-clock / thread-order dependence) —
  reuse the existing deterministic evaluation counter the engine already maintains for R1 cumulative trials.
  Flag off ⇒ digest byte-identical. Flag on ⇒ different selection ⇒ new deterministic digest.

**Files.** `factory/fitness.hpp/.cpp`, `factory/search_driver.cpp`, FactoryConfig, `atx-impl/src/config.hpp/.cpp`,
stage wiring, `*_test`. `oracle.hpp` UNTOUCHED.

**Tests.** (1) off-path byte-identity (flag absent ⇒ pinned F1 digest unchanged). (2) on-path: flag on ⇒ search
digest diverges from default (RED→GREEN), and the selected population reflects deflation (a high-N run prefers a
more robust candidate than the raw-wq winner on a constructed fixture). (3) twice-run identical.

---

## After all tasks
- Update `scripts/canonical-acceptance-run.ps1` to compose the new flags (`--typed-fields`,
  `--reject-price-scale`, `--dsr-all-windows`, `--pbo-blocks`, `--deflate-selection`) in the strict profile.
- FINAL whole-branch review (opus): determinism contract upheld, oracle.hpp untouched, every flag-absent path
  byte-identical, only version_id re-baselined on flag-on admission changes.
- DEFERRED follow-ups: P2 search-affordability (date-subsample per candidate during search → raise generations,
  cut immigrants/novelty in exploit phase; expose pop/gen/immigrants as flags); trivial-basis (momentum/size)
  extension of R2; coverage/cardinality safety gate.
