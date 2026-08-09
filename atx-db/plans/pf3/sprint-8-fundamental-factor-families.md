# Sprint PF3-S8 — Fundamental factor families (academic + signal-native)

**Goal:** populate the empty S7 factor catalog with the full set of FUNDAMENTAL factor families and turn
the framework into content. In scope: the academic canon — value, quality, profitability (including
Novy-Marx gross profitability), growth, investment, leverage, ACCRUALS (Sloan working-capital accruals),
and distress — plus the named multi-input composite scores (Piotroski F-score, Altman Z-score) — AND the
SIGNAL-NATIVE set that vendors do not ship, which is the point of surpass axis 2: PIT
revisions-momentum, standardization-delta anomalies, and footnote/segment-derived signals. Every family
emits a **PIT factor panel** keyed at `(security_id, as_of_date)`, every value **lineage-traced** back to
its source metrics, formula, standardization rule, and restatement vintage. Definitions land as **data**,
not bespoke code — a seed CSV (`db/seeds/factor_definitions.csv`) in the same definition-as-data spirit as
pf2's `formula_registry.csv` / `standardization_rules.csv`, with family-specific compute logic living in a
single new module. Reserved migrations **0156–0159**.

**Mandate / Owns:** NEW `db/factors/fundamental_families.py` (the family compute logic + the seed loader),
NEW seed `db/seeds/factor_definitions.csv` (the definition-as-data factor catalog rows), NEW
`db/tests/test_fundamental_factors.py`. All family definitions are seeded into the S7 factor catalog via
this sprint's reserved migrations; all family compute registers against the S7 PIT engine.

**Must NOT touch:** the factor ENGINE and cross-sectional OPERATORS — `db/factors/engine.py` and
`db/factors/cross_section.py` (rank / zscore / winsorize / neutralize) are **PF3-S7**'s surface; S8 adds
factor DEFINITION rows and per-family compute *against* that engine, it does not modify the engine or the
operator kernels. Cross-domain factors (price/liquidity/estimate/13F/short-interest/insider integration
into one namespace) are **PF3-S9**. The unified `v_factor_panel` export views and Parquet/Arrow export are
**PF3-S10**. Do not edit any landed migration (≤ 0155) or another sprint's reserved region.

**Depends on:** PF3-S7 (the factor framework: definition-as-data catalog, dependency DAG, PIT-safe compute,
and the cross-sectional operators every family standardizes through); PF3-S6 (the ratio & metric engine v2
— the profitability / leverage / per-share inputs the families read); pf2-S3 standardization
(`db/standardization.py` + `db/seeds/standardization_rules.csv` — the canonical standardized inputs, and
the surface the standardization-delta anomaly reads); pf2-S4 vintages (as-first-reported vs
most-recently-restated — the revisions-momentum input); pf2-S7 segments/footnotes (the
footnote/segment-derived signal inputs). **Sequential after PF3-S7, before PF3-S9** — S8/S9 share the
`db/factors/` package and never run concurrently in the same tree.

---

## Baseline / where the cycles go

The S7 factor framework is a *shape*, not *content*: it can hold, sequence, and PIT-compute factors, but
almost no fundamental family is defined in it, and the differentiating signal-native factors do not exist
anywhere in the warehouse. Measured 2026-07-04 against `atx-impl/db`.

1. **The S7 catalog is a framework with ~no fundamental family content.** PF3-S7 delivers the
   definition-as-data catalog, the dependency DAG, and PIT-safe compute with cross-sectional operators —
   but it is a *generic engine*. It ships the machinery to define and compute a factor; it does not ship
   the value/quality/profitability/growth/investment/leverage/accruals/distress *definitions* themselves.
   The catalog is effectively empty of fundamental content the day S8 opens.
2. **The academic factors are absent.** No Piotroski F-score, no Altman Z-score, no Novy-Marx gross
   profitability, no Sloan accruals factor exists as a governed factor. The *inputs* are partially staged —
   `db/fundamental_ratios.py::GROWTH_PRIOR_KEYS` already pairs prior-year `gross_profit`, `cost_of_revenue`,
   `cash_and_equivalents`, `inventory`, `accounts_payable` "for the Sloan working-capital-accruals
   composite" and "the Piotroski F-score YoY deltas" — but nothing assembles those inputs into a scored,
   catalogued, PIT factor. The scaffolding for the inputs exists; the factors do not.
3. **The SIGNAL-NATIVE factors — the surpass differentiator — do not exist anywhere.** These are the
   factors vendors cannot ship because they require the warehouse's own PIT machinery:
   revisions-momentum needs the pf2-S4 as-of vintages (the trend of a metric *as it was reported over
   successive vintages*, never the restated series); the standardization-delta anomaly needs the pf2-S3
   standardization surface (`db/standardization.py` + `db/seeds/standardization_rules.csv`) to compare a
   raw-tagged value against its standardized value; footnote/segment-derived signals need the pf2-S7
   segment/footnote sub-ledgers. None of these is a factor today.
4. **There is no seed-CSV factor catalog yet.** The definition-as-data pattern is proven for *formulas*
   and *standardization rules*, but no `factor_definitions.csv` exists — factor definitions have nowhere to
   live as data.

**Already good — do not regress:**
- The **definition-as-data seed pattern**: `db/seeds/formula_registry.csv`
  (`formula_code,family,kind,unit,numerator_code,denominator_code,…,expression,…,valid_from,valid_to`) and
  `db/seeds/standardization_rules.csv` (`rule_id,item_id,canonical_code,basis,…,valid_from,valid_to`) are
  the exact CSV shape `factor_definitions.csv` mirrors — governed, valid-windowed rows, not code.
- The **S7 PIT engine + cross-sectional operators** (`db/factors/engine.py`, `db/factors/cross_section.py`).
  S8 defines *against* them and standardizes *through* them; it does not fork or reimplement them.
- The **staged Sloan/Piotroski input priors** in `db/fundamental_ratios.py::GROWTH_PRIOR_KEYS` — S8 reads
  those, does not restate them.

---

## PIT / determinism + production contract

pf1 clauses **(A)–(D)**, pf2 **(E)–(G)**, and pf3 **(I)–(J)** apply in full; **(A)** and **(I)** are the
load-bearing clauses this sprint lives or dies on.

- **(A) / (I) Signal-native factors are strictly PIT.** Revisions-momentum uses **only the vintages
  available as-of** — the value as it was reported at each historical `available_at`, never the
  most-recently-restated series. A composite score (Piotroski, Altman, Sloan) sets
  `available_at = max(input.available_at)` across *every* input leg; if one leg is not yet available as-of,
  the score is not emitted for that date. Cross-sectional standardization ranks only within the as-of
  cross-section. A lookahead-detection test gates every family.
- **(B) / (F) Append-only, catalogued, backed-up migrations.** **0156** — value / quality / profitability
  family definition rows + compute registration (Novy-Marx gross profitability lands here). **0157** —
  growth / investment / leverage family rows + the named composite scores (Piotroski F, Altman Z, Sloan
  accruals). **0158** — the signal-native factors (revisions-momentum, standardization-delta,
  footnote/segment-derived). **0159** — indexes + the factor-family catalog view + `table_catalog` /
  `field_catalog` completeness. Every new table/view is catalogued in the same migration; timestamped
  DB+WAL backup precedes any live apply; strictly within 0156–0159.
- **(C) Offline / no-network tests.** Every family computes over in-memory / template-copy DuckDB with
  fixtures — a small fixture universe with hand-computed Piotroski / Altman / Sloan references and a planted
  restatement so revisions-momentum and standardization-delta are exercised without any vendor network.
- **(D) Determinism + provenance.** Each family's `compute_*` is pure (standardized metrics in → long
  factor DataFrame out), unit-tested independent of DuckDB; every emitted factor row records its full input
  lineage (source metrics → formula → standardization rule → vintage).
- **(J) Semantic contract.** Every factor column declares unit, sign convention, and scale in the contract
  (a higher Piotroski F is "better"; a lower Altman Z is more distressed; the accruals factor's sign is
  fixed so high-accruals = low-quality); a check fails on a sign/unit-domain violation.

---

## Tasks

### S8-0 — Core academic families (value / quality / profitability incl. Novy-Marx)

**Root cause:** the S7 catalog holds no value/quality/profitability definitions; the profitability family in
particular is missing Novy-Marx **gross profitability** (gross profit scaled by total assets), the single
most-cited fundamental profitability factor, even though `gross_profit` and `cost_of_revenue` are already
canonical standardized items.

**Fix:** NEW `db/factors/fundamental_families.py` — a seed loader plus per-family `compute_*` functions for
value (earnings/book/sales/cashflow yields), quality (accrual-free earnings quality, balance-sheet quality),
and profitability (gross profitability à la Novy-Marx, ROA/ROE/ROIC-based). Author the NEW seed
`db/seeds/factor_definitions.csv` mirroring `formula_registry.csv`'s shape (a `factor_code`, `family`,
input metric codes, standardization/neutralization directives, unit/sign, `valid_from`/`valid_to`), and
seed those rows into the S7 catalog in migration **0156**. Each family standardizes through the S7
operators; nothing recomputes ranks itself.

**PIT:** (A)/(I) inputs read as-of; (D) pure compute unit-tested off-DuckDB. **(B)** 0156 rows + catalog.

**Accept:** value/quality/profitability families each emit a PIT factor panel on the fixture slice;
Novy-Marx gross profitability reconciles to a hand reference; every row is lineage-traced to source
metrics + formula + vintage; seed rows round-trip from `factor_definitions.csv` into the catalog.

### S8-1 — Growth / investment / leverage + named composite scores

**Root cause:** the growth, investment (asset-growth / capex-intensity), and leverage families are absent,
and — most importantly — the named multi-input composites (Piotroski F-score, Altman Z-score, Sloan
accruals) that quants ask for by name do not exist despite their input priors being staged in
`fundamental_ratios.py`.

**Fix:** extend `db/factors/fundamental_families.py` with growth (YoY / CAGR of the standardized metrics),
investment (total-asset growth, capex/assets), and leverage (debt/equity, debt/assets, coverage) families,
plus the composite scorers: **Piotroski F** (the nine binary profitability/leverage/efficiency signals,
summed), **Altman Z** (the weighted five-ratio distress score), and the **Sloan working-capital accruals**
factor (ΔWC − depreciation, scaled by average assets). Seed their definition rows into
`factor_definitions.csv`; land compute registration in migration **0157**.

**PIT:** (A)/(I) a composite emits only when *all* legs are available as-of; `available_at = max(legs)`.
(D) each composite pure + hand-referenced. **(B)** 0157 rows + catalog.

**Accept:** growth/investment/leverage families emit PIT panels; Piotroski F, Altman Z, and Sloan accruals
compute and **reconcile to hand references** on the fixtures; a composite with one leg unavailable as-of is
correctly withheld (not silently backfilled from a later vintage).

### S8-2 — Signal-native factors (surpass axis 2)

**Root cause:** the factors that differentiate the warehouse from FactSet/Compustat — the ones only the
warehouse's own PIT/standardization/footnote machinery can produce — do not exist. This is the surpass-axis-2
deliverable and the whole reason pf2-S3/S4/S7 were built.

**Fix:** add three signal-native families to `db/factors/fundamental_families.py`: (a) **PIT
revisions-momentum** — the trend/sign of successive as-of vintages of a metric (pf2-S4), capturing whether
management keeps revising a number up or down *as it was seen at the time*; (b) **standardization-delta
anomaly** — the gap between a firm's raw-tagged value and its standardized value (pf2-S3 via
`db/standardization.py`), a tagging-aggressiveness / earnings-management proxy; (c) **footnote/segment-derived**
signals — dispersion / concentration / disclosure-change signals from the pf2-S7 segment and footnote
sub-ledgers. Seed definition rows; land in migration **0158**.

**PIT:** (A)/(I) **strict** — revisions-momentum MUST read only vintages with `available_at ≤ as_of_date`
and NEVER the restated series; the standardization-delta reads the standardized value as-of. (D) pure,
fixture-referenced with a planted restatement. **(B)** 0158 rows + catalog.

**Accept:** all three signal-native families are present, catalogued, and lineage-traced; a
leakage/lookahead test proves revisions-momentum uses as-of vintages only; the standardization-delta and
footnote/segment signals emit on the fixture slice.

### S8-3 — Family panels + lineage + gated checks

**Root cause:** individual family compute is not the same as a governed, gate-checked panel surface; without
coverage and lineage-completeness gates, a family can silently emit thin or lineage-broken rows.

**Fix:** materialize each family as a PIT factor panel through the S7 engine, and register two gated
`db/quality.py`-style checks reading this sprint's catalog rows: **lineage-completeness** (every emitted
factor row resolves a full source-metric → formula → standardization-rule → vintage chain) and
**factor-coverage** (each defined family emits ≥ the expected factor count over the slice). Add the
factor-family catalog view + `table_catalog`/`field_catalog` completeness in migration **0159**.

**PIT:** (I) panel PIT-safe by construction; (G) both checks authored `severity=critical`, gate-ready for
PF3-S12. **(B)** 0159 indexes + catalog view.

**Accept:** each family emits a PIT panel; lineage-completeness and factor-coverage checks are green on the
slice and red on planted fixtures (a lineage-broken row, a family emitting zero factors); the catalog view
carries its own catalog rows.

---

## Sequencing & expected compounding

**S8-0 → S8-1 → S8-2 → S8-3.** S8-0 stands up the module + the seed CSV and lands the core academic families
(the definition-as-data spine every later task extends). S8-1 adds the remaining academic families and the
named composites *on that spine*. S8-2 adds the signal-native factors — the differentiator — once the
academic families prove the pattern end-to-end. S8-3 gates the whole surface. Compounding: the fundamental
families + signal-native factors are the **core content of the unified factor panel** — PF3-S9 unifies them
with cross-domain factors into one namespace and PF3-S10 exports that panel — and they are the **most
differentiated surface vs vendors** (surpass axis 2 evidenced, surpass axis 1 lineage threaded through every
factor row). An empty catalog after S7 becomes a populated, gate-checked, lineage-traced factor store after
S8.

---

## Risks / guardrails

- **Composite scores are silent-leak magnets.** Piotroski F and Altman Z each mix many inputs; if even one
  leg is not PIT + lineage-traced, the score leaks lookahead invisibly (the number still computes). Every
  leg of every composite must resolve as-of and record lineage, and `available_at = max(legs)` — a
  composite is withheld, never approximated, when a leg is missing as-of.
- **Signal-native factors are the differentiator AND the highest leakage risk.** Revisions-momentum in
  particular MUST read as-of vintages and NEVER the most-recently-restated series — restated inputs would
  make the factor trivially predictive and worthless. The leakage test on S8-2 is mandatory, not optional.
- **Definition-as-data over bespoke code.** Every factor expressible as a `factor_definitions.csv` row
  (input codes + standardization/neutralization directives + unit/sign) is a row, not a function; only the
  multi-input scorers (Piotroski/Altman/Sloan) and signal-native families warrant per-family compute.
- **Stay in lane.** No edits to the S7 engine/operators, no cross-domain factors (S9), no export views
  (S10), no landed migration touched; strictly within **0156–0159**.

---

## Bench / acceptance

- Each fundamental family (value / quality / profitability / growth / investment / leverage / accruals /
  distress) emits a PIT factor panel on the proof slice.
- Piotroski F-score, Altman Z-score, and Sloan accruals **compute and reconcile to hand references** on the
  fixtures; a composite with an unavailable leg is correctly withheld.
- The signal-native factors (revisions-momentum, standardization-delta, footnote/segment-derived) are
  **present, lineage-traced, and leakage-tested** — revisions-momentum proven to use as-of vintages only.
- Lineage-completeness and factor-coverage checks green on the slice, red on planted fixtures.
- `python -m pytest atx-impl\db\tests\test_fundamental_factors.py -q` green, and full
  `python -m pytest atx-impl\db\tests -q` green before commit.
- **Live-DB smoke** recorded in the ledger: per-family emitted factor-row counts on the live proof slice,
  the composite reconciliation deltas, and the `run_id`.
- `PARITY_GAP.md` updated (fundamental factor families + signal-native surpass axis 2 now populated); a
  `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, families landed, verification commands, live
  smoke with exact per-family counts + run_id, caveats/next → PF3-S9 cross-domain unification).

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish
sprint-8-fundamental-factor-families`; controller `superpowers:subagent-driven-development` (fresh
implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage explicit
paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
