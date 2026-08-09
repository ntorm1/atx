# Sprint PF3-S6 — Ratio & metric engine v2 (complete Compustat/FactSet catalog + growth + lineage)

**Goal:** complete the derived-metric catalog to full FactSet / S&P GMI Compustat coverage and make every
number in it queryably lineage-traced. Today `fundamental_ratios.py` materializes a strong ~53-formula core
(margins, a handful of returns, efficiency turnovers, leverage, coverage, liquidity, per-share, payout,
cash-flow, and four distress/quality scores) driven definition-as-data from `formula_registry`; S6 finishes
the catalog — the full margins / returns (ROE / ROA / ROIC) / efficiency (turnovers, days-outstanding) /
leverage / coverage / liquidity / per-share / **valuation** multiple families — and adds a first-class
**growth / CAGR engine** (YoY, QoQ, multi-year CAGR, growth-stability / consistency). Everything stays driven
by the `formula_registry` (definition-as-data, no `eval`/`exec`), PIT- / TTM- / calendar-aware (pf2-S6), and
every emitted metric is **lineage-traced** to its source facts + formula + restatement vintage (surpass axis 1
made a queryable surface). The existing ~53 ratios must stay bit-for-bit reconcilable — regression-locked — so
completing the catalog never silently perturbs a landed number. Reserved migrations **0148–0151**.

**Mandate / Owns:** completion of `db/fundamental_ratios.py` + `db/formula_library.py` (new `formula_registry`
seed rows and any thin engine glue the existing interpreter does not already cover), a NEW
`db/metric_engine.py` (the orchestrating derived-metric compute + lineage surface), and
`db/tests/test_metric_engine.py`. New formula families land as **registry rows first**; new dispatch code is
added only where the closed operand-term / composite interpreter genuinely cannot express a metric.

**Must NOT touch:** the standardization **engine** (pf2-S3) — S6 *reads* its output (`fundamental_standardized`)
and never edits how a value is standardized; the factor cross-sectional layer (PF3-S7) — ratios and metrics
are **inputs** to factors, not factors, so no `rank`/`zscore`/`neutralize`/`winsorize` belongs here; and the
industry-template statement maps (pf2-S5 owns the bank / broker-dealer / insurance / reit / utility statement
mapping — S6 may *consume* industry-standardized items but authors no template edits). No edits to any landed
migration (≤ 0147) and no writing outside 0148–0151.

**Depends on:** PF3-S5 (complete valuation inputs — enterprise-value components, float / treasury shares —
without which the valuation multiples and per-share family stay stubs), pf2-S3 (the standardized surface every
formula reads), and pf2-S6 (calendarization / TTM, the flow basis and fiscal-calendar alignment the growth
engine and TTM ratios require). Sequential **after PF3-S5** (dense, complete inputs) and **before the factor
wave (PF3-S7/S8)** — the complete, lineage-traced metric catalog is the direct input set the fundamental
factor families consume.

---

## Baseline / where the cycles go

The ratio spine is real and well-shaped; the gap is **completeness**, a **growth engine**, a **queryable
lineage surface**, and **valuation density**. Measured 2026-07-04 against `atx-impl/db`.

1. **The ratio catalog is a strong core, not the full vendor catalog.** `RATIO_DEFS` is built by
   `_build_ratio_defs()` (`fundamental_ratios.py:136`) mechanically from `formula_registry` seed rows via
   `load_ratio_formula_rows()` — the ~53 hand-authored `RatioDef` entries pf1-S4 ported under a byte-identity
   gate. The seed (`db/seeds/formula_registry.csv`) has since begun growing (currently ~87 rows across
   profitability / leverage / efficiency / liquidity / cash_flow / payout / per_share / health / growth /
   valuation plus nascent industry families), but it is still **short of** the ~hundreds-of-metrics
   FactSet / Compustat derived catalog: many returns (a clean ROIC on invested capital), coverage ratios
   (interest / fixed-charge / EBITDA-based), days-outstanding efficiency metrics (DSO / DIO / DPO / cash-
   conversion-cycle), and the full valuation-multiple set are missing or thinly seeded. Completing them is the
   bulk of S6-0.
2. **There is no first-class growth / CAGR engine.** The seed carries ~6 `growth`-family rows, but there is no
   engine that computes **YoY / QoQ / multi-year CAGR / growth-stability** as a family over a metric's own
   history — growth today is a per-formula special case, not a systematic, TTM- and calendar-aware transform
   over the period axis. A quant fundamentals warehouse needs revenue / earnings / FCF / book-value growth and
   their multi-year CAGRs and consistency scores as first-class, lineage-traced metrics.
3. **Lineage exists per-row but is not a queryable surface.** Each `fundamental_ratios` row already records its
   input provenance (`input_codes_json`, `input_item_ids_json`, `numerator_code`/`denominator_code`,
   `available_at = max(input.available_at)`, `is_latest_revision`, `vintage_class`), but there is no
   `metric → {source facts, formula, vintage}` **as-of surface** a caller can query to answer "what did this
   metric depend on, and which vintage, as of date D." Surpass axis 1 requires exactly that queryable chain.
4. **Valuation multiples are underpopulated pending upstream data.** `_FUNDAMENTAL_RATIO_EXCLUDED_FAMILIES =
   {"valuation"}` (`fundamental_ratios.py:126`) intentionally routes the valuation family to the sibling
   valuation-multiples engine; those emit few rows on the current proof slice precisely because EV components,
   float shares, and the price×fundamental overlap were incomplete (PARITY_GAP / pf2-S9). PF3-S4 (dense
   overlap) and PF3-S5 (EV + float) close that upstream — S6 then completes the valuation catalog against real
   inputs.

**Already good — do not regress:**
- **Definition-as-data `formula_registry`.** New families are *rows*, not code. The strict CSV contract
  (`SEED_COLUMNS`, `read_formula_registry_seed`) and `_build_ratio_defs()`'s mechanical row → `RatioDef`
  translation are the extension surface — preserve them.
- **The closed operand-term interpreter — NO `eval`/`exec`.** `eval_operand_term` dispatches a fixed grammar
  (`key` / `abs` / `sum` / `abs_sum` / `diff` / `diff_z` / `avg`), and `resolve_composite_evaluator` maps a
  whitelisted `composite:<code>` dispatch key to one of the four vetted, unit-tested score functions (Altman
  Z'', Piotroski F, Ohlson O, Beneish M). Every new metric expresses through this closed dispatch; a genuinely
  new shape adds a *named evaluator*, never arbitrary code execution.
- **Standardized-input preference (pf2-S3).** Formulas read `fundamental_standardized` (with
  `fundamental_ttm_points` for flows and `fundamental_statement_points` for balances) — the canonical, industry-
  aware surface. S6 consumes it and does not reach around it to raw XBRL.
- **PIT availability computation.** Every ratio sets its own `available_at` from the max availability of the
  specific inputs it consumes, with `as_of_date` = period close. New metrics inherit this discipline unchanged.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal / no-lookahead, **(B)** append-only catalogued migrations, **(C)** offline /
no-network tests, **(D)** determinism + provenance, **(E)** schema-as-contract, and **(J)** semantic contract
apply in full to this sprint.

- **(A)** Every derived metric row carries `as_of_date`, `available_at`, `source_loaded_at`, `run_id`,
  `is_latest_revision`; a metric sets `available_at = max(input.available_at)` so an as-of read returns it only
  once every input was knowable. Growth / CAGR metrics compute over the period axis using only periods whose
  inputs were available as-of, and are vintage-consistent (first-reported vs restated never mixed within one
  series).
- **(J)** Every new metric column declares its **unit and sign** in the contract (extending PF3-S2's semantic
  contract) — ratio / currency / currency_per_share / per_share / pct / score / days — and a value violating
  its declared unit/sign domain fails the check.
- **(D)** `compute_*` transforms stay pure (pandas in → long DataFrame out), unit-tested independent of DuckDB;
  same inputs + params → same rows. The growth engine is a pure transform over a metric's period history.
- **(E)/(B)** No table / view lands without a contract row and a `table_catalog` entry, seeded in the same
  migration. Reconciliation to pf2's ratio outputs is a **hard gate**. Migrations, schema-vs-index split per
  the standing precedent: **0148** (ratio-family expansion — new registry rows + any metric columns / catalog),
  **0149** (growth / CAGR surface), **0150** (metric lineage view), **0151** (indexes + catalog completion).
- **(C)** Every test runs against in-memory / template-copy DuckDB with fixture or injected data; live
  proof-slice counts are operator-run and recorded in the ledger, never asserted in pytest.

---

## Tasks

### S6-0 — Complete the ratio catalog

**Root cause:** the `formula_registry`-driven catalog is a strong core (~53 ported formulas) but is short of the
full FactSet / Compustat derived catalog — returns beyond ROE/ROA (ROIC on invested capital), the full coverage
family (interest / fixed-charge / EBITDA coverage), days-outstanding efficiency (DSO / DIO / DPO / CCC), the
complete leverage / liquidity / per-share set, and the valuation multiples — are missing or thinly seeded.

**Fix:** extend `formula_registry` (and therefore `RATIO_DEFS`, with no engine rewrite) with the complete
margins / returns / efficiency / leverage / coverage / liquidity / per-share / valuation families as **new
registry rows**, each with `family`, `kind`, `unit`, numerator/denominator codes + item-ids, `inputs` gating
tuple, `expression` (operand-term or `composite:` dispatch), `is_meaningful_rule`, `definition`, and vendor /
academic `citation`. Add new **named operand evaluators** to the closed `eval_operand_term` dispatch **only**
where an existing shape cannot express a metric (e.g. a multi-term invested-capital denominator) — never new
free-form code. Where a formula belongs to the valuation family, route it through the existing valuation-
multiples path (`_FUNDAMENTAL_RATIO_EXCLUDED_FAMILIES`) rather than duplicating it. Migration **0148** seeds the
new rows and catalogs any new metric columns.

**PIT:** (A) each new formula sets `available_at` from the max availability of its own inputs; TTM formulas read
`fundamental_ttm_points`, balance formulas `fundamental_statement_points`, all through `fundamental_standardized`.
(J) each new metric declares unit + sign. (B) rows + catalog in 0148.

**Accept:** the full margins / returns / efficiency / leverage / coverage / liquidity / per-share / valuation
catalog emits on the proof slice; every new row parses through the closed interpreter (zero `eval`/`exec`); the
existing ~53 formulas are byte-unchanged (S6-3 gate); each new metric carries a unit + sign contract row.

### S6-1 — Growth / CAGR engine

**Root cause:** there is no systematic growth family — YoY / QoQ / multi-year CAGR and growth-stability are not
computed as first-class, TTM- and calendar-aware transforms over a metric's own period history.

**Fix:** add a growth / CAGR engine (a pure `compute_growth_rows`-style transform, seeded by `formula_registry`
growth-family rows naming a base metric + window + mode) computing **YoY**, **QoQ**, **multi-year CAGR**
(configurable horizon, e.g. 3y / 5y), and a **growth-stability / consistency** score (e.g. sign-consistency /
dispersion of the growth series) over revenue / earnings / FCF / book-value and any base metric flagged
growable. It is TTM- and calendar-aware (pf2-S6): YoY compares fiscal-aligned periods, QoQ adjacent fiscal
quarters, CAGR the endpoints of the horizon window. Materialized to its own catalogued surface via migration
**0149**.

**PIT:** (A) growth over period t uses only periods with `available_at ≤ as_of`; the output row's
`available_at = max` over the periods consumed. **Vintage-consistency is mandatory** — a growth series is
computed entirely within one vintage class (first-reported *or* restated), never mixed (pf2-S4). (D) pure
transform, unit-tested. (J) growth metrics declare unit=pct (or ratio for CAGR) + sign.

**Accept:** YoY / QoQ / multi-year CAGR / growth-stability emit for the growable base metrics on the slice;
each growth row is vintage-consistent and lineage-traced to the exact periods it consumed; recomputation is
deterministic.

### S6-2 — Metric engine + queryable lineage

**Root cause:** derived-metric computation is spread across `fundamental_ratios` and the valuation-multiples
path with per-row provenance only; there is no single orchestrator and no queryable `metric → facts + formula +
vintage` as-of surface (surpass axis 1 remains latent).

**Fix:** NEW `db/metric_engine.py` orchestrating all derived-metric computation (ratios + growth + valuation) as
one governed pass that records **input lineage** for every metric, plus a **metric-lineage view** exposing a
`metric → {source XBRL fact(s) / item-ids, formula_code + expression, restatement vintage, available_at}`
**as-of** surface — the queryable transparency chain vendors do not ship. The view is catalogued with its own
`table_catalog` / `field_catalog` rows and read through an as-of reader gating on catalog / metric availability
(mirroring the `formula_registry_asof` / `fundamental_ratios_asof` precedent). Migration **0150** adds the view.

**PIT:** (A) the as-of reader excludes any metric whose lineage inputs were not yet available as-of; no
lookahead. (D) lineage rows are a deterministic projection of the metric inputs. (B) view catalogued in 0150.

**Accept:** for any emitted metric, the lineage surface returns its source facts + formula + vintage as-of; an
as-of date before a metric's `available_at` excludes it; lineage-completeness (every metric row resolves to ≥1
source fact + a formula + a vintage) holds on the slice.

### S6-3 — Reconciliation gate + catalog

**Root cause:** completing the catalog and adding a metric engine risks silently perturbing a landed ratio; and
"the catalog is complete + fully lineage-traced" needs to be an enforced invariant, not a claim.

**Fix:** a **reconciliation gate** that regression-locks pf2's ~53 ratios to be byte / near-identical (within a
declared numeric tolerance) between the pre-S6 engine and the completed engine, plus a **lineage-completeness
gated check** (severity=critical, clause G) asserting every metric row resolves to source facts + formula +
vintage. Emit a **metric catalog** (a catalogued index of every metric_code with family / unit / sign /
definition / citation) so the full catalog is itself queryable. Migration **0151** adds indexes + the catalog
completion.

**PIT:** (G) both checks authored gate-ready and wired so a regression or lineage gap halts the run. (C)
offline fixtures with a planted perturbed ratio and a planted lineage-orphan each go red. (B) indexes / catalog
in 0151.

**Accept:** the reconciliation gate is green (every pre-existing ratio reconciles within tolerance) and red on
a planted perturbation; the lineage-completeness check is green live and red on a planted orphan; the metric
catalog enumerates every emitted metric with unit + sign + definition.

---

## Sequencing & expected compounding

**S6-0 → S6-1 → S6-2 → S6-3.** S6-0 completes the base ratio catalog (the widest surface, load-bearing for
everything after). S6-1 layers the growth / CAGR family **on top of** the now-complete base metrics (growth of
a metric presupposes the metric). S6-2 then wraps both in the metric engine and exposes the queryable lineage
surface over the full, complete set. S6-3 locks it: reconciliation regression + lineage-completeness gate +
metric catalog. Compounding: the complete, lineage-traced metric catalog is the **direct input set** for the
fundamental factor families (PF3-S8) — value / quality / profitability / growth / leverage factors read these
metrics, and the lineage surface is what lets a factor trace end-to-end to source facts (surpass axis 1). A
factor wave built on an incomplete or unlineaged metric catalog would be built on sand.

---

## Risks / guardrails

- **Reconciliation is the guardrail.** Any change to an existing ratio must be *intentional* and
  regression-gated: the S6-3 byte / near-identical reconciliation of pf2's ~53 ratios is the tripwire that
  catches an accidental perturbation from a shared-input refactor or a new registry row colliding with an old
  `formula_code`.
- **Growth across vintages must be vintage-consistent.** A CAGR that mixes a first-reported endpoint with a
  restated endpoint is silently wrong. Every growth / CAGR series is computed entirely within one vintage class
  (pf2-S4); the vintage-consistency assertion is part of S6-1's accept.
- **Prefer registry ROWS over new code.** The whole point of the definition-as-data `formula_registry` + closed
  operand-term interpreter is that new metrics are *data*. Adding dispatch code (and never `eval`/`exec`) is the
  exception, allowed only when no existing operand shape expresses the metric — and then as a named, unit-tested
  evaluator.
- **Read, don't rewrite, upstream.** Formulas consume the pf2-S3 standardized surface and pf2-S6 TTM /
  calendarization; S6 never edits standardization or calendarization logic, and never authors an industry
  statement-map edit (pf2-S5's lane).
- **Stay in 0148–0151.** Schema / growth-surface / lineage-view / index split across the four reserved numbers;
  no landed migration (≤ 0147) is touched; timestamped DB + WAL backup before any live apply.

---

## Bench / acceptance

- The **full ratio catalog** (margins / returns / efficiency / leverage / coverage / liquidity / per-share /
  valuation) emits on the proof slice; every new formula parses through the closed interpreter with zero
  `eval`/`exec`.
- The **growth / CAGR** family (YoY / QoQ / multi-year CAGR / growth-stability) emits, vintage-consistent and
  lineage-traced to the exact periods consumed.
- **Every metric traces to facts + formula + vintage** via the S6-2 lineage as-of surface; lineage-completeness
  holds on the slice.
- **pf2's ~53 ratios reconcile** (regression gate green) — any intentional change is explicit and gated; a
  planted perturbation goes red.
- `python -m pytest atx-impl\db\tests\test_metric_engine.py -q` green, and full `python -m pytest
  atx-impl\db\tests -q` green in the worktree before commit.
- **Live proof-slice smoke** recorded in the ledger: per-family emitted metric counts, growth-family counts,
  lineage-completeness count, reconciliation residual (with tolerance), and the `run_id`.
- `PARITY_GAP.md` updated (ratio / metric catalog status advanced; surpass-axis-1 lineage surface noted); a
  `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke with
  exact counts + run_id, caveats / next → PF3-S7 factor framework).

**Process:** each sprint runs in its **own git worktree** off `main` via
`atx-impl/scripts/new_db_worktree.sh new|finish sprint-6-ratio-metric-engine-v2`; controller
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; implementers use TDD +
verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. New module
⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
