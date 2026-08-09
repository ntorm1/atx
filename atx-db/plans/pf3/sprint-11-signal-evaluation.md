# Sprint PF3-S11 — Signal evaluation surface (IC / decay / quantile spread / turnover / crowding)

**Goal:** score every factor in the exported panel for SIGNAL. The warehouse now *produces* factors (S8/S9) and
*exports* a PIT-safe, schema-hashed panel (S10), but nothing tells a researcher which of those factors actually
carries alpha. This sprint builds that verdict layer: rank information coefficient (rank-IC) and IC decay against
forward returns over a horizon ladder, quantile/decile long-short spread returns and hit rate, factor turnover,
factor-to-factor correlation / crowding, and cross-sectional breadth — plus factor-level data-quality checks
(leakage + coverage) registered as **GATED** (`severity=critical`) checks. Scoring is what makes the panel
something a researcher can "mine for signal" rather than a bag of undifferentiated columns, and it closes the loop
on the pf3 product: content is produced, exported, and now *evaluated*. Reserved migrations 0168–0171.

**Mandate / Owns:** NEW `db/signal_eval.py` — the pure IC / IC-decay / quantile-spread / turnover /
correlation-crowding / breadth transforms plus their persistence and manifests; factor-level DQC (leakage +
coverage) registered inside the (S3-decomposed) `quality` package as gated checks; `db/tests/test_signal_eval.py`.
The evaluation surfaces land under migrations 0168–0171 with their own `table_catalog` / `field_catalog` rows
(clause E) and reuse the alpha-backtest manifest shape (`backtest_id` / `params_json` / `source` / `run_id`)
already established by `alpha_research.py`.

**Must NOT touch:** the panel *materialization* — S10 owns `db/factor_panel.py`, `v_factor_panel`, and the
Parquet/Arrow export; S11 *reads* the exported panel and never rewrites it or its schema-hash. The factor engine,
factor families, and cross-domain assembly (S7–S9, the `db/factors/` package) are frozen inputs — S11 does not
add, redefine, or re-neutralize a factor. Critically, **no forward-return LOOKAHEAD may leak back into the factor
values themselves**: forward returns exist in this sprint only as a scoring *target*, never as a factor *input*.
Do not edit a landed migration (≤ 0167) or another sprint's reserved region.

**Depends on:** PF3-S10 (the exported factor panel + its schema-contracted PIT views — the object under
evaluation) and PF3-S4 (forward returns from the dense historical price backfill, without which IC/decile/spread
arithmetic has too few overlapping security-days to be meaningful). Sequential **after S10**; this is the last
content sprint before the S12 capstone, which gates and observes the surface S11 produces.

---

## Baseline / where the cycles go

Measured 2026-07-04 against `atx-impl/db`. Factors can now be built and shipped, but there is no honest,
per-factor verdict on whether any of them predicts returns.

1. **Factors are produced and exported but NOT scored.** S8/S9 emit fundamental and cross-domain factor families
   into one namespace, and S10 exports them as `v_factor_panel` + partitioned Parquet/Arrow. Nothing then asks
   *does this factor predict forward returns* — there is no per-factor rank-IC, no IC-decay-across-horizons, no
   turnover surface. A factor with zero signal is indistinguishable from the best factor in the panel: both are
   just columns.
2. **The only IC/spread machinery that exists is per-ALPHA, not per-FACTOR.** `db/alpha_research.py` runs a
   backtest over composite *alpha expressions* — `corr(signal_value, forward_return) AS rank_ic` (line 373),
   `hit_rate` (line 388), a top/bottom-`quantile` long-short (lines 69–70, 131–136), forward return via
   `lead(close, ?) OVER (PARTITION BY security_id ORDER BY trade_date)` over `equity_daily_bars` (line 349),
   written to `alpha_backtest_manifests`. This is the right *shape* but the wrong *grain*: it scores a handful of
   hand-authored alphas, aggregates one number per alpha, and computes no IC decay, no factor turnover, no
   crowding. There is **no per-factor evaluation surface** over the exported panel.
3. **No crowding / correlation view exists to detect redundant factors.** The word "crowding" already appears in
   the codebase (`db/short_interest_metrics.py`, lines 59/141) but it means *short-interest days-to-cover
   crowding* — an entirely different concept. There is no factor-to-factor correlation matrix and no notion of a
   factor being redundant because it duplicates others already in the namespace. A dozen near-collinear value
   factors could ship as "twelve signals" with no signal that they are one.
4. **There is no gated factor DQC.** A factor that is accidentally leaky (correlated with the contemporaneous or
   future return because a lag was dropped upstream) or effectively empty (present for a tiny fraction of the
   universe) would pass every existing check and ship silently into the panel a researcher trusts. Leakage and
   coverage are not asserted anywhere at the factor grain.

**Already good — do not regress:**
- **The exported panel and its schema-hash.** S10's `v_factor_panel` + Parquet/Arrow export and its
  `schema_sha256` stamp are the fixed object under evaluation; S11 reads them read-only and must not perturb the
  export contract.
- **`equity_price_metrics` as the forward-return input.** The S9 price/liquidity surface
  (`db/equity_price_metrics.py`) plus the S4 dense price backfill are the canonical forward-return inputs; S11
  derives horizon returns from them rather than re-deriving prices.
- **The alpha manifest pattern.** `alpha_research.py`'s manifest discipline (`backtest_id` hash + `params_json` +
  `source` + `run_id`, persisted to `alpha_backtest_manifests`) is the template the S11 evaluation manifests
  reuse — extended to per-factor grain, not reinvented.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal / no-lookahead, **(C)** offline no-network tests, **(D)** determinism + provenance,
**(E)** schema-as-contract, **(G)** quality-gated, and **(I)** panel PIT-safety apply in full. The load-bearing
constraint of this sprint is that **the evaluation itself must be honest**: forward returns are used ONLY as a
scoring target (t+1..t+h), and are never fed back into any factor value.

- **(A)/(I)** Every score is computed cross-sectionally *within a single as-of cross-section* and only then
  aggregated across dates. Forward returns are strictly future-dated relative to the factor's `as_of_date`; the
  factor value at `as_of_date` may use only inputs with `available_at ≤ as_of_date`. No pooled (date-mixing)
  correlation that would let one date's future leak into another's score.
- **(D)** All `compute_*` scorers are pure (panel + forward-return frames in → long DataFrame out), unit-tested
  independent of DuckDB; identical inputs + params produce identical rows; every score row records the factor id,
  horizon, date window, and `run_id` lineage.
- **(E)/(B)** Migrations **0168** (IC + IC-decay surface), **0169** (quantile/decile spread + turnover), **0170**
  (correlation/crowding + breadth), **0171** (factor DQC catalog + indexes). Given the 1-migration-per-task-group
  budget, each migration seeds its tables *and* their `table_catalog`/`field_catalog` rows *and* their indexes in
  the same reserved number; strictly within 0168–0171, never editing a landed migration. Timestamped DB+WAL
  backup before any live apply (clause F).
- **(C)** Every test runs against in-memory / template-copy DuckDB with fixture factors and fixture forward
  returns — including a deliberately-planted leaky factor and a deliberately-sparse factor. No network. Live
  proof-slice counts are operator-run and recorded in the ledger.
- **(G)** The two factor DQC checks (leakage, coverage) are authored `severity=critical` and wired gate-ready so
  S12 halts a run on a red factor-DQC result.

---

## Tasks

### S11-0 — Information-coefficient surface (rank-IC + IC decay)

**Root cause:** the exported panel is unscored — there is no per-factor rank-IC and no view of how predictive
power decays with horizon. The only IC in the tree is `alpha_research.py`'s per-alpha
`corr(signal_value, forward_return)`, which is the wrong grain and computes no decay.

**Fix:** NEW `db/signal_eval.py::compute_information_coefficient(panel, forward_returns, horizons)` — a pure
transform that, for each `(factor_id, as_of_date)` cross-section, computes the Spearman rank correlation between
the factor value and the forward return at each horizon `h ∈ {1, 5, 10, 21, 63}`, then aggregates across dates
into mean rank-IC, IC information ratio (mean/std), an IC t-statistic, and sign-consistency. The **IC decay** is
the rank-IC profile across the horizon ladder (how fast predictive power falls off). Persist to `factor_ic` and
`factor_ic_decay` with an evaluation manifest mirroring the alpha-backtest shape (migration **0168**, catalogued +
indexed). Forward returns are derived from `equity_price_metrics` / the S4 dense backfill.

**PIT:** rank-IC is computed cross-sectionally *per as_of_date* and only then averaged — no pooled cross-date
leakage (A/I); forward returns are strictly t+1..t+h with `available_at` respected; transform is pure and
deterministic (D).

**Accept:** on the proof slice every factor in the panel receives a rank-IC row and an IC-decay row across all
horizons; a fixture factor constructed with zero relationship to returns yields rank-IC ≈ 0; a fixture factor
with a persistent-but-fading relationship yields a monotonically-decaying IC profile; identical inputs reproduce
identical rows.

### S11-1 — Quantile / decile spread + turnover

**Root cause:** there is no per-factor decile long-short spread, hit rate, or turnover. `alpha_research.py` has a
top/bottom quantile long-short but only per composite alpha and as a single aggregate; factor-level turnover /
rank-autocorrelation does not exist (the codebase's "turnover" in `short_interest_metrics.py` is share turnover,
unrelated).

**Fix:** `compute_quantile_spread` sorts each date's cross-section into deciles (configurable quantile count),
computes the top-minus-bottom decile forward return, the per-decile mean returns (to check monotonicity), and the
long-short hit rate. `compute_turnover` measures name-level factor turnover — the fraction of top/bottom-decile
membership that changes rebalance-to-rebalance — plus factor rank autocorrelation (how stable the ranking is
across consecutive as-of dates). Persist `factor_quantile_spread` and `factor_turnover` with manifests (migration
**0169**, catalogued + indexed).

**PIT:** quantile buckets are formed *within the as-of cross-section only*; forward returns are future-dated;
turnover compares consecutive as-of dates in forward chronological order with no lookahead (A/I).

**Accept:** decile spread, per-decile returns, hit rate, and turnover emit per factor on the slice; a monotone
fixture factor shows monotonically increasing decile returns and a positive long-short spread; a random-walk
fixture factor shows roughly uniform decile returns and high turnover; deterministic.

### S11-2 — Correlation / crowding + breadth

**Root cause:** there is no factor-to-factor correlation matrix and no crowding score, so redundant / collinear
factors ship as if independent; and there is no cross-sectional breadth/coverage view telling a researcher on how
many names a factor is actually defined each date.

**Fix:** `compute_factor_correlation` builds the pairwise cross-sectional correlation of factor values (and,
separately, of factor decile returns) averaged over dates into a correlation matrix. `compute_crowding` derives a
per-factor crowding score = the max (and average) absolute correlation of a factor to the rest of the namespace —
a factor highly correlated with many others is crowded/redundant. `compute_breadth` computes per-date
cross-sectional breadth/coverage (count of non-null names, effective breadth). Persist `factor_correlation`,
`factor_crowding`, and `factor_breadth` (migration **0170**, catalogued + indexed).

**PIT:** correlations are computed cross-sectionally per date and then aggregated (no date-pooling leakage);
breadth is an as-of coverage measure over the as-of universe (A/I); pure and deterministic (D).

**Accept:** the correlation matrix and crowding scores emit for the panel; two near-duplicate fixture factors are
flagged as mutually crowded (high pairwise correlation, high crowding score); per-date breadth matches the known
coverage of the fixture panel.

### S11-3 — Factor DQC gated (leakage + coverage)

**Root cause:** nothing asserts factor-level data quality, so a leaky factor (its lag dropped upstream, so it
correlates with the same-day or future return) or an effectively-empty factor could ship into the panel silently.

**Fix:** register two first-class checks in the (S3-decomposed) `quality` package, reusing the existing
`QualityResult` machinery. **(a) Leakage:** a factor whose correlation with the SAME-day (t+0) forward return
exceeds a chance threshold is flagged — a correctly-lagged factor should have ≈ 0 contemporaneous correlation, so
a strong same-day correlation is the signature of lookahead. **(b) Coverage:** a factor must be present for ≥ X%
of the as-of universe. Both authored `severity=critical` and wired gate-ready (clause G). Migration **0171** adds
the factor-DQC catalog table + indexes.

**PIT:** the leakage check uses the same-day (t+0) forward return purely as an *adversarial probe*, never as a
scoring target, and never writes it back into any factor value; coverage is measured against as-of universe
membership (S4), not a pooled roster.

**Accept:** the leakage DQC is RED on a planted leaky fixture factor (deliberately correlated with the t+0 return)
and GREEN on real, properly-lagged factors; the coverage DQC is RED on a deliberately-sparse fixture factor and
GREEN on a well-covered factor; both checks are authored gate-ready so S12 can halt on them; existing `quality`
checks are unaffected.

---

## Sequencing & expected compounding

**S11-0 → S11-1 → S11-2 → S11-3.** IC first: it is the load-bearing predictiveness measure that every later view
references and the cheapest honest verdict on a factor. Then quantile spread + turnover, the return-and-stability
view that turns rank-IC into economically legible long-short performance. Then correlation / crowding + breadth,
the redundancy-and-coverage view *over the now-scored set* — you can only judge crowding once you know which
factors are worth keeping. DQC last: it gates the fully-scored surface. **Compounding:** once every factor is
scored (IC / decay / turnover / crowding / breadth) and factor DQC is gate-ready, the S12 capstone can gate the
orchestrator on factor-panel quality and observe signal quality (freshness / anomaly / lineage), and a researcher
can select the factors that actually carry signal instead of guessing — which is the entire point of the pf3
product.

---

## Risks / guardrails

- **The evaluation must not itself leak.** Forward returns are strictly future-dated and used only as the scoring
  target; a dropped lag in the scorer would silently manufacture predictive power. IC and every correlation are
  computed cross-sectionally per date and only then aggregated — never pooled across dates — so one date's future
  cannot leak into another's score.
- **The leakage DQC is adversarial by design.** A deliberately-leaky fixture factor (correlated with the t+0
  return) MUST be flagged RED; if the leakage check passes it, the check is broken. The planted-leaky and
  planted-sparse fixtures are the acceptance backbone.
- **Read the panel, do not rewrite it.** S11 never touches S10's `v_factor_panel` / export or its schema-hash,
  nor the S7–S9 factor engine/families; forward returns never re-enter a factor value.
- **Stay in lane.** Strictly migrations 0168–0171; each seeds its own catalog rows + indexes; reuse the
  alpha-backtest manifest pattern rather than inventing a parallel one; never edit a landed migration.

---

## Bench / acceptance

- Every factor in the exported panel is scored on the proof slice: rank-IC + IC decay across the horizon ladder
  (S11-0), decile long-short spread + hit rate + turnover (S11-1), and per-date breadth (S11-2).
- The correlation / crowding surface emits: a correlation matrix over the namespace and a per-factor crowding
  score, with two near-duplicate fixture factors flagged as mutually crowded.
- The leakage DQC is RED on a planted leaky factor and GREEN on real factors; the coverage DQC is RED on a sparse
  factor and GREEN on a well-covered factor; both authored `severity=critical` and gate-ready.
- `python -m pytest atx-impl\db\tests\test_signal_eval.py -q` green, and full `python -m pytest atx-impl\db\tests
  -q` green before commit.
- **Live-DB smoke** recorded in the ledger: per-factor IC / decay / turnover / breadth counts on the slice, the
  crowding surface row counts, the factor-DQC pass/fail tallies, and the `run_id`.
- `PARITY_GAP.md` status updated (factor-level signal evaluation now present); a `WAREHOUSE_PARITY_TRANCHES.md`
  row appended (start/end SHA, domains, verification commands, live smoke with exact counts + run_id, caveats/next
  → PF3-S12 capstone gating + observability).

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD +
verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. New module ⇒
new `test_*.py`. Commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
