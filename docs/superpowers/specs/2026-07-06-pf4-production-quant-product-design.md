# PF4 — Production Quant-Product & Warehouse Activation (design)

**Created:** 2026-07-06. Successor to **pf3**
([../../../atx-impl/plans/pf3/ROADMAP.md](../../../atx-impl/plans/pf3/ROADMAP.md)) — the Signal-Ready
Fundamentals Factor Warehouse. This document is the PF4 design record; the concrete roadmap and per-sprint
plan files live under `atx-impl/plans/pf4/` (ROADMAP.md, IMPLEMENT_ALL_SPRINTS_GOAL.md, `sprint-N-*.md`),
generated from this spec.

**Assumes pf1 (PF-S1…S8), pf2 (PF2-S1…S10), and pf3 PF3-S1…S10 have landed** (merged to the integration
mainline). Where PF4 references a prior deliverable whose exact landed name differs at implementation time,
the PF4 implementer reconciles to the landed name.

---

## Why PF4 exists — measured state of the warehouse (2026-07-06)

pf3 was scoped as twelve sprints. A ground-truth audit of the repo (git history, the SDD progress ledger,
`WAREHOUSE_PARITY_TRANCHES.md`, migration bodies, and the `db/` tree) shows pf3 is **not complete**, on two
independent axes:

1. **PF3-S11 (signal evaluation) and PF3-S12 (production capstone) were never built.** Migration bodies stop
   at `bodies_0164_0167.py` (S10's range); the reserved ranges `0168–0171` (S11) and `0172–0175` (S12) are
   unclaimed. `db/signal_eval.py`, `db/tests/test_signal_eval.py`, and `db/tests/test_pf3_capstone.py` do not
   exist. There is no factor-panel gate wired into the orchestrator and no factor-domain observability. So the
   warehouse can *produce* a factor panel but cannot yet *tell which factors carry signal*, cannot *halt a bad
   panel build*, and cannot *observe panel freshness/collapse/lineage*.
2. **Every pf3 sprint is `OPERATOR-PENDING`.** The `WAREHOUSE_PARITY_TRANCHES.md` ledger records, for every
   PF3 tranche, that no live shared-DB migration/apply, no historical backfill, and no proof-slice population
   was run. Migrations `0132–0167` were never applied to the live 14 GB `atx_impl.duckdb`; `equity_daily_bars`
   still holds only ~3.18M rows for **2012–2014** while fundamentals are 2017–2026, so the price×fundamental
   overlap — and therefore the factor tables — are effectively empty. The rails and engines are built and
   fixture-proven; the warehouse is **data-empty for factors and un-migrated live**.

A 2026-07-06 production-readiness review of the S1–S10 factor/panel code found the architecture **sound**
(cross-sectional operators partition correctly on `(factor_id, as_of_date)` with no cross-date leakage;
`available_at = max(input.available_at)` is consistently propagated; SQL is uniformly parameterized — no
injection), but with **one High**, **five Medium**, and several **Low** latent defects that must be
remediated before the stack is declared production-grade (enumerated under PF4-S3).

A 2026-07-06 run of the offline suite (`python -m pytest atx-impl\db\tests -q`, from `atx-impl/`) on the
current integration tree returned **~8 failures** — a mix of **date-sensitive time-bombs**
(`test_formula_registry_catalog` fixtures whose `valid_to=2010-01-01` expired once the clock reached 2026-07)
and **snapshot drift** (`test_module_boundaries::public_api_snapshot`, `test_concept_coverage`,
`test_factor_engine::…migration_seeds_catalog_rows`, `test_fundamental_concept_dictionary`) accrued from
downstream branch drift + wall-clock passage since the pf3 merges (which were green at merge time). A
production product requires a green, time-bomb-free suite — PF4-S3 re-greens it and de-time-bombs
date-sensitive fixtures (freeze/parametrize the reference clock) as an explicit deliverable.

**The gap map for a downstream quant product** (from a read-only capability survey): no signal-evaluation
surface; no factor observability; the panel export-contract check is recorded but not an orchestrator halt
gate; no public data-access SDK/client (`db.factor_panel` isn't even exported from `db/__init__.py`); no
versioned/immutable releases (only ephemeral `export_run_id` UUID dirs); **Arrow is claimed but absent** (only
Parquet+ZSTD is written); no docs / data dictionary / runbook / notebooks; a read path that opens the 14 GB DB
`read_only=False`; shallow price history; empty delisting-return observations (survivorship bias unhandled);
and only one governed universe.

**PF4's mission:** take this code-complete-through-S10, fixture-proven, data-empty warehouse and turn it into
a **state-of-the-art, production-ready US-equity fundamental factor _product_ for downstream quant teams** —
by closing pf3's two missing sprints, hardening the S1–S10 code, making the data trustworthy and dense,
packaging a versioned/served/SDK-fronted product, and delivering a reproducible activation runbook.

---

## Scope decisions (user, 2026-07-06)

1. **Live-backfill posture = _Code + gated runbook_.** PF4 builds all code plus a reproducible activation
   runbook; live migration/backfill against the 14 GB DB is executed **only on explicit per-step operator
   go**. No autonomous mutation of production data; no multi-hour vendor/SEC network pulls inside a sprint.
   This preserves the pf1/pf2/pf3 "operator runs the archive" posture. Consequence: "full production read
   backfill complete" is delivered as a **turnkey, verifiable operator milestone** — PF4 makes it push-button
   and evidenced, and the actual live execution is a gated operator run.
2. **Breadth = _Tie-together + productionize_.** Close pf3 (S11 + S12), activate the backfill (code+runbook),
   and build the quant-product layer (SDK, versioned releases, docs, factor observability, serving). ESG /
   licensed vendor estimate feeds (IBES/broker-detail) / international-IFRS-ESEF / supply-chain graph remain
   **parked → pf5**.
3. **Consumer form = _Python SDK client_.** A thin installable `atx-panel` client is the primary interface
   (PIT reads, as-of universe filter, factor metadata, pandas + zero-copy Arrow out, release pinning), over
   the contracted views + versioned Parquet/Arrow releases. Example notebooks ship alongside.

---

## North star (PF4 acceptance)

> A downstream quant team can `pip install atx-panel`, pin an **immutable, semver'd panel release**, and pull
> a **point-in-time, lookahead-tested factor panel** (pandas or zero-copy Arrow) — every factor **scored for
> signal** (rank-IC / IC-decay / quantile spread / turnover / crowding / breadth), **survivorship-safe**
> (delisting returns populated), across **multiple governed universes**, with a **generated data dictionary**
> and **runnable example notebooks** — while the warehouse itself is **orchestrator-gated** (a panel-critical
> check halts a bad build), **observable** (factor freshness SLA, panel-collapse anomaly,
> lineage-completeness), and **reproducibly activatable** from a fresh checkout via a documented runbook. The
> four pf3 surpass axes (full lineage, signal-native factors, PIT-perfect vintages + freshness, open
> cross-vendor reconciliation) are **evidenced** — not asserted — in the parity ledger.

---

## Four tracks

- **Track A — Close pf3 (evaluation + trust).** The signal-evaluation surface (the missing "which factors
  carry alpha" layer) and orchestrator panel-gating + factor observability + a codified maintenance schedule.
- **Track B — Data correctness & density.** Survivorship-safe delisting/corporate-action returns;
  multi-universe management + versioning; a dense price/return backfill **activation harness** (operator-gated
  execution) that makes "backfill complete" turnkey and verifiable.
- **Track C — Quant-product surface.** Immutable versioned releases (Parquet **+ Arrow**); the `atx-panel`
  Python SDK client; a generated data dictionary + docs + example notebooks; and a served read tier + panel
  query performance.
- **Track D — Harden + capstone.** Remediate the pf3 S1–S10 review findings; then a production capstone that
  ties the whole activation together, flips the surpass-ledger with evidence, and closes pf3+pf4 with a
  whole-branch review.

---

## The eleven sprints (reserved migrations start at 0176; pf3 used through 0175)

| Sprint | Track | Theme | Goal metric | Reserved migrations |
|---|---|---|---|---|
| **PF4-S1** | A | **Signal-evaluation surface** (closes PF3-S11) — NEW `db/signal_eval.py`: per-factor rank-IC + IC information-ratio + t-stat + sign-consistency over the horizon ladder {1,5,10,21,63}; IC-decay profile; quantile/decile long-short spread + per-decile monotonicity + hit-rate; factor turnover + rank autocorrelation; factor-to-factor correlation + crowding; per-date breadth; gated **leakage** (t+0 probe) + **coverage** DQC (`severity=critical`). Reads S10 `v_factor_panel` read-only. | every panel factor scored; zero-signal fixture → rank-IC ≈ 0; monotone fixture → monotone deciles + positive spread; leakage DQC RED on a planted leak, coverage DQC RED on a sparse factor; deterministic reproduction | 0176–0179 |
| **PF4-S2** | A | **Panel gating + factor observability** (closes PF3-S12 operational core) — wire S1 leakage/coverage + S10 export-contract checks as `critical` **orchestrator halt gates** on the factor-panel dataset (`panel_quality_gate_halt`, reusing pf2-S10 evaluator/halt semantics); extend `db/observability.py` with factor **freshness SLA**, **panel row-count anomaly** (median/MAD z-score), and **lineage-completeness**; codify `maintenance_schedule` (cadence-as-data per dataset). | orchestrator halts on a planted panel-critical check (`gate=True`); stale/collapsed/lineage-broken panel each flagged and route through the gate; schedule queryable per dataset | 0180–0183 |
| **PF4-S3** | D | **pf3 S1–S10 hardening** — remediate the 2026-07-06 review: **High** EV PIT period-selection coverage gap; **Med** nondeterministic `manifest_id` (set-ordering), pivot revision ambiguity, universe rolling-window truncation, missing latest-revision dedup before cross-domain ranking, O(N²)/per-row perf; **Low** panel-dedup tiebreak, read-only panel reads, EV skip-not-abort, `Inf` guards, backfill `full_rebuild` window-scoping. **Also re-greens the offline suite** (regenerate the `public_api_snapshot`/concept/factor-seed fixtures deliberately after audit) and **de-time-bombs date-sensitive tests** (freeze/parametrize the reference clock so `formula_registry` valid-window fixtures never expire). Each fix is TDD (failing test first) and regression-locked; public API unchanged. | every High/Med finding closed with a proof test (incl. a row-order-shuffle determinism property test and an EV filing-boundary test); **full offline suite green with 0 date-sensitive failures**; `module_boundaries` public-API snapshot deliberately re-pinned | 0184 (if any catalog/threshold rows needed; else none) |
| **PF4-S4** | B | **Survivorship-safe returns** — populate observed DLRET + DLSTCD reconciliation via injectable loaders; delisting-return stitching into forward returns; spinoff/merger return policy; a survivorship-bias DQC (`severity=critical`). Extends `db/delisting.py`; the returns feed S1 forward returns and the panel. | terminal returns present per delisted security-day on the fixture; forward-return series survivorship-safe; DLSTCD recon gate green; leakage/lookahead preserved | 0185–0188 |
| **PF4-S5** | B | **Multi-universe + versioning** — governed PIT universes beyond `us_common_equity_liquid_v1` (e.g. broad / liquid-large / sector-neutral / custom), universe **release versions** + membership **turnover** reporting; universe-as-of applied consistently in panel assembly and the SDK filter. Extends `db/universe.py`. | ≥3 governed universes PIT-queryable; universe versions pinnable; per-universe turnover reported; panel + SDK filter by universe id + version | 0189–0191 |
| **PF4-S6** | B | **Activation harness + dense price backfill runbook** — a resumable **operator harness** (thin `scripts/` driver over the S1 DAG) that plans (dry-run) and, on operator go, executes the historical price backfill widening `equity_daily_bars` (→2004+), records live-count evidence + a verification report, and drives the full migrate→backfill→rebuild→gate sequence. Fixture-proves resumability+idempotency on a bounded slice; **live execution is operator-gated per decision #1**. | harness dry-run plans the archive without touching the live DB; bounded slice proves resumable + idempotent (re-run = no-op); verification report emits per-partition + per-dataset counts | 0192–0194 |
| **PF4-S7** | C | **Panel release engine** — immutable semver'd releases (`YYYY.MM.patch`) with a release manifest + generated changelog + per-file checksums; **Arrow/Feather** output alongside Parquet in `db/lake.py`; pinnable, content-addressed snapshots; retention that **never prunes a pinned release**. NEW `db/panel_release.py`. | a release is immutable + checksummed + pinnable; Arrow + Parquet both emitted and validated; changelog generated from panel/contract diff; re-publishing identical inputs is a no-op | 0195–0197 |
| **PF4-S8** | C | **`atx-panel` Python SDK client** — a thin installable package (own `pyproject`, not internal-module imports): `read_panel(as_of, universe=, factors=, release=)`, factor metadata + data-dictionary access, signal-eval lookups, pandas + zero-copy **Arrow** out, release pinning, and an **isolated read-only** DB/lake connection. NEW top-level `clients/atx-panel/` (or `atx-impl/atx_panel/`). | client installs standalone; a PIT read matches the contracted view read **bit-for-bit** (parity test); reads are read-only-isolated; typed public API with docstrings | 0198 (client-registry/version row only, if any) |
| **PF4-S9** | C | **Data dictionary + docs + notebooks** — a **generated** factor/panel data dictionary from `panel_contract`/`catalog`/`signal_eval` (unit/sign/scale/lineage/IC); a fresh-agent **activation + consumption runbook**; runnable **quickstart notebooks** (load release → PIT cross-section → factor IC → decile backtest). Docs live under `atx-impl/docs/` (created). | the dictionary regenerates deterministically from the contract; the runbook is followed end-to-end offline; notebooks execute against a bounded slice with pinned outputs | (docs; no migration) |
| **PF4-S10** | C | **Served read tier + panel perf** — a read-only served panel tier (isolated connection + Arrow cache) fronting the SDK; concurrency-safe reads (fixes the `read_only=False` panel-read hazard from S3); panel query performance (indexes / materialized cross-sections) within a stated budget. Extends `db/factor_panel.py` + a thin serving module. | concurrent reads are isolated (no writer lock on the read path); a single-date cross-section read meets the perf budget on the slice; served tier returns rows identical to the SDK/view | 0199–0200 |
| **PF4-S11** | D | **Production capstone** — an end-to-end **activation runbook** a fresh agent can follow (recover-from-`.bak` → CHECKPOINT → migrate → operator historical backfill → deterministic rebuild through the gated DAG → freshness/anomaly/lineage sweep → export → release → SDK-verify), honest about offline-deterministic vs operator-run steps; the **surpass-ledger flip** in `db/parity.py` with a `surpass_axis_evidence` table citing a concrete surface/check per axis; a **whole-branch pf4 + pf3-closure review** (strongest reviewer) → `superpowers:finishing-a-development-branch`; a final catalog sweep asserting 0 uncatalogued pf3/pf4 tables. | orchestrated activation reproducible + deterministic on a slice (incremental re-run = byte-identical panel); all 4 surpass axes cited to resolvable evidence; branch review clean; catalog sweep green | 0201–0204 |

---

## Sequencing

1. **Close pf3 + harden — PF4-S1 → PF4-S2 → PF4-S3 (sequential).** Land the signal-evaluation surface, then
   promote its DQC to orchestrator halt gates + add factor observability, then harden the S1–S10 code that
   S1/S2 have now exercised. (S2's gate needs S1's critical checks to exist; S3's remediation benefits from
   S1/S2 stressing the factor/EV/panel paths.)
2. **Data correctness & density — PF4-S4 → PF4-S5 → PF4-S6 (sequential; they share the
   `fundamental_*`/pricing/universe surfaces).** Survivorship-safe returns, then multi-universe, then the
   dense-price activation harness that consumes both.
3. **Product surface — PF4-S7 → PF4-S8 → PF4-S9 → PF4-S10 (sequential; the SDK reads releases, docs document
   the SDK, the served tier fronts it).**
4. **Capstone — PF4-S11 (last).** Ties activation together, flips the ledger with evidence, and closes
   pf3+pf4 with a whole-branch review.

**Minimal end-to-end product slice (if only a subset is possible):** PF4-S1 (eval) + PF4-S2 (gating) + PF4-S6
(activation harness) + PF4-S7 (releases) + PF4-S8 (SDK) + PF4-S11 (capstone). That is the "scored, gated,
activatable, releasable, SDK-consumable panel" spine.

---

## Shared contract (every sprint)

PF4 inherits pf1 clauses **(A)–(D)**, pf2 **(E)–(G)**, and pf3 **(H)–(J)** unchanged, and adds **(K)–(L)**:

- **(A) Bitemporal correctness / no lookahead.** `available_at = max(input.available_at)`; as-of readers gate
  on the valid window **and** `available_at ≤ as_of_ts`.
- **(B) Append-only, catalogued migrations.** Forward-only, idempotent; each sprint uses only its reserved
  range; every new table/view seeds `table_catalog` + `field_catalog` in the same migration; schema split
  from index; back up before any live apply.
- **(C) Offline / no-network tests.** In-memory / template-copy DuckDB, fixtures or injected files only. No
  SEC/FRED/FINRA/OpenFIGI/GLEIF/vendor network in pytest. Live connectors stay behind injectable file
  options; live smoke is operator-run and recorded in the ledger.
- **(D) Determinism + provenance.** `compute_*` transforms are pure (pandas in → long DataFrame out),
  stable-sorted, unit-tested independent of DuckDB. Same inputs + params → same rows.
- **(E) Schema-as-contract.** No table lands without a contract row + `table_catalog` entry; drift check fails
  on divergence or any uncatalogued table.
- **(F) Backup-before-migrate.** Every live apply is preceded by a scripted CHECKPOINT + timestamped backup
  and followed by a verify; WAL-split discipline is the standing invariant.
- **(G) Quality-gated.** A `severity=critical` check is wired into the orchestrator and halts the affected
  run. Each sprint that adds a load-bearing invariant registers it as a gated check.
- **(H) Backfill-safe.** Windowed, chunked, resumable, idempotent; per-partition watermarks; no unbounded
  full-table rewrites.
- **(I) Panel PIT-safety.** The exported panel is point-in-time by construction; cross-sectional operators
  rank only within the as-of cross-section; universe membership applied as-of; a lookahead test gates export.
- **(J) Semantic contract.** Every fact/metric/factor column declares unit, sign, and scale; a check fails on
  a value that violates its declared unit/sign domain.
- **(K) Release immutability *(new — PF4-S7).*** A published panel release is content-addressed +
  checksummed; re-publishing the same inputs is a no-op; a pinned release is never mutated or pruned.
- **(L) Client/view parity *(new — PF4-S8).*** The SDK read path returns rows identical to the contracted
  view read for the same `(as_of, universe, factors, release)`; a parity test gates the client.

**Data posture (PF4-specific).** Content sprints ship injectable loaders + engines + offline fixtures.
**Live migration/backfill is operator-run, gated per scope decision #1**, and recorded in the ledger with
live counts. PF4 proves determinism, resumability, and idempotency on bounded slices; the operator runs the
archive on explicit go.

**Process (all sprints).** Own git worktree off the integration mainline (`main`), merged back at sprint end,
via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>` — schema churn never touches the primary tree and
the git-ignored multi-GB DB is never copied. Controller `superpowers:subagent-driven-development` (fresh
implementer + reviewer per task; implementers use TDD + verification-before-completion). Never `git add -A`;
commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. New module ⇒
new `test_*.py`. `python -m pytest atx-impl\db\tests -q` green in the worktree before every commit (run from
`atx-impl/` — `db/calendar.py` shadows stdlib `calendar` if cwd is `db/`). Per sprint: update `PARITY_GAP.md`
and append a `WAREHOUSE_PARITY_TRANCHES.md` row.

---

## Primary-module ownership + reserved ranges

PF4 treats the shared hubs (`schema.py`, `migrations/`, `jobs.py`, `orchestrator.py`, `quality/`, `asof/`,
`lake.py`, `parity.py`, `observability.py`, plus `PARITY_GAP.md` / `WAREHOUSE_PARITY_TRANCHES.md`) as
**append-only coordination surfaces**. Each sprint appends under its reserved migration range and never edits
a prior migration or another sprint's region.

| Sprint | Primary modules (owns / creates) | Reserved migrations |
|---|---|---|
| PF4-S1 | NEW `db/signal_eval.py`; factor DQC in `db/quality/`; `db/tests/test_signal_eval.py` | 0176–0179 |
| PF4-S2 | `db/orchestrator.py` panel-gate hook, `db/observability.py` factor surfaces, `maintenance_schedule`; `db/tests/test_panel_gating.py` | 0180–0183 |
| PF4-S3 | targeted fixes in `db/enterprise_value.py`, `db/factors/{engine,cross_domain,cross_section}.py`, `db/factor_panel.py`, `db/universe.py`, `db/backfill.py`; new determinism/edge tests | 0184 (only if catalog/threshold rows needed) |
| PF4-S4 | `db/delisting.py` DLRET/DLSTCD populate + stitching; `db/tests/test_delisting_returns.py` | 0185–0188 |
| PF4-S5 | `db/universe.py` multi-universe + versioning; `db/tests/test_multi_universe.py` | 0189–0191 |
| PF4-S6 | NEW `scripts/warehouse_activate.py` harness, S1-DAG driver; `db/tests/test_activation_harness.py` | 0192–0194 |
| PF4-S7 | NEW `db/panel_release.py`, `db/lake.py` Arrow writer + release manifest; `db/tests/test_panel_release.py` | 0195–0197 |
| PF4-S8 | NEW `clients/atx-panel/` package (own `pyproject`); `clients/atx-panel/tests/` + a `db/tests/test_sdk_parity.py` | 0198 (client version row only) |
| PF4-S9 | NEW `atx-impl/docs/` (data dictionary generator, activation+consumption runbook, notebooks); `db/tests/test_data_dictionary.py` | none |
| PF4-S10 | `db/factor_panel.py` read-only/perf, NEW thin `db/panel_serving.py`; `db/tests/test_panel_serving.py` | 0199–0200 |
| PF4-S11 | `db/parity.py` surpass-ledger, `docs/` activation runbook, capstone catalog sweep; `db/tests/test_pf4_capstone.py` | 0201–0204 |

**Overlap note.** PF4-S4/S5/S6 touch the `fundamental_*`/pricing/universe surfaces and PF4-S7/S8/S10 share
the panel export/read/serve path — run each group **sequentially within itself**. Disjoint-module sprints may
run concurrently in isolated worktrees.

---

## Out of scope (parked for pf5)

ESG / sustainability, licensed vendor estimate feeds (IBES / broker-detail), international / IFRS / ESEF
fundamentals, and the supply-chain relationship graph. PF4 productionizes the **US-equity
fundamentals → ratios → factors → panel** product that already exists — closing pf3, hardening it, making it
survivorship-safe and dense, and packaging it for quant consumption. The parked domains are the natural pf5
successors and remain logged in `PARITY_GAP.md`.

---

## Success = the north star, evidenced

PF4 is done when: the signal-evaluation surface scores every factor; a panel-critical check halts a bad build
in the orchestrator and factor freshness/anomaly/lineage are observable; the S1–S10 review findings are
remediated and regression-locked; delisting returns make backtests survivorship-safe across ≥3 governed
universes; the activation harness makes a full migrate→backfill→rebuild→gate→export→release sequence
reproducible and operator-gated; immutable semver'd releases emit Parquet **and** Arrow; a `pip install`-able
`atx-panel` SDK returns a PIT panel bit-for-bit-identical to the contracted view; a generated data dictionary,
runbook, and runnable notebooks exist; a served read tier answers concurrent PIT reads safely; and the
capstone flips the surpass-ledger with resolvable evidence and passes a clean whole-branch pf3+pf4 review.
