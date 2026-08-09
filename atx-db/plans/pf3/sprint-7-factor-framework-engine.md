# Sprint PF3-S7 — Factor framework + PIT factor engine + cross-sectional operators

**Goal:** promote the warehouse's existing *partial* signal layer — `features.py`'s hand-listed
`FEATURE_DEFINITIONS` (returns / momentum / volatility), `alpha_research.py`'s cross-sectional `zscore_cs`
alphas, and the `feature_*` lake surfaces — into a **governed factor store**: a definition-as-data **factor
catalog** with a dependency DAG and PIT-safe compute, plus first-class cross-sectional **operators** (rank /
zscore / winsorize / neutralize by sector / size / beta). This is the flagship track's foundation. It is
where clause **(I)** — panel PIT-safety for cross-sectional operations — is first *defined and enforced*, not
merely asserted with an `is_point_in_time_safe = True` flag. It **extends existing surfaces**, it is not
greenfield: the factor catalog absorbs `feature_definitions`, the engine extends `feature_dependency_edges` /
`feature_build_manifests`, and the operators generalize the `zscore_cs` / `percent_rank` cross-section
patterns already living in `alpha_research.py`. Reserved migrations **0152–0155**.

**Mandate / Owns:** a NEW `db/factors/` package — `catalog.py` (the definition-as-data factor catalog,
reconciling the legacy features/alphas into `factor_definition` rows), `engine.py` (the dependency-DAG,
topologically ordered, PIT-safe compute engine), and `cross_section.py` (the pure cross-sectional operators:
rank / zscore / winsorize / neutralize) — extending `db/features.py`; plus a NEW
`db/tests/test_factor_engine.py`. Owns migrations **0152–0155** and their `table_catalog` / `field_catalog`
seeds.

**Must NOT touch:** the specific factor **families** — value / quality / profitability / growth / accruals /
distress and the signal-native set are **PF3-S8**'s content rows, seeded via `db/seeds/factor_definitions.csv`;
S7 ships the framework and only a *minimal* reconciliation set (the legacy features) to prove the engine. The
**export panel** (`v_factor_panel`, partitioned Parquet/Arrow) is **PF3-S10**. The **ratio & metric engine v2**
is **PF3-S6** — its metrics are factor *inputs*, read but never re-derived here. And do **not** discard
`db/features.py` / `db/alpha_research.py`: extend and absorb them behind the new catalog + engine, preserving
the `feature_definitions` / `feature_values` surfaces and the catalogued `v_alpha_daily_panel` view so legacy
readers keep resolving while they reconcile through the engine.

**Depends on:** PF3-S6 (the ratio & metric catalog = the fundamental factor inputs), pf2's standardized + PIT
surfaces (`fundamental_ttm_points`, populated valuation multiples, the five bitemporal columns), and pf1
reference classifications (the sector / industry assignment neutralization partitions by). This is the
**first** factor sprint and is **sequential before PF3-S8 / PF3-S9**, which share the `db/factors/` package and
must never run concurrently in the same tree.

---

## Baseline / where the cycles go

A signal layer exists, but it is a demo, not a governed store. Measured 2026-07-04 against `atx-impl/db`.

1. **The feature layer is a small hand-listed set, not a governed catalog + engine.**
   `features.py::FEATURE_DEFINITIONS` is seven price-derived features (`ret_1d`, `mom_5d`, `mom_21d`,
   `mom_63d`, `vol_21d`, `adv_21d`, `dollar_volume`), each a literal dict of
   `{description, expression_sql, lookback_days}`; a companion `FUNDAMENTAL_FEATURE_DEFINITIONS` block reads
   `fundamental_points` / `fundamental_ttm_points`. `alpha_research.py::DEFAULT_ALPHA_SPECS` adds three
   `AlphaExpressionSpec` alphas. There is no factor *family*, no governed `direction` / `neutralization` /
   `lookback` surface beyond the ad-hoc `alpha_expression_catalog` columns, and no engine that computes an
   *arbitrary declared* factor from its *declared inputs* — the SQL is inlined per feature. → **S7-0 / S7-1.**

2. **Cross-sectional operators exist ad hoc and ungoverned.** `alpha_research.py` computes a cross-sectional
   z-score by inlining, per feature,
   `(mom_21d - avg(mom_21d) OVER (PARTITION BY as_of_date)) / nullif(stddev_samp(mom_21d) OVER (PARTITION BY as_of_date), 0)`
   and a `percent_rank() OVER (PARTITION BY alpha_id, as_of_date ORDER BY raw_signal)` rank — correct, but
   bespoke and unshared. Winsorization exists only far away in `short_interest_metrics.py`
   (`days_to_cover_winsorized`, capped at a within-cohort percentile). And `neutralization` is a *string
   column* on the alpha catalog set to `"none"` with no operator behind it. There is no governed, reusable
   `rank` / `zscore` / `winsorize` / `neutralize`. → **S7-2 / S7-3.**

3. **There is no factor dependency DAG — but the seed already exists.** `feature_dependency_edges` is a real
   table (written by `features.py`, e.g. `vol_21d` depends on `ret_1d`), and `feature_build_manifests` records
   each build. Nothing composes these into a *topologically ordered, PIT-safe* compute for a factor that
   depends on a ratio that depends on a standardized fact. The engine must **extend** `feature_dependency_edges`
   into a factor DAG, not reinvent it. → **S7-1.**

4. **PIT-safety of the cross-sectional ops is asserted, never proven.** `alpha_research.py` sets
   `is_point_in_time_safe = True` and an `available_at_policy` string, but nothing *tests* that a rank / zscore
   partitions strictly by `as_of_date` (never pooling across dates), nor that every input row satisfies
   `available_at ≤ as_of`. Cross-sectional leakage — ranking a security's value against a *future* date's
   cross-section, or against an input not yet available — is the signature bug of a factor engine and is
   currently unguarded. → **S7-3.**

**Already good — do not regress:**

- **The `feature_definitions` / `feature_values` surfaces.** The long-format
  `feature_values(security_id, symbol, feature_set, feature_name, as_of_date, value, available_at, run_id,
  source)` store and its `feature_definitions` catalog stay the physical substrate; the factor catalog is a
  governed layer *over* them, not a replacement.
- **`feature_dependency_edges` + `feature_build_manifests`.** The dependency-edge table and the build-manifest
  provenance pattern are exactly the DAG + lineage substrate S7-1 extends.
- **`v_alpha_daily_panel`.** The catalogued daily-panel view and its `percent_rank` / partition-by-`as_of_date`
  z-score cross-section shape are the precedent the operators generalize; legacy alpha readers must keep
  resolving through it.
- **The pure-transform + manifest discipline.** `features.py` / `alpha_research.py` already separate a pure
  `compute_*` (pandas in → long DataFrame out) from the DuckDB write and stamp a build manifest — the clause
  (D) pattern the engine keeps.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal / no-lookahead, **(D)** determinism + provenance, and **(E)** schema-as-contract
apply in full; **(I) panel PIT-safety is DEFINED by this sprint** for cross-sectional operations (S7 delivers
its *operator* gate; PF3-S10 later delivers its *export* gate). **(B)/(F)** govern the migrations.

- **(I)** A cross-sectional operator computes **only within the as-of cross-section**: every window partitions
  by `as_of_date` and never pools rows across dates, and every input row is filtered to `available_at ≤ as_of`
  before the operator runs. A derived factor sets `available_at = max(input.available_at)` per clause (A). A
  **lookahead-detection test** (S7-3) is the gate: planting a future-dated input, or a cross-date pooling bug,
  must turn it red. This is the clause's first implementation; PF3-S10 extends it to the exported panel.
- **(A)** Factor rows carry the five PIT columns (`as_of_date`, `available_at`, `source_loaded_at`, `run_id`,
  `is_latest_revision`); neutralization reads **as-of** sector / size / beta, never a future classification or
  beta.
- **(D)** `db/factors/cross_section.py` operators are **pure** (pandas in → long DataFrame out), unit-tested
  independent of DuckDB; same inputs + params → same rows. The engine records each factor's input lineage into
  its build manifest.
- **(B)/(F)** Migrations **0152–0155** only; never renumber or edit ≤ 0151. Schema/index split per the landed
  precedent:
  - **0152** — `factor_definition` catalog (extending `feature_definitions`) + its catalog seed;
  - **0153** — factor dependency-DAG + engine tables (`factor_dependency_edges`, `factor_build_manifests`);
  - **0154** — cross-section operator metadata (`factor_operator`);
  - **0155** — indexes + the catalogued factor-engine view.

  Each migration seeds `table_catalog` + `field_catalog` in the same version; a timestamped DB+WAL backup
  precedes any live apply.
- **(C)** All tests run offline against in-memory / template-copy DuckDB with fixtures; no vendor network. The
  live proof-slice factor counts are operator-run and recorded in the ledger.

---

## Tasks

### S7-0 — Factor catalog (definition-as-data, extending `feature_definitions`)

**Root cause:** factors "exist" only as hand-inlined dict literals in `features.py` and `AlphaExpressionSpec`
tuples in `alpha_research.py` — there is no governed catalog that declares a factor's family, expression,
inputs, direction, lookback, and neutralization spec as *data* a generic engine can execute. A new factor
today means new inlined SQL, not a new row.

**Fix:** NEW `db/factors/catalog.py` and a `factor_definition` table (migration **0152**) that **extends**
`feature_definitions` rather than forking it:
`(factor_id PK, family, expression, input_ids_json, direction, lookback_days, neutralization_spec_json, unit,
sign, is_point_in_time_safe, declared_in, owner, source)`. `catalog.py` reconciles the existing
`FEATURE_DEFINITIONS` / `FUNDAMENTAL_FEATURE_DEFINITIONS` / `DEFAULT_ALPHA_SPECS` into `factor_definition`
rows (tagging `declared_in` so each factor's origin — feature engine vs alpha engine — stays queryable), so
the legacy features become the catalog's first governed citizens. A `validate_catalog()` pure check asserts
every `input_ids_json` entry resolves to another `factor_definition` or a known ratio/metric id. Catalogued in
the same migration per (E).

**PIT:** (E) no factor row lands without a `factor_definition` + `table_catalog` entry. (D) `validate_catalog`
is a pure, deterministic check. (B) 0152 schema + catalog seed.

**Accept:** every existing feature/alpha resolves to a `factor_definition` row with a non-null family,
direction, and neutralization spec; a factor declared with an undeclared input id is rejected by
`validate_catalog`; the catalog is queryable by family; `declared_in` correctly attributes each row.

### S7-1 — Factor dependency DAG + PIT compute engine (extending `feature_dependency_edges`)

**Root cause:** `feature_dependency_edges` records edges but nothing composes them into an ordered, PIT-safe
compute; a factor that depends on a ratio that depends on a standardized fact cannot be materialized
generically, and each feature's SQL is inlined instead of driven by its declared inputs.

**Fix:** NEW `db/factors/engine.py` that **extends `feature_dependency_edges`** into a factor DAG (adding
factor→factor and factor→ratio/metric edges), topologically sorts it, and computes each factor from its
declared inputs in dependency order. Every derived factor stamps `available_at = max(input.available_at)`
(clause A) and writes a `factor_build_manifest` extending `feature_build_manifests` (input ids, params, row
counts, `run_id`, source). The engine reuses the pure-transform + manifest discipline already in `features.py`
rather than inventing a second write path. Migration **0153** adds the DAG + engine tables.

**PIT:** (A) `available_at = max(input.available_at)`; as-of reads gate on `available_at ≤ as_of`. (D)
deterministic topological order; full input lineage recorded in the manifest. (B) 0153 schema + catalog.

**Accept:** a fixture factor depending on two upstream factors materializes in correct topological order; its
`available_at` equals the max of its inputs'; a cyclic dependency is rejected with a clear error; the existing
`ret_1d` → `vol_21d` edge reconciles through the engine and reproduces the legacy `feature_values` row-for-row.

### S7-2 — Cross-sectional operators (rank / zscore / winsorize)

**Root cause:** the cross-sectional z-score and `percent_rank` are inlined per-feature in `alpha_research.py`,
winsorize lives only in `short_interest_metrics.py`, and none is a governed, reusable, unit-tested operator —
so every new factor family would re-inline (and risk re-breaking) the cross-section logic.

**Fix:** NEW `db/factors/cross_section.py` exposing pure operators — `rank(x)` (cross-sectional percent-rank,
generalizing `percent_rank() OVER (PARTITION BY … as_of_date)`), `zscore(x)` (standardization, generalizing
the inlined `(x - avg(x)) / nullif(stddev_samp(x), 0)` over the as-of partition), and `winsorize(x, limits)`
(symmetric tail-capping at declared cross-section percentiles) — each computing **only within the as-of
cross-section**: partitioned by `as_of_date`, **never** pooled across dates. Each is a pure transform (pandas
in → long DataFrame out), unit-tested independent of DuckDB. Migration **0154** records cross-section
**operator metadata** (`factor_operator`: operator id, kind, params, partition key = `as_of_date`) so a
factor's operator chain is declaration-as-data, not code.

**PIT:** (I) operators partition strictly by `as_of_date`; no cross-date pooling. (D) pure, deterministic,
unit-tested. (B) 0154 operator metadata + catalog.

**Accept:** `rank` / `zscore` / `winsorize` on a two-date fixture produce values identical to computing each
date in isolation (proving no cross-date bleed); winsorize caps exactly at the declared cross-section
percentile; the operators reproduce `alpha_research.py`'s inlined z-scores on the same inputs.

### S7-3 — Neutralization + leakage / lookahead-detection test (the clause (I) gate)

**Root cause:** `neutralization` is a string column set to `"none"` with no operator behind it, and no test
proves the cross-sectional ops are free of cross-date or future-input leakage — the signature failure mode of
a factor engine.

**Fix:** add `neutralize(x, by)` to `db/factors/cross_section.py` — a within-cross-section residualization
(regress the factor on the grouping/covariates, keep the residual) against **as-of** sector (pf1
classifications), size, and beta, each read `available_at ≤ as_of`, never a future beta or classification. Then
add a **lookahead-detection test** to `db/tests/test_factor_engine.py` that plants (a) a future-dated input
row and (b) a cross-date pooling bug and asserts *both* turn the check red — the clause (I) operator gate.
Migration **0155** adds the engine indexes + the catalogued factor-engine view.

**PIT:** (I) neutralization partitions by `as_of_date` and reads only `available_at ≤ as_of` sector / size /
beta; the leakage test is the gate. (A) as-of classifications only, no future beta. (B) 0155 indexes + view
catalogued.

**Accept:** sector / size / beta neutralization residualizes within the as-of cross-section and leaves a
mean-zero-per-group residual; the lookahead test is green on clean inputs and red when a future input or a
cross-date pool is injected; no future beta or classification is ever read.

---

## Sequencing & expected compounding

**S7-0 → S7-1 → S7-2 → S7-3.** S7-0 lays the definition-as-data catalog (everything downstream reads it);
S7-1 makes the catalog *executable* via the dependency DAG + PIT engine; S7-2 adds the governed
cross-sectional operators the factors compose from; S7-3 adds neutralization and the leakage gate that
certifies the whole path PIT-safe. Compounding: the framework + operators are precisely what **PF3-S8**'s
fundamental factor families (value / quality / profitability / growth / accruals / distress) and **PF3-S9**'s
cross-domain factors plug into — each seeds `factor_definition` rows and reuses the operators rather than
re-inlining cross-section logic — and what **PF3-S10** exports as `v_factor_panel`. Once S7's leakage gate is
green, every downstream factor lands on rails that *refuse* a cross-date or future-input leak.

---

## Risks / guardrails

- **Cross-sectional leakage is THE trap.** Ranking / z-scoring / winsorizing must partition strictly by the
  as-of date and consume only inputs with `available_at ≤ as_of`; a single `OVER ()` missing its
  `PARTITION BY as_of_date`, or an unfiltered future row, silently pools the future into today's
  cross-section. S7-3's lookahead-detection test is the mitigation and must be *red-provable* before S8 builds
  on the engine.
- **Neutralization must use as-of inputs.** Regressing out sector / size / beta with a *current*
  (future-relative) classification or beta is lookahead by the back door — read every neutralization partition
  and covariate as-of, never the latest.
- **Do not reinvent — absorb.** The catalog extends `feature_definitions`, the engine extends
  `feature_dependency_edges` / `feature_build_manifests`, the operators generalize the `zscore_cs` /
  `percent_rank` shapes; `features.py` / `alpha_research.py` are kept and reconciled through the engine, not
  discarded, and `v_alpha_daily_panel` keeps resolving.
- **Stay in lane.** No factor *family* content (S8), no export panel (S10), no ratio re-derivation (S6).
  Strictly migrations **0152–0155**; never edit ≤ 0151; schema / index / view split; timestamped DB+WAL backup
  before any live apply.

---

## Bench / acceptance

- The `factor_definition` catalog and the factor dependency DAG exist; every legacy feature/alpha reconciles
  to a governed catalog row and materializes through the engine, reproducing its `feature_values`.
- `rank` / `zscore` / `winsorize` / `neutralize` are PIT-safe: each computes only within the as-of
  cross-section (two-date fixtures match per-date-isolated computation) and reads only `available_at ≤ as_of`
  inputs.
- The **leakage / lookahead-detection test is green** on clean inputs and **red** when a future-dated input or
  a cross-date pool is injected — the clause (I) operator gate.
- `python -m pytest atx-impl\db\tests\test_factor_engine.py -q` green, and full
  `python -m pytest atx-impl\db\tests -q` green in the worktree before commit.
- **Live proof-slice smoke** recorded in the ledger: per-factor row counts materialized through the engine,
  cross-section sizes, and the `run_id`.
- `PARITY_GAP.md` status updated (clause I now defined/enforced for cross-sectional ops); a
  `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke with
  exact counts + run_id, caveats/next → PF3-S8 fundamental factor families).

**Process:** own git worktree off `main` via
`atx-impl/scripts/new_db_worktree.sh new|finish sprint-7-factor-framework-engine`, merged at sprint end;
controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD +
verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. New module
⇒ new `test_*.py`. Commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
