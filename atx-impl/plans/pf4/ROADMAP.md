# pf4 — Production Quant-Product & Warehouse Activation (SDK + releases + activation + close pf3)

**Created:** 2026-07-06. Successor to **pf3** ([../pf3/ROADMAP.md](../pf3/ROADMAP.md)), which built a
signal-ready fundamentals **factor store** (definition-as-data catalog, PIT factor engine, cross-sectional
operators, fundamental + cross-domain factor families) on **production backfill rails** (windowed backfill +
maintenance DAG, schema-contract v2, architecture decomposition, PIT universe + price-backfill client,
fundamentals completeness, ratio/metric engine v2) and a backtest export panel.

**Design spec:** [../../../docs/superpowers/specs/2026-07-06-pf4-production-quant-product-design.md](../../../docs/superpowers/specs/2026-07-06-pf4-production-quant-product-design.md).

**Assumes pf1 PF-S1…S8, pf2 PF2-S1…S10, and pf3 PF3-S1…S10 have landed.** Where pf4 references a prior
deliverable whose exact landed name differs at implementation time, the pf4 implementer reconciles to the
landed name.

---

## Why pf4 exists — measured state (2026-07-06, audited against the repo)

pf3 was scoped as twelve sprints. A ground-truth audit shows it is **not complete**, on two axes:

1. **PF3-S11 (signal evaluation) and PF3-S12 (production capstone) were never built.** Migration bodies stop
   at `bodies_0164_0167.py` (S10); ranges `0168–0175` are unclaimed; `db/signal_eval.py`,
   `db/tests/test_signal_eval.py`, and `db/tests/test_pf3_capstone.py` do not exist; no factor-panel gate is
   wired into the orchestrator; there is no factor-domain observability. The warehouse can *produce* a factor
   panel but cannot *tell which factors carry signal*, cannot *halt a bad panel build*, and cannot *observe*
   panel freshness / collapse / lineage.
2. **Every pf3 sprint is `OPERATOR-PENDING`.** `WAREHOUSE_PARITY_TRANCHES.md` records that no live shared-DB
   migration/apply, no historical backfill, and no proof-slice population ran. Migrations `0132–0167` were
   never applied to the live 14 GB `atx_impl.duckdb`; `equity_daily_bars` still holds only ~3.18M rows for
   **2012–2014** while fundamentals are 2017–2026 — so the price×fundamental overlap, and therefore the factor
   tables, are effectively empty. Rails + engines are built and **fixture-proven**; the warehouse is
   **data-empty for factors and un-migrated live**.

A code review of S1–S10 found the architecture **sound** (no cross-date leakage, `available_at` propagated,
SQL parameterized) but with **1 High / 5 Med / ~6 Low** latent defects (→ PF4-S3). A 2026-07-06 offline-suite
run returned **~8 failures** — date-sensitive time-bombs (`formula_registry` `valid_to=2010-01-01` expired)
plus snapshot drift (`public_api_snapshot`, concept/factor-seed fixtures) — also owned by PF4-S3.

**pf4's mission:** take this code-complete-through-S10, fixture-proven, data-empty warehouse and turn it into
a **state-of-the-art, production-ready US-equity fundamental factor _product_ for downstream quant teams** —
by closing pf3's two missing sprints, hardening the S1–S10 code, making the data survivorship-safe + dense,
packaging a versioned/served/SDK-fronted product, and delivering a reproducible activation runbook.

---

## Scope decisions (user, 2026-07-06)

1. **Live-backfill posture = _Code + gated runbook_.** pf4 builds all code + a reproducible activation
   runbook; live migration/backfill against the 14 GB DB is executed **only on explicit per-step operator
   go**. No autonomous mutation of production data; no multi-hour vendor/SEC network pulls inside a sprint.
   "Full production read backfill complete" is delivered as a **turnkey, verifiable operator milestone** —
   pf4 makes it push-button and evidenced; the live run is a gated operator job.
2. **Breadth = _Tie-together + productionize_.** Close pf3 (S11 + S12), activate the backfill, build the
   quant-product layer. ESG / IBES licensed estimates / international-IFRS-ESEF / supply-chain graph remain
   **parked → pf5**.
3. **Consumer form = _Python SDK client_.** A thin installable `atx-panel` client is the primary interface
   (PIT reads, as-of universe filter, factor metadata, pandas + zero-copy Arrow out, release pinning), over
   the contracted views + versioned Parquet/Arrow releases. Example notebooks ship alongside.

---

## The northstar (pf4 acceptance)

> A downstream quant team can `pip install atx-panel`, pin an **immutable, semver'd panel release**, and pull
> a **point-in-time, lookahead-tested factor panel** (pandas or zero-copy Arrow) — every factor **scored for
> signal** (rank-IC / IC-decay / quantile spread / turnover / crowding / breadth), **survivorship-safe**
> (delisting returns populated), across **multiple governed universes**, with a **generated data dictionary**
> and **runnable example notebooks** — while the warehouse itself is **orchestrator-gated** (a panel-critical
> check halts a bad build), **observable** (factor freshness SLA, panel-collapse anomaly,
> lineage-completeness), and **reproducibly activatable** from a fresh checkout via a documented runbook. The
> four pf3 surpass axes are **evidenced** in the parity ledger.

---

## The eleven sprints

| Sprint | Track | Theme | Goal metric | Doc |
|---|---|---|---|---|
| **PF4-S1** | Close pf3 | **Signal-evaluation surface** — `db/signal_eval.py`: per-factor rank-IC + IC-IR + t-stat + sign-consistency over horizons {1,5,10,21,63}; IC-decay; quantile/decile long-short spread + monotonicity + hit-rate; turnover + rank autocorr; factor correlation + crowding; breadth; gated leakage + coverage DQC | every factor scored; zero-signal→IC≈0; monotone→monotone deciles; leakage DQC RED on planted leak; deterministic | [sprint-1-signal-evaluation.md](sprint-1-signal-evaluation.md) |
| **PF4-S2** | Close pf3 | **Panel gating + factor observability** — orchestrator panel-critical halt gate (`panel_quality_gate_halt`); factor freshness SLA, panel row-count anomaly, lineage-completeness; codified `maintenance_schedule` | orchestrator halts on planted panel-critical; stale/collapsed/lineage-broken flagged; schedule queryable | [sprint-2-panel-gating-observability.md](sprint-2-panel-gating-observability.md) |
| **PF4-S3** | Harden | **pf3 S1–S10 hardening + suite re-green** — the 1 High / 5 Med / ~6 Low review findings, TDD + regression-locked; re-green the offline suite + de-time-bomb date-sensitive tests | all High/Med closed with proof tests; full offline suite green, 0 time-bombs; public API re-pinned | [sprint-3-code-hardening.md](sprint-3-code-hardening.md) |
| **PF4-S4** | Data | **Survivorship-safe returns** — observed DLRET + DLSTCD reconciliation (injectable), delisting-return stitching, spinoff/merger policy, survivorship-bias DQC | terminal returns per delisted security-day; forward returns survivorship-safe; recon gate green | [sprint-4-survivorship-returns.md](sprint-4-survivorship-returns.md) |
| **PF4-S5** | Data | **Multi-universe + versioning** — ≥3 governed PIT universes beyond `us_common_equity_liquid_v1`; universe release versions + turnover reporting; universe-as-of in panel + SDK | ≥3 universes PIT-queryable; versions pinnable; turnover reported; panel/SDK filter by universe+version | [sprint-5-multi-universe.md](sprint-5-multi-universe.md) |
| **PF4-S6** | Data | **Activation harness + dense price backfill runbook** — resumable operator harness (dry-run plan + operator-gated exec) driving migrate→backfill→rebuild, widening `equity_daily_bars` (→2004+), evidence + verification report | dry-run plans archive without touching live DB; slice proves resumable+idempotent; verification report emits counts | [sprint-6-activation-harness.md](sprint-6-activation-harness.md) |
| **PF4-S7** | Product | **Panel release engine** — immutable semver releases (`YYYY.MM.patch`), manifest + changelog + checksums; Arrow/Feather + Parquet; pinnable content-addressed snapshots; retention never prunes a pinned release | release immutable + checksummed + pinnable; Arrow + Parquet emitted + validated; changelog generated | [sprint-7-panel-release-engine.md](sprint-7-panel-release-engine.md) |
| **PF4-S8** | Product | **`atx-panel` Python SDK** — installable client: `read_panel(as_of, universe=, factors=, release=)`, factor metadata/dictionary, pandas + zero-copy Arrow out, release pinning, isolated read-only connection | client installs standalone; PIT read matches view read bit-for-bit; read-only isolated; typed API | [sprint-8-atx-panel-sdk.md](sprint-8-atx-panel-sdk.md) |
| **PF4-S9** | Product | **Data dictionary + docs + notebooks** — generated factor/panel dictionary from `panel_contract`/`catalog`/`signal_eval`; fresh-agent activation + consumption runbook; quickstart notebooks | dictionary regenerates from contract; runbook followed offline; notebooks execute on a slice | [sprint-9-docs-dictionary-notebooks.md](sprint-9-docs-dictionary-notebooks.md) |
| **PF4-S10** | Product | **Served read tier + panel perf** — read-only served panel tier (isolated conn + Arrow cache), concurrency-safe reads (fix `read_only=False`), panel query perf within budget | concurrent reads isolated; cross-section read meets budget; served tier rows == SDK/view | [sprint-10-served-read-tier.md](sprint-10-served-read-tier.md) |
| **PF4-S11** | Capstone | **Production capstone** — end-to-end activation runbook (recover→migrate→backfill→rebuild→gate→observe→export→release→SDK-verify); surpass-ledger flip evidenced; whole-branch pf3+pf4 review → finish-branch; final catalog sweep | activation reproducible + deterministic on a slice; 4 axes cited to evidence; review clean; sweep green | [sprint-11-production-capstone.md](sprint-11-production-capstone.md) |

---

## Primary-module ownership + reserved ranges

pf4 treats the shared hubs (`schema.py`, `migrations/`, `jobs.py`, `orchestrator.py`, `quality/`, `asof/`,
`lake.py`, `parity.py`, `observability.py`, plus `PARITY_GAP.md` / `WAREHOUSE_PARITY_TRANCHES.md`) as
**append-only coordination surfaces**. Each sprint appends under its reserved migration range and never edits
a prior migration or another sprint's region. **pf3 used migrations through 0175; pf4 starts at 0176.**

| Sprint | Primary modules (owns / creates) | Reserved migrations |
|---|---|---|
| PF4-S1 | NEW `db/signal_eval.py`; factor DQC in `db/quality/`; `db/tests/test_signal_eval.py` | 0176–0179 |
| PF4-S2 | `db/orchestrator.py` panel-gate hook, `db/observability.py` factor surfaces, `maintenance_schedule`; `db/tests/test_panel_gating.py` | 0180–0183 |
| PF4-S3 | targeted fixes in `db/enterprise_value.py`, `db/factors/{engine,cross_domain,cross_section}.py`, `db/factor_panel.py`, `db/universe.py`, `db/backfill.py`; suite re-green + date-fixture freeze; new determinism/edge tests | 0184 (only if catalog/threshold rows needed) |
| PF4-S4 | `db/delisting.py` DLRET/DLSTCD populate + stitching; `db/tests/test_delisting_returns.py` | 0185–0188 |
| PF4-S5 | `db/universe.py` multi-universe + versioning; `db/tests/test_multi_universe.py` | 0189–0191 |
| PF4-S6 | NEW `scripts/warehouse_activate.py` harness, S1-DAG driver; `db/tests/test_activation_harness.py` | 0192–0194 |
| PF4-S7 | NEW `db/panel_release.py`, `db/lake.py` Arrow writer + release manifest; `db/tests/test_panel_release.py` | 0195–0197 |
| PF4-S8 | NEW `clients/atx-panel/` package (own `pyproject`); `clients/atx-panel/tests/` + `db/tests/test_sdk_parity.py` | 0198 (client version row only) |
| PF4-S9 | NEW `atx-impl/docs/` (dictionary generator, runbook, notebooks); `db/tests/test_data_dictionary.py` | none |
| PF4-S10 | `db/factor_panel.py` read-only/perf, NEW `db/panel_serving.py`; `db/tests/test_panel_serving.py` | 0199–0200 |
| PF4-S11 | `db/parity.py` surpass-ledger, `docs/` activation runbook, capstone sweep; `db/tests/test_pf4_capstone.py` | 0201–0204 |

**Overlap note.** PF4-S4/S5/S6 touch `fundamental_*`/pricing/universe; PF4-S7/S8/S10 share the panel
export/read/serve path — run each group **sequentially within itself**, never concurrently in the same tree.
Disjoint-module sprints may run concurrently in isolated worktrees.

---

## Shared PIT / determinism + production contract (every sprint)

pf1 **(A)–(D)**, pf2 **(E)–(G)**, pf3 **(H)–(J)** carry forward unchanged; pf4 adds **(K)–(L)**.

- **(A) Bitemporal / no lookahead.** `available_at = max(input.available_at)`; as-of readers gate on the valid
  window **and** `available_at ≤ as_of_ts`.
- **(B) Append-only, catalogued migrations.** Forward-only, idempotent; reserved range only; catalog every new
  table/view in the same migration; split schema from index; back up before any live apply.
- **(C) Offline / no-network tests.** In-memory / template-copy DuckDB, fixtures or injected files only. Live
  connectors behind injectable file options; live smoke operator-run + recorded.
- **(D) Determinism + provenance.** `compute_*` pure (pandas in → long DataFrame out), stable-sorted,
  unit-tested independent of DuckDB. Same inputs + params → same rows.
- **(E) Schema-as-contract.** No table without a contract row + `table_catalog` entry; drift check fails on
  divergence or any uncatalogued table.
- **(F) Backup-before-migrate.** CHECKPOINT + timestamped backup before every live apply, verify after; WAL
  split is the standing invariant.
- **(G) Quality-gated.** A `severity=critical` check is wired into the orchestrator and halts the run.
- **(H) Backfill-safe.** Windowed, chunked, resumable, idempotent; per-partition watermarks; no unbounded
  full-table rewrites.
- **(I) Panel PIT-safety.** Cross-sectional operators rank only within the as-of cross-section; universe
  membership applied as-of; a lookahead test gates export.
- **(J) Semantic contract.** Every fact/metric/factor column declares unit/sign/scale; a check fails on a
  value that violates its declared domain.
- **(K) Release immutability *(new — PF4-S7).*** A published release is content-addressed + checksummed;
  re-publishing identical inputs is a no-op; a pinned release is never mutated or pruned.
- **(L) Client/view parity *(new — PF4-S8).*** The SDK read path returns rows identical to the contracted view
  read for the same `(as_of, universe, factors, release)`; a parity test gates the client.

**Data posture (pf4-specific).** Content sprints ship injectable loaders + engines + offline fixtures. **Live
migration/backfill is operator-run, gated per scope decision #1**, and recorded in the ledger with live
counts. pf4 proves determinism, resumability, and idempotency on bounded slices; the operator runs the archive
on explicit go.

**Process (all sprints).** Own git worktree off the integration mainline, merged back at sprint end, via
`atx-impl/scripts/new_db_worktree.sh new|finish <slug>`. Controller `superpowers:subagent-driven-development`
(fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A`; commit
trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. New module ⇒ new
`test_*.py`. `python -m pytest atx-impl\db\tests -q` green in the worktree before every commit — **run from
`atx-impl/`, never from `db/`** (`db/calendar.py` shadows stdlib `calendar` and breaks collection if cwd is
`db/`). Per sprint: update `PARITY_GAP.md` and append a `WAREHOUSE_PARITY_TRANCHES.md` row.

---

## Sequencing

1. **Close pf3 + harden — PF4-S1 → PF4-S2 → PF4-S3 (sequential).** Signal-eval surface, then promote its DQC
   to orchestrator halt gates + factor observability, then harden the S1–S10 code and re-green the suite.
2. **Data correctness & density — PF4-S4 → PF4-S5 → PF4-S6 (sequential; share `fundamental_*`/pricing/universe).**
   Survivorship-safe returns, then multi-universe, then the dense-price activation harness that consumes both.
3. **Product surface — PF4-S7 → PF4-S8 → PF4-S9 → PF4-S10 (sequential; the SDK reads releases, docs document
   the SDK, the served tier fronts it).**
4. **Production capstone — PF4-S11 (last).**

**If you can only do a subset:** PF4-S1 (eval) + PF4-S2 (gating) + PF4-S6 (activation) + PF4-S7 (releases) +
PF4-S8 (SDK) + PF4-S11 (capstone) — the "scored, gated, activatable, releasable, SDK-consumable panel" spine.

---

## North star (pf4 acceptance)

pf4 is done when: the signal-evaluation surface scores every factor; a panel-critical check halts a bad build
in the orchestrator and factor freshness/anomaly/lineage are observable; the S1–S10 review findings are
remediated, the suite is green and time-bomb-free; delisting returns make backtests survivorship-safe across
≥3 governed universes; the activation harness makes a full migrate→backfill→rebuild→gate→export→release
sequence reproducible and operator-gated; immutable semver'd releases emit Parquet **and** Arrow; a
`pip install`-able `atx-panel` SDK returns a PIT panel bit-for-bit-identical to the contracted view; a
generated data dictionary, runbook, and runnable notebooks exist; a served read tier answers concurrent PIT
reads safely; and the capstone flips the surpass-ledger with resolvable evidence and passes a clean
whole-branch pf3+pf4 review.

**Out of scope for pf4 (parked for pf5):** ESG / sustainability, licensed vendor estimate feeds
(IBES / broker-detail), international / IFRS / ESEF fundamentals, and the supply-chain relationship graph.
