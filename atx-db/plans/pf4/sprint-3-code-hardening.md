# Sprint PF4-S3 — pf3 S1–S10 hardening + suite re-green (close the review findings, TDD + regression-locked)

**Goal:** the pf3 S1–S10 factor/valuation/panel stack is architecturally sound (a 2026-07-06 production-readiness
review found no cross-date leakage, `available_at = max(input.available_at)` consistently propagated, and uniformly
parameterized SQL) but carries **1 High / 5 Medium / 5 Low latent defects** and rides on a **date-time-bombed, drifted
offline suite** (~8 failures on the integration tree — `formula_registry` `valid_to` fixtures that expired once the
wall clock reached 2026-07, plus `public_api_snapshot` / concept-coverage / factor-seed snapshot drift). This is a
**remediation sprint**: **each finding becomes one task run as a strict TDD cycle** — write the failing test that
exposes the defect, run it → **RED**, apply the minimal fix, run it → **GREEN**, regression-lock, commit. It ships
**no new product surface** — its acceptance is that every High/Med finding is closed with a proof test, the five Low
findings are hardened, the full offline suite is **green with zero date-sensitive failures**, and the `module_boundaries`
public-API snapshot is **deliberately re-pinned only where an intended change requires it**. Reserved migration **0184**
is drawn on **only if a catalog/threshold row is truly needed — the finding set needs none, so 0184 is expected to stay
unused**.

**Mandate / Owns:** targeted, behavior-scoped fixes in `db/enterprise_value.py`, `db/factors/engine.py`,
`db/factors/cross_domain.py`, `db/factors/cross_section.py`, `db/factor_panel.py`, `db/universe.py`, `db/backfill.py`,
`db/metric_engine.py`; new determinism / edge / boundary / scale tests appended to the existing suites
(`db/tests/test_enterprise_value.py`, `test_factor_engine.py`, `test_cross_domain_factors.py`, `test_universe.py`,
`test_factor_panel.py`, `test_backfill.py`, `test_metric_engine.py`); the de-time-bomb of `test_formula_registry_catalog.py`
(injectable as-of clock); and the audited regeneration of `db/tests/data/public_api_snapshot.json` plus the
concept-coverage / factor-seed reference fixtures.

**Must NOT touch:** the public import surface — `db/__init__.py` re-exports, the 56 `*_asof` readers, the `MIGRATIONS`
registry and its pf2-S2 checksums, and the `run_warehouse_quality_checks` set stay unchanged. **No migration is renumbered
or edited**; no landed migration body changes shape (that would break `verify_migration_checksums`). No cross-sectional
operator gains cross-date pooling; every fix keeps `available_at ≤ as_of_ts` gating intact. The `module_boundaries`
contract (no cross-package private imports, DAG import graph) stays green — the public-API snapshot is re-pinned **only**
by the explicit, reviewed S3-12 step, never silently by a test.

**Depends on:** PF4-S1 (signal-eval surface) and PF4-S2 (panel gating + factor observability) — sequential, the last of
the "close pf3 + harden" wave. S1/S2 exercise the factor/EV/panel paths this sprint hardens; landing S3 after them means
the remediation is regression-locked against a suite that already stresses those surfaces. The suite re-green (S3-12) is
a hard precondition for every downstream pf4 sprint: a green, time-bomb-free baseline is what PF4-S4…S11 branch from.

---

## Baseline / where the findings are (measured 2026-07-06 against `atx-impl/db`)

Every reference below is a real file/line/function in the integration tree; each becomes exactly one task.

**HIGH — 1.**
1. **EV period selection hides rows across the filing-lag boundary.** `load_enterprise_value_inputs`
   (`db/enterprise_value.py` **~449–507**) builds a `matched` CTE that joins `market_caps` to `complete_fundamentals`
   on `f.period_end <= mc.trade_date` and ranks `row_number() OVER (PARTITION BY market_cap_source, security_id,
   trade_date ORDER BY f.period_end DESC, f.fundamental_available_at DESC, …)`, keeping `ev_period_rn = 1` (line 507).
   There is **no `f.fundamental_available_at <= mc.trade_date` filter before the ranking**. During a filing-lag window
   the latest `period_end <= trade_date` is often **not yet available** at `trade_date`, so rn=1 selects that
   not-yet-visible period; its `available_at = max(...)` (line 294–300) then exceeds `trade_date`, the row is filtered
   out at as-of read time, **and the older, actually-available period was already dropped by the ranking** → a
   daily-EV coverage hole exactly at the filing boundary. The pure-transform dedup
   (`_select_latest_enterprise_value_inputs` **~200–218**) has the identical latest-`period_end`-wins defect as a second
   line of defense.

**MEDIUM — 5.**
2. **Nondeterministic `manifest_id`.** `compute_factor_rows` (`db/factors/engine.py`) computes
   `manifest_id = _hash_id("factor_build_manifest", tuple(targets), run_id, len(input_values), len(frame))` at
   **line 290**, where `targets = set(target_factor_ids)` (**line 213**). `tuple(set(...))` iteration order is
   randomized by `PYTHONHASHSEED`, so two builds with identical inputs produce **different `manifest_id`s** — even
   though the very next line (**292**) already uses `sorted(targets)` for `factor_ids`, proving the intent.
3. **Pivot revision ambiguity.** In the same function, `value_wide = needed.pivot_table(..., aggfunc="last")`
   (**~235–241**) selects an **arbitrary** revision per PIT key (pandas "last" = last in current row order), while
   availability is `needed.groupby([...])["available_at"].max()` (**~246–248**) over **all** revisions. So `value` and
   `available_at` for one output row can come from **different revisions**, and the chosen value is input-order
   dependent — a determinism + provenance-integrity hole.
4. **Universe rolling window truncation.** `_daily_decisions` (`db/universe.py` **~271–369**) computes `history_days`
   (`count(*) OVER (… ROWS BETWEEN {lookback_preceding} PRECEDING AND CURRENT ROW)`, **~309–313**) and
   `avg_dollar_volume` (**~314–318**) over the `base` CTE, whose bars are already filtered to `b.trade_date >=
   start_date` / `<= end_date` (**~293–298**). The trailing window therefore only sees bars **inside** `[start, end]`,
   so at the window's left edge `history_days` is **understated** and `avg_dollar_volume` is computed over too few bars
   → wrong `min_history_days` / `min_dollar_volume` exclusions, and **membership depends on the build window size**
   (a windowed backfill and a full rebuild disagree at the window start).
5. **Missing latest-revision dedup before cross-domain ranking.** `_compute_source_factor_rows`
   (`db/factors/cross_domain.py` **~727–766**) and `compute_price_liquidity_factor_rows` (**~815–853**) feed `subset` /
   `temp` straight into `cs_rank(..., partition_columns=("factor_id", "as_of_date"))` after `_normalize_source_metrics`
   / `_normalize_price_metrics` (**~628–642**, **~682–705**), which gate `available_at <= as_of_date` but **do not
   reduce to one latest-visible revision per `(security_id, as_of_date)`**. Multiple visible revisions of the same
   security-day therefore **both enter the cross-section and double-count**, distorting the percentile ranks — the exact
   collapse `fundamental_families.py` **349–353** already prevents with a
   `sort_values([...,"available_at",...]).groupby(...).tail(1)` reduction.
6. **Per-row / O(N²) hot paths.** Three provenance-heavy loops: (a) EV row assembly `for _, row in
   selected.iterrows(): … json_dumps(...)` (`db/enterprise_value.py` **~287–344**); (b) growth base-pairing — nested
   `for key, group in history.groupby(...)` → `for _, current in group.iterrows()` → `for spec in group_specs: base =
   _base_row(group, current, spec)` (`db/metric_engine.py` **~596–643**), where each `_base_row` rescans the group,
   giving **O(N²)** per security/metric; (c) cross-domain per-row `json_dumps([lineage_record])` inside the
   `ranked.itertuples()` loops (`db/factors/cross_domain.py` **~767–807** and **~854–889**).

**LOW — 5.**
7. **Panel dedup tiebreak is non-deterministic.** `read_factor_panel_long` (`db/factor_panel.py` **~221–223**)
   `panel.sort_values(["as_of_date","security_id","factor_id","available_at","source_loaded_at"])` (default
   `kind="quicksort"`, **unstable**) then `drop_duplicates([...], keep="last")`. When two rows tie on `available_at`
   **and** `source_loaded_at`, the survivor is input-order dependent → non-reproducible panels.
8. **Panel read/describe/export open the 14 GB DB writable.** `read_factor_panel` (**~681**), `describe_factor_panel`
   (**~738**), and `export_factor_panel` (**~750**) each `connect(db_path, read_only=False)` on a pure read path — a
   writer-lock hazard for concurrent reads (also called out for PF4-S10).
9. **`zscore` Inf leak + `neutralize` absent from the safety map.** `zscore` (`db/factors/cross_section.py` **~73–92**)
   guards `std == 0`/NaN but a huge-magnitude cross-section can still emit `±Inf`; and `_operator_by_name`
   (**~173–182**) / `pit_safety_report` (**~185–230**) map only `{rank, zscore, winsorize}` — `neutralize` (**~141–170**),
   a registered PIT-safe operator, **cannot be checked** by the leakage report.
10. **EV aborts the whole build on one bad component.** `_assert_non_negative_component`
    (`db/enterprise_value.py` **~221–225**) **raises** on any `None`/negative component, so a single dirty
    `total_debt`/`cash` row aborts the entire EV refresh instead of skipping-and-flagging that one security-day.
11. **`full_rebuild` delete scope is unverified per partition.** `_partition_params` (`db/backfill.py` **~665–687**)
    sets `full_rebuild=True` **alongside** `start_date`/`end_date`/`window_lo`/`window_hi` for each partition. Dataset
    delete-scopes (e.g. `_delete_enterprise_value_scope`, `db/enterprise_value.py` **~516–539**) must honor the window
    predicates so a windowed `full_rebuild` deletes **only** rows in `[window_lo, window_hi)` — but there is no test
    proving a per-partition `full_rebuild` leaves out-of-window rows intact, and `orchestrator._params_for_step`
    (**~1801–1823**) pops the window keys on `full_rebuild`, so the backfill path and the orchestrator path must be
    proven to agree.

**SUITE — ~8 offline failures (date-time-bombs + snapshot drift).**
12. **`test_formula_registry_catalog.py` time-bombs on the real clock.** `test_every_committed_formula_is_queryable_as_of_today`
    (**286–299**) and `test_altman_z_double_prime_definition_and_citation_are_queryable` (**301–313**) call
    `formula_registry_asof(dt.date.today(), …)` and assert `len(result) == seeded_count`; any committed
    `formula_registry` row whose `valid_to` has passed relative to `2026-07` drops out → RED. **Snapshot drift:**
    `test_module_boundaries.py::test_public_api_snapshot_matches_pinned_fixture` (**23–26**) compares
    `db/tests/data/public_api_snapshot.json` (whose keys drive the checked module set — note it pins
    **`db.enterprise_value`** at **598–633** in addition to the four decomposed packages), plus
    `test_concept_coverage.py`, `test_fundamental_concept_dictionary.py`, and
    `test_factor_engine.py::test_factor_definition_migration_seeds_catalog_rows` (**90–132**, asserts `count ==
    len(legacy_factor_definitions())`) each fail on accrued branch drift in their reference fixtures.

**Already good — do not regress:**
- **No cross-date leakage.** Cross-sectional operators partition on `(factor_id, as_of_date)`
  (`cross_section.py` **20–30**); `pit_safety_report` (**185–230**) and `_filter_decision_time`
  (`cross_domain.py` **708–724**) enforce `available_at ≤ as_of_ts`. Every fix moves logic **before** ranking or
  tightens a filter — none widens the cross-section across dates.
- **`available_at = max(input.available_at)` propagation** (EV **294–300**, engine **246–248**,
  fundamental_families **363**). Fixes preserve this max-availability rule.
- **pf2-S2 migration governance + the `MIGRATIONS` checksums.** Untouched — no migration body moves.
- **The `module_boundaries` lint (PF3-S3).** Its DAG + no-cross-package-private-import rules stay green; only the
  public-API **snapshot data** is re-pinned, and only under S3-12.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal / no-lookahead, **(D)** determinism + provenance, **(G)** quality-gated, and **(I)** panel
PIT-safety are the load-bearing ones for this sprint; the remediation is defined as **behavior correction under
regression lock**.

- **(A)/(I)** S3-1, S3-4, S3-5 all sharpen availability/window gating **before** selection or ranking — the corrected
  cross-section never includes a not-yet-visible period, a truncated trailing window, or a duplicate revision.
- **(D)** S3-2, S3-3, S3-7 make outputs a pure function of inputs (sorted set → stable `manifest_id`; one revision →
  one value+availability; stable sort + `run_id` tiebreak → deterministic survivor). Each is proven by a
  **row-order-shuffle** property test: shuffle the input rows, recompute, assert byte-identical output.
- **(C)** Every new test runs offline over in-memory / template-copy DuckDB or pure pandas fixtures. The de-time-bomb
  (S3-12) removes the last real-clock dependency by injecting an **as-of clock** instead of `dt.date.today()`.
- **(B)/(E)** Migration **0184** stays unused (no schema/catalog/threshold row is required by any finding); the schema
  contract and `detect_schema_drift` return identical results pre/post. If — and only if — a fix must add a
  module-level symbol to a snapshot-tracked module, the public-API snapshot is re-pinned in S3-12 with a recorded reason.

---

## Tasks

Each task is a **TDD cycle**: (1) append the failing test to the named suite; (2) run it and **capture the RED**
signature named under *Red*; (3) apply the *Fix* (minimal, no refactor beyond the finding); (4) re-run → **GREEN**;
(5) run the *Regression* slice; (6) commit the test **and** fix together (never `git add -A`; stage explicit paths;
trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`).

### S3-1 — HIGH: EV falls back to the latest *available* period across the filing boundary

**Root cause:** `load_enterprise_value_inputs` ranks `complete_fundamentals` by `period_end DESC` with no
`fundamental_available_at <= trade_date` filter before `ev_period_rn = 1` (`enterprise_value.py` **~496–507**), so a
not-yet-filed latest period is chosen and then hidden at read time while the older available period is dropped.

**Red — write first in `db/tests/test_enterprise_value.py`:** `test_ev_falls_back_to_latest_available_period_across_filing_boundary`.
Fixture: one `security_id` with **two** fundamental periods — an older period P1 (`period_end` = Q-2, `available_at`
well before `trade_date`) and a newer period P2 (`period_end` = Q-1 `<= trade_date` but `available_at` **after**
`trade_date`, i.e. still in filing lag) — plus a daily `market_cap` row at `trade_date`. Assert the EV row at
`trade_date` is present, `period_end == P1`, and `available_at <= trade_date` (continuous across the boundary). Run →
**RED**: today the row selects P2 and is dropped, so the assertion sees an empty/hole result.

**Fix:** in the `matched` CTE (**~496–504**) add `AND f.fundamental_available_at <= mc.trade_date` to the
`JOIN complete_fundamentals f` predicate (alongside `f.period_end <= mc.trade_date`), so ranking sees only
as-of-visible periods and `ev_period_rn = 1` returns the latest **available** one. Mirror the guard in the pure path
`_select_latest_enterprise_value_inputs` (**~200–218**): filter `out[out["fundamental_available_at"] <=
out["trade_date"]]` before the `drop_duplicates(..., keep="first")`. SQL-only + a pandas-filter — no module-level symbol
added, so the `db.enterprise_value` snapshot is unaffected.

**Accept:** the new test GREEN; `test_enterprise_value.py` GREEN; a second assertion that at a later `trade_date`
where P2 **is** available, EV switches to P2 (fallback is as-of, not permanent).

### S3-2 — MED: deterministic `manifest_id`

**Root cause:** `manifest_id = _hash_id(..., tuple(targets), ...)` with `targets = set(...)` (`engine.py` **213**, **290**).

**Red — `test_factor_engine.py`:** `test_factor_build_manifest_id_is_hashseed_independent`. Build the same
`(input_values, rows, target_factor_ids)` twice with a **multi-target** set (≥3 factor ids) and assert the two
`manifest.iloc[0]["manifest_id"]` are equal; a second variant permutes the `target_factor_ids` iterable order and
asserts the id is unchanged. Run → **RED** (set-ordering makes them differ under a randomized seed / permutation).

**Fix:** line 290 → `_hash_id("factor_build_manifest", tuple(sorted(targets)), run_id, len(input_values), len(frame))`
(consistent with the already-sorted `factor_ids` on line 292).

**Accept:** new test GREEN; `test_compute_factor_rows_uses_dependency_order_and_max_input_available_at` (**155–192**)
still GREEN.

### S3-3 — MED: one latest-visible revision per PIT key before pivot (value + availability from the same row)

**Root cause:** `pivot_table(aggfunc="last")` (**~235–241**) picks an arbitrary revision while `available_at` is `max()`
over all revisions (**~246–248**) → value/availability can diverge and are order-dependent.

**Red — `test_factor_engine.py`:** `test_compute_factor_rows_selects_one_latest_revision_per_key`. Input frame with
**two revisions** of the same `(factor_id, security_id, as_of_date)` — a stale revision (`available_at` earlier, value
V_old) and a latest revision (`available_at` later, value V_new) — for each dependency; then a shuffled copy. Assert
the derived factor's `value` is computed from **V_new only** and its `available_at` equals the latest revision's, and
that the shuffled input yields byte-identical output. Run → **RED** (today "last" can pick V_old while availability is
the later timestamp).

**Fix:** before the pivot (after the `needed = needed.merge(complete_keys, …)` at **234**), reduce `needed` to one row
per `(factor_id, security_id, symbol, as_of_date)` via
`needed.sort_values([...,"available_at", <deterministic tiebreak>], kind="mergesort").groupby([...], as_index=False).tail(1)`
(the `fundamental_families.py` **349–353** pattern), then pivot on the deduped frame and derive both `value` and
`availability` from it. Keep `_normalize_factor_values` and the topological loop otherwise untouched.

**Accept:** new test GREEN; existing engine tests GREEN.

### S3-4 — MED: universe rolling window reads a lookback buffer, emits only in-window decisions

**Root cause:** `history_days` / `avg_dollar_volume` are windowed over a `base` CTE already clipped to
`[start_date, end_date]` (`universe.py` **~293–318**), truncating the trailing window at the left edge.

**Red — `test_universe.py`:** `test_windowed_and_full_build_agree_at_window_start`. Seed `equity_daily_bars` with a
long continuous history for one security. Build membership once over the **full** range and once over a **narrow
window** whose `start_date` sits deep inside the history. Assert the membership decision (in/out, `history_days`,
`avg_dollar_volume`) for the first in-window `as_of_date` is **identical** between the two builds. Run → **RED** (the
windowed build understates `history_days`, flipping the exclusion at the window start).

**Fix:** in `_daily_decisions`, decouple the **read** window from the **emit** window: read bars from
`start_date - (lookback_days) ` (a lookback buffer) by lowering the `b.trade_date >= ?` bound used for the `base` CTE,
compute the `ROWS BETWEEN {lookback_preceding} PRECEDING` windows over the buffered bars, then filter the final SELECT
to `as_of_date >= start_date` so only in-window decisions are emitted. Leave `end_date`, the `listing` join, and the
`universe_memberships` join unchanged.

**Accept:** new test GREEN; `test_universe.py` GREEN (membership counts unchanged for full builds; only window-start
correctness added).

### S3-5 — MED: latest-visible-revision dedup before cross-domain `cs_rank`

**Root cause:** `_compute_source_factor_rows` / `compute_price_liquidity_factor_rows` rank `subset`/`temp` without
reducing to one revision per `(security_id, as_of_date)` (`cross_domain.py` **~727–766**, **~815–853**), so duplicate
revisions double-count in the cross-section.

**Red — `test_cross_domain_factors.py`:** `test_cross_domain_rank_dedups_to_latest_visible_revision`. Build a source
frame with, for one `as_of_date`, several distinct securities plus **two visible revisions of one** of them (same
`security_id`, different `available_at`, different `raw_value`). Assert (a) the output has exactly one row per
`(factor_id, security_id, as_of_date)` — the latest revision — and (b) the percent-rank denominator equals the number
of **distinct** securities (the duplicate does not inflate the cross-section). Run → **RED** (both revisions rank today,
shifting percentiles).

**Fix:** insert a reduction step immediately after `_normalize_source_metrics` / `_normalize_price_metrics` (or at the
top of each spec loop, before `cs_rank`) that keeps one latest-visible row per `(security_id, as_of_date)` using
`sort_values([...,"available_at","source_row_id"], kind="mergesort").groupby(["security_id","as_of_date"]).tail(1)`
(the `fundamental_families.py` **349–353** idiom, with `source_row_id`/`metric_id` as the deterministic tiebreak).
Reduce **before** `native_percent_rank` too, so the native rank sees the same deduped population.

**Accept:** new test GREEN; `test_cross_domain_factors.py` GREEN (single-revision fixtures unchanged).

### S3-6 — MED: vectorize the three provenance hot paths (row-count, not wall-clock, assertions)

**Root cause:** per-row `iterrows()`+`json_dumps` in EV assembly (**~287–344**), O(N²) growth base-pairing
(`metric_engine.py` **~596–643**), and per-row `json_dumps` in cross-domain lineage (**~767–807**, **~854–889**).

**Red — three tests (one per path), each asserting complexity, not clock:**
- `test_enterprise_value.py::test_ev_row_assembly_is_vectorized_and_matches_reference`: a scale fixture (e.g. 2,000
  security-days) where the vectorized output must equal a small-fixture reference **and** a call-counter monkeypatched
  onto `json_dumps` (or `pd.DataFrame.iterrows`) proves the per-row Python loop is gone (≤ O(1) `iterrows`, lineage
  built in a single batched pass).
- `test_metric_engine.py::test_growth_base_pairing_is_linear`: a single security with **M periods** (e.g. 600);
  instrument `_base_row` (or its replacement) with a counter and assert comparisons scale **O(M)** (≤ c·M), not
  ~M²/2 — while the result equals the current reference on a small fixture.
- `test_cross_domain_factors.py::test_lineage_json_is_batched`: a scale fixture asserting the lineage-JSON serialization
  count is one batched pass over rows, and output equals the reference.

Run → **RED** (today: counters show O(N) `json_dumps` calls in a Python loop and O(N²) base scans).

**Fix, minimal + equivalence-preserving:** (a) EV — build the records DataFrame via vectorized column assignment and a
single batched lineage-JSON pass (replace `for _, row in selected.iterrows()`). (b) growth — pair each `current` period
to its base via a single per-group **merge/`searchsorted`** on the sorted `period_end_ts` offset instead of rescanning
the group in `_base_row`. (c) cross-domain — assemble the `lineage_record` list and `json_dumps` in one vectorized pass
after `cs_rank`, not inside `itertuples`. Each fix must reproduce the existing outputs **byte-for-byte** on the current
fixtures (assert equality against the pre-fix reference).

**Accept:** three new tests GREEN; `test_enterprise_value.py`, `test_metric_engine.py`, `test_cross_domain_factors.py`
all GREEN with unchanged values.

### S3-7 — LOW: deterministic panel dedup (stable sort + `run_id` tiebreak)

**Root cause:** unstable `sort_values` + `drop_duplicates(keep="last")` in `read_factor_panel_long`
(`factor_panel.py` **~221–223**) → order-dependent survivor on `(available_at, source_loaded_at)` ties.

**Red — `test_factor_panel.py`:** `test_panel_dedup_is_deterministic_on_availability_ties`. Two panel rows for one
`(security_id, as_of_date, factor_id)` tying on `available_at` **and** `source_loaded_at` but with different `run_id`
and `value`; feed the frame and a shuffled copy; assert both yield the **same** surviving row. Run → **RED**.

**Fix:** add `run_id` as the final sort key and pass `kind="mergesort"`:
`sort_values(["as_of_date","security_id","factor_id","available_at","source_loaded_at","run_id"], kind="mergesort")`
then `drop_duplicates([...], keep="last")`.

**Accept:** new test GREEN; `test_factor_panel.py` GREEN.

### S3-8 — LOW: panel read/describe/export open read-only

**Root cause:** `read_factor_panel` (**~681**), `describe_factor_panel` (**~738**), `export_factor_panel` (**~750**)
`connect(db_path, read_only=False)` on read paths.

**Red — `test_factor_panel.py`:** `test_panel_read_paths_open_read_only`. Monkeypatch/wrap `db.factor_panel.connect`
to capture the `read_only` kwarg, call each of the three functions against a fixture DB path (no `store=` passed), and
assert every capture is `read_only=True`. Run → **RED** (captures `False`).

**Fix:** change the three `connect(db_path, read_only=False)` calls to `read_only=True`. (Export first opens read-only
to run `assert_factor_panel_export_ready`; the `LakehouseExporter` write path is separate and unchanged.) Note this is
the S3 down-payment on the PF4-S10 served-read-tier hazard.

**Accept:** new test GREEN; `test_factor_panel.py` GREEN.

### S3-9 — LOW: `zscore` Inf→NaN guard + `neutralize` in the PIT-safety operator map

**Root cause:** `zscore` (`cross_section.py` **73–92**) can emit `±Inf`; `_operator_by_name` (**173–182**) omits
`neutralize`, so `pit_safety_report` (**185–230**) cannot validate it.

**Red — `test_factor_engine.py`:** two assertions in
`test_zscore_guards_inf_and_pit_safety_covers_neutralize`. (a) A cross-section whose standardization would overflow to
`Inf` → assert the `zscore` output has **no `Inf`** (non-finite → `NA`). (b) `pit_safety_report(frame,
transformed_frame=neutralize(frame, by=[...]), operator="neutralize")` returns `status="passed"` on a leakage-free
fixture instead of raising `CrossSectionOperatorError("Unsupported PIT-safety operator: neutralize")`. Run → **RED**
on both.

**Fix:** in `zscore._z`, after computing `(values - mean) / std`, replace non-finite values with `pd.NA`
(`.where(np.isfinite(...))`). Add `"neutralize": neutralize` to the `_operator_by_name` map (**174–178**). `neutralize`
takes a required `by=`; have `pit_safety_report` pass `by` through (or accept an `operator_kwargs` mapping) so the
recompute matches.

**Accept:** both assertions GREEN; `test_factor_operator_metadata_migration_seeds_pit_safe_operators` (**272–289**) and
`test_neutralize_residualizes_within_asof_sector_groups` (**293**) GREEN.

### S3-10 — LOW: EV skips-and-flags a bad component instead of aborting the build

**Root cause:** `_assert_non_negative_component` (`enterprise_value.py` **221–225**) **raises** on `None`/negative,
aborting the whole refresh for one dirty security-day.

**Red — `test_enterprise_value.py`:** `test_ev_skips_and_flags_bad_component_without_aborting`. Inputs with two
securities where one has a negative `cash_and_equivalents`; assert (a) the clean security's EV row is produced, (b) the
dirty one is **absent** (skipped), and (c) it is recorded/flagged (returned in a skip list or counted), not raised.
Run → **RED** (a `ValueError` aborts the batch).

**Fix:** replace the raising helper's use in the assembly loop with a skip-and-flag: coerce the component, and if
`None`/negative, record the `(security_id, trade_date, component)` to a skipped-rows collector and `continue` past that
row rather than raising. Keep the arithmetic identical for clean rows. **Public-API note:** if this introduces a new
module-level symbol (e.g. a `_skip_reason`/collector helper) to `db.enterprise_value`, that module is snapshot-tracked
(`public_api_snapshot.json` **598–633**) — re-pin it under S3-12 with the reason; prefer changing behavior in place to
avoid a surface delta.

**Accept:** new test GREEN; `test_enterprise_value.py` GREEN.

### S3-11 — LOW: prove `full_rebuild` deletes are window-scoped per partition

**Root cause:** `_partition_params` (`backfill.py` **665–687**) sets `full_rebuild=True` with per-partition
`start_date`/`end_date`/`window_lo`/`window_hi`; nothing proves a dataset's delete honors the window under
`full_rebuild`, and `orchestrator._params_for_step` (**1801–1823**) pops window keys on `full_rebuild`.

**Red — `test_backfill.py`:** `test_full_rebuild_partition_deletes_only_within_window`. Seed a windowed dataset
(e.g. `enterprise_value`) with rows in **two** partitions; run a `full_rebuild` backfill of **one** partition; assert
the other partition's rows survive and only the target window's rows were replaced. Run → **RED** if the delete scope
ignores the window (or documents the current behavior if it already scopes — in which case the test is a **regression
lock**, and the task confirms + records that the `backfill._partition_params` window keys and
`_delete_enterprise_value_scope` **516–539** predicates agree).

**Fix (minimal):** ensure the delete-scope predicates for windowed datasets consume `window_lo`/`window_hi`
(or `start_date`/`end_date`) even when `full_rebuild=True`; if `_params_for_step` drops window keys on `full_rebuild`,
scope the backfill-driven full rebuild by passing the window explicitly so per-partition rebuilds never table-wipe.
Keep a non-windowed `full_rebuild` (orchestrator run) behaving as before.

**Accept:** new test GREEN; `test_backfill.py` and `test_orchestrator.py` full-rebuild tests
(`test_orchestrator_full_rebuild_runs_even_when_watermarks_match`, **492**) GREEN.

### S3-12 — Suite re-green + de-time-bomb (injectable as-of clock; audited fixture re-pin)

**Root cause:** ~8 offline failures — real-clock `formula_registry` time-bombs and accrued snapshot/fixture drift.

**Red:** run `python -m pytest atx-impl\db\tests -q` from `atx-impl/` and capture the failing set (expect
`test_formula_registry_catalog.py::test_every_committed_formula_is_queryable_as_of_today`,
`::test_altman_z_double_prime_definition_and_citation_are_queryable`,
`test_module_boundaries.py::test_public_api_snapshot_matches_pinned_fixture`, `test_concept_coverage.py`,
`test_fundamental_concept_dictionary.py`, and
`test_factor_engine.py::test_factor_definition_migration_seeds_catalog_rows`).

**Fix:**
- **De-time-bomb the clock.** Replace `dt.date.today()` in `test_formula_registry_catalog.py` (**293**, **307**) with
  an **injectable as-of reference date** — a module-level `AS_OF = dt.date(2026, 7, 6)` constant or a
  `frozen_as_of`/monkeypatched clock fixture — so the "every committed formula is queryable" assertion evaluates at a
  **fixed** as-of and can never expire on wall-clock passage. Keep the assertion semantics (all rows valid as of the
  frozen date are returned); do not touch the seed CSV or the reader.
- **Audit-then-regenerate the snapshots.** For `public_api_snapshot.json`, `concept_map`/concept-coverage fixtures, and
  the factor-seed count fixture: **first audit** the drift (diff current surface vs pinned; confirm each added/removed
  symbol or count delta is an **intended** downstream landing, not an accidental leak of a private helper), then
  regenerate the fixture from the authoritative source (`public_api_snapshot()` for the JSON; the seed loaders for
  concept/factor counts). Record the audited deltas (which module, which symbols, why) in the commit body. Fold in any
  intended surface delta this sprint itself introduced (only S3-10 could add a `db.enterprise_value` symbol; prefer
  none). Re-pin is an **explicit, reviewed** step — the test must still fail if a future drift is unaudited.

**Accept:** the six named failures GREEN; the snapshot/fixtures re-pinned with a recorded, audited rationale; no test
now depends on the real clock.

### S3-13 — Closeout: full-suite gate + parity ledger

**Fix:** run the **whole** offline suite from `atx-impl/`:
`python -m pytest atx-impl\db\tests -q` → **green, 0 failures, 0 date-sensitive skips**. Then append a
`WAREHOUSE_PARITY_TRANCHES.md` row (start/end SHA, modules touched, the per-finding proof-test names, the full-suite
command + pass count, "no schema change — migration 0184 unused; public-API snapshot re-pinned deliberately for
`<modules>` — reason", next → PF4-S4) and update `PARITY_GAP.md` (S1–S10 hardening milestone: High/Med closed with
proof tests, Low hardened, suite de-time-bombed; parity **content** unchanged — a code-hardening entry, not a new
domain). No `git add -A`.

**Accept:** full suite GREEN; ledger row + `PARITY_GAP.md` note landed; `git diff --check` clean.

---

## Sequencing & expected compounding

**S3-1 → S3-2 → S3-3 → S3-4 → S3-5 → S3-6 → S3-7 → S3-8 → S3-9 → S3-10 → S3-11 → S3-12 → S3-13.** Close the High
coverage hole first (it is the one correctness defect that silently drops product rows), then the five Medium
determinism/correctness findings (S3-2…S3-6 — the two determinism fixes S3-2/S3-3 and the two gating fixes S3-4/S3-5
each land with a shuffle/property test that hardens the path S3-6 then vectorizes), then the five Low hardening
findings (S3-7…S3-11), then re-green + de-time-bomb the suite (S3-12 — it must run **after** the code fixes so their
new tests are part of the green baseline and any intended `db.enterprise_value` surface delta from S3-10 is folded into
the single audited re-pin), and finally the full-suite gate + ledger (S3-13). Compounding: after S3-3/S3-5 the factor
engine and cross-domain paths are revision-deterministic, so PF4-S7's release engine content-addresses a **stable**
panel; after S3-1/S3-4 the panel is coverage-complete across filing boundaries and window-invariant, so PF4-S5's
multi-universe and PF4-S6's dense backfill build on membership that doesn't depend on build-window size; and after
S3-12 every downstream pf4 sprint branches from a **green, time-bomb-free** suite.

---

## Risks / guardrails

- **Equivalence is the contract for S3-2/S3-3/S3-6/S3-7.** These fixes must not change any correct value — only make it
  deterministic/faster. Each carries a **shuffle-invariance** or **reference-equality** assertion so a value change is
  caught immediately. Move code, do not reflow logic.
- **The public-API snapshot is a tripwire, not noise.** `public_api_snapshot.json` pins `db.enterprise_value` as well as
  the four decomposed packages. Only S3-10 can plausibly add a symbol there; keep the EV skip-and-flag change in-place
  to avoid a surface delta, and if a symbol is unavoidable, re-pin **only** in S3-12 with the reason recorded — never
  let a test silently regenerate it.
- **`available_at` gating must not regress.** S3-1's new predicate, S3-4's read-buffer, and S3-5's dedup each touch the
  as-of frontier; every one keeps `available_at ≤ trade_date`/`as_of_ts` and is proven by an at-boundary test. A fix
  that let a future-dated row into the cross-section would fail `pit_safety_report` — keep that check green.
- **Migration 0184 stays unused.** No finding needs a table/catalog/threshold row; if S3-11's window-scope fix tempts a
  schema tweak, prefer a code-only scope predicate. Any use of 0184 must be additive, idempotent, catalogued in the
  same migration, and backed up before a live apply — but the expectation is **zero migrations this sprint**.
- **Run pytest from `atx-impl/`, never from `db/`** — `db/calendar.py` shadows stdlib `calendar` and breaks collection.
- **De-time-bomb without weakening coverage.** Freezing the `formula_registry` clock must keep the assertion meaningful
  (all rows valid **as of the frozen date** are returned) — freeze the reference, don't delete the check.

---

## Bench / acceptance

- **Every High/Med finding closed with a proof test:** S3-1 filing-boundary EV continuity; S3-2 hashseed-independent
  `manifest_id`; S3-3 latest-revision pivot (value+availability from one row, shuffle-invariant); S3-4 windowed==full
  membership at window start; S3-5 no double-count in the cross-domain cross-section; S3-6 three O(N)/batched-lineage
  complexity tests with reference-equality.
- **Every Low finding hardened with a proof test:** S3-7 deterministic panel dedup; S3-8 read-only panel connections;
  S3-9 `zscore` no-Inf + `neutralize` in the safety map; S3-10 EV skip-and-flag; S3-11 window-scoped `full_rebuild`
  deletes.
- **Suite:** `python -m pytest atx-impl\db\tests -q` (from `atx-impl/`) **green with 0 date-sensitive failures**; the
  six named failures fixed; `formula_registry` fixtures evaluate at an injected as-of clock.
- **Public API:** `test_module_boundaries.py` green; `public_api_snapshot.json` **byte-identical to the deliberately
  re-pinned surface**, with the audited deltas recorded in the S3-12 commit body; no unaudited symbol drift.
- **No schema change:** migration 0184 unused; `detect_schema_drift` and `verify_migration_checksums` identical pre/post.
- `PARITY_GAP.md` updated (S1–S10 hardening milestone; parity content unchanged) and a `WAREHOUSE_PARITY_TRANCHES.md`
  row appended (start/end SHA, per-finding proof tests, full-suite command + pass count, "no behavior change beyond the
  enumerated fixes; migration 0184 unused", next → PF4-S4).

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; strict TDD — failing test first — +
verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. Commit trailer
EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
