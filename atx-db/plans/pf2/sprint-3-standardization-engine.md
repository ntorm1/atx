# Sprint PF2-S3 — Standardization engine

**Goal:** stand up the standardization engine — the #1 Compustat/FactSet differentiator over raw XBRL. Produce `fundamental_standardized`, a long-format PIT fact (one row per `security_id`, `item_id`, `period_end`, `basis`) built by a pure deterministic engine that applies *declarative* rules from a NEW `db/seeds/standardization_rules.csv` — per canonical `item_id`: source concept(s), a combination rule, a sign convention, a scale/unit normalization, and a missing-value policy — so a filer's idiosyncratic us-gaap tags + custom extensions land on ONE fixed cross-company/cross-time template (~300 annual / ~100 quarterly core items). Route `fundamental_ratios.py` to read the standardized inputs, reconciliation-gated to pf1. Reserved migrations 0103–0106.

**Mandate / Owns:** NEW `db/standardization.py`, NEW `db/seeds/standardization_rules.csv`, the `fundamental_standardized` table, the `fundamental_ratios.py` change that reads standardized inputs, `db/tests/test_standardization.py`.

**Must NOT touch:** the pf1-S1 item dimension — `db/item_registry.py` and the `fundamental_item` / `fundamental_item_alias` / `fundamental_item_vendor_map` tables (read-only; the engine *consumes* `Registry.resolve_item` / `resolve_inputs` and the `sign_convention` / `unit_type` / `coalesce_priority` columns, never rewrites them). PF2-S4's ratio-vintage region of `fundamental_ratios.py` (S3→S4 serial — both touch this file). `formula_registry` / `formula_library.py` derivation math (PF-S4 owns the ratio arithmetic; S3 only changes which *inputs* feed it). The producers `fundamental_statements.py` / `fundamental_xbrl_metrics.py` — read their outputs (`fundamental_statement_points`, `fundamental_xbrl_metric`) as engine inputs; do not change how they are produced.

**Depends on:** pf1-S1 item dictionary (`fundamental_item` + `fundamental_item_alias` + `fundamental_item_vendor_map` + the `Registry` resolver); pf1-S3 concept coverage (the `FUNDAMENTAL_STATEMENT_MAP_ROWS` projection and `fundamental_xbrl_metric`); pf1-S4 `formula_registry` (the definition-as-data pattern these rules mirror). **FIRST content sprint** — everything downstream reads the standardized template. S3→S4 sequential.

---

## Baseline / where the cycles go

Standardization is the single biggest residual parity lever (ROADMAP fact #1). pf1 built the *dictionary*; it never built the *engine*. Measured against the live code this session:

1. **A dictionary, no normalization engine.** `item_registry.Registry` maps `(alias_scheme, alias_code)` → `item_id` via `resolve_item`, and `fundamental_item` carries `sign_convention` (`positive` / `positive_expense`), `unit_type` (`monetary` / `shares` / `per_share`), and `coalesce_priority` per item. **Nothing consumes those governance columns** to force a filer value onto a template. They are documentation, not a transform.
2. **The ratio surface reads a handful of raw concepts, not a template.** `fundamental_ratios.load_ratio_inputs` pivots `fundamental_ttm_points`, `fundamental_statement_points`, and `fundamental_xbrl_metric` through `_pivot_case`, matching `canonical_metric = '<literal>'` from `TTM_INPUTS` / `BALANCE_INPUTS` / `XBRL_BALANCE_INPUTS` / `XBRL_FLOW_INPUTS`. Comparability is only as good as whichever *single* us-gaap concept each filer happened to tag — no cross-company coalesce, no template guarantee.
3. **Custom filer extensions are silently dropped.** `fundamental_xbrl_metrics.normalize_xbrl_metric_rows` does `out["concept"].map(CONCEPT_MAP)` then keeps only `canonical_metric.notna()`. Every concept outside the ~few-dozen-entry `CONCEPT_MAP` — the entire custom-extension long tail that breaks cross-company comparability — vanishes with **no exception record**. `fundamental_item_vendor_map` is seeded by `seed_fundamental_item_registry` but **nothing routes extensions through it.**
4. **Combination/coalesce exists only as ad-hoc, split literals.** The statement map encodes N-way COALESCE as multiple `FUNDAMENTAL_STATEMENT_MAP_ROWS` at ascending `concept_priority` (revenue: `RevenueFromContractWithCustomerExcludingAssessedTax` p10 → `Revenues` p20 → `SalesRevenueNet` p30) and sums via `__DERIVED__` sentinel rows carrying a `derivation_expr` (`total_debt = st_debt + lt_debt`, `ebit = pretax_income + interest_expense`). Right shape, wrong home: it lives half in `fundamental_statements.py` literals and half in the ratio pivots, and is never applied as one deterministic pass emitting a comparable per-item fact.
5. **No template-coverage measure.** There is no notion of "of the ~300 annual / ~100 quarterly standardized items, N are populated for security X in period P." A filer that buries a line in "other" just leaves a gap; nothing counts, gates, or reports it.

**Already good — do not regress:**
- `Registry` alias resolution + the `sign_convention` / `unit_type` / `coalesce_priority` governance columns, already validated (`_validate_aliases` rejects overlapping alias windows). The engine *reads* these; it never re-derives them.
- Source-fact bitemporal discipline: `available_at`, `as_of_date`, `is_latest_revision`, `run_id`, and `_pivot_case`'s `is_latest_revision` + `period_type` filters. The engine preserves them unchanged.
- The pf1-S1/S4 **reconcilable-rebuild** discipline (the S4-1 byte-identity gate over the 53 ratio codes). Standardization *adds* a comparable surface; it must not silently move an existing ratio value without an explicit, tested reason.
- The pure-transform posture: `compute_ratio_rows` and `normalize_xbrl_metric_rows` are pandas-in / DataFrame-out, DuckDB-free, independently unit-tested. The new `compute_standardized_rows` matches it exactly.

---

## PIT / determinism + production contract

ROADMAP clauses **(A)** bitemporal, **(B)** append-only catalogued migrations, **(C)** offline tests, **(D)** determinism/provenance apply in full; **(E)** schema-as-contract (PF2-S1) applies to the new table.

- **(A)** Every `fundamental_standardized` row sets `available_at = max(input.available_at)`, `as_of_date = period_end`, `is_latest_revision`, `run_id` — a standardized item is knowable only once every source concept it combines was knowable. No lookahead.
- **(B)** Migrations **0103–0106** only. `0103` creates `fundamental_standardized` + seeds `table_catalog` / `field_catalog`; `0104` the lookup index (schema/index split per the S5g/S5k WAL-replay precedent); `0105` the `fundamental_standardization_exception` table + vendor-extension routing catalog rows; `0106` reserved (coverage view registration / catalog rows). All `CREATE … IF NOT EXISTS`; never edit or renumber a landed migration; timestamped DB+WAL backup before any live apply.
- **(C)** Tests run on in-memory / template-copy DuckDB with fixture filings: one filer tagging revenue via ASC-606, one via legacy `Revenues`, one via a *custom extension* routed through `fundamental_item_vendor_map`, plus a summed item (`total_debt`). No SEC/network in pytest.
- **(D)** `compute_standardized_rows` is pure; the rule evaluator is a **fixed closed dispatch table** (mirroring S4's `eval_operand_term` — no `eval`/`exec`). Every row records `input_codes_json` (the source concepts/metrics combined) and the `rule_id` applied. Same inputs + same params → same rows.
- **(E)** `fundamental_standardized` lands with a schema-contract row (columns/types/nullability/natural key/required PIT columns) and a `table_catalog` entry; the drift check must stay green.
- **Data posture.** Ship engine + `standardization_rules.csv` + offline fixtures, then an **operator-run ~1-year recent proof slice** with live counts recorded in the ledger. No historical backfill in this sprint.

---

## Tasks

### S3-0 — Standardization rule schema + engine + `fundamental_standardized` table *(the big one)*

**Root cause.** No engine applies the governed `sign_convention` / `unit_type` / `coalesce_priority` (and the map's `value_multiplier` / `normal_balance` / `concept_priority`) across companies. Comparability is accidental — whatever single concept a filer tagged.

**Fix.** NEW `db/seeds/standardization_rules.csv`: one row per canonical `item_id` from `fundamental_item`, carrying `source_aliases` (an ordered `alias_scheme`/`alias_code` list), a `combination_rule` (`coalesce_priority | sum | difference | first_non_null | identity`), a `sign_rule` (derived from `sign_convention` / `normal_balance`), a `scale_rule` (unit→`unit_type` via `value_multiplier`), and a `missing_policy` (`skip | zero_fill | derive`) — definition-as-data, mirroring `formula_registry`. NEW `db/standardization.py` holds the seed loader plus a pure `compute_standardized_rows(inputs) -> DataFrame` whose `combination_rule` dispatch is a fixed closed table (no `eval`), each branch unit-tested. `refresh_fundamental_standardized(store, options)` pivots the candidate facts from `fundamental_statement_points` + `fundamental_xbrl_metric` (reusing the `is_latest_revision` + `period_type` filter shape of `load_ratio_inputs`), applies the rules, and writes the long fact — same DELETE-by-source + `insert_frame` shape as `refresh_fundamental_ratios`. Migration **0103** creates `fundamental_standardized` (`security_id, item_id, period_end, basis` natural key; carries `value`, `input_codes_json`, `rule_id`, the bitemporal columns); **0104** the lookup index.

**PIT.** (B) 0103/0104 idempotent + catalogued; (D) pure transform, closed dispatch; (A) `available_at = max(inputs)`.

**Accept.** A fixture where two filers report revenue under different concepts and a third item (`total_debt`) is summed from `st_debt` + `lt_debt` yields identical standardized `item_id` values across filers; each `combination_rule` has a passing dispatch unit test; a fresh in-memory DuckDB migrates cleanly through `0104` and the contract/drift check is green.

### S3-1 — Custom-extension routing + unmapped-concept exception report + quality gate

**Root cause.** `normalize_xbrl_metric_rows` drops any concept absent from `CONCEPT_MAP`; custom extensions never reach a canonical item and leave no trace; `fundamental_item_vendor_map` is seeded but unused for routing.

**Fix.** In `db/standardization.py`, route each filer tag first through the rule's us-gaap aliases (`Registry.resolve_item`), then — for vendor/custom-extension tags — through `fundamental_item_vendor_map`; anything still unresolved lands in a NEW `fundamental_standardization_exception` table (migration **0105**, catalogued) recording the raw `concept`, `security_id`, `accession_number`, and a reason. Register a **gated** quality check (clause G / severity) on the per-slice exception rate so an unroutable long tail surfaces instead of silently shrinking coverage.

**PIT.** (B) 0105 catalogued. (C) fixture: one custom extension that routes via `fundamental_item_vendor_map`, one genuinely unmapped tag that lands in the exception report.

**Accept.** Every fixture filer tag either standardizes onto an `item_id` or appears in `fundamental_standardization_exception` — none silently dropped; the exception-rate check is green on the proof slice and red on a planted all-unmapped fixture.

### S3-2 — `fundamental_ratios.py` reads the standardized inputs (reconciliation-gated to pf1)

**Root cause.** `load_ratio_inputs` pivots raw `canonical_metric` literals straight off `fundamental_statement_points` / `fundamental_xbrl_metric`; those are as-reported, not the standardized template.

**Fix.** Add `fundamental_standardized` as an input source to `load_ratio_inputs`, pivoted by `item_id` into the *same* wide-frame keys (the `TTM_INPUTS` / `BALANCE_INPUTS` keys resolve to item ids via `ratio_input_item_ids`), leaving `_pivot_case`, `compute_ratio_rows`, and the `available_at = max(inputs)` computation untouched. Confine the edit to S3's region — **not** PF2-S4's vintage region. Gate the change behind a **reconciliation**: the 53+ ratio codes must rebuild equal to the pf1 golden snapshot (mirror the S4-1 byte-identity gate) *unless* a specific standardized item intentionally changes a value, in which case each delta is individually enumerated, explained, and fixture-tested.

**PIT.** (D) `compute_ratio_rows` stays pure; `ratio_id` / `available_at` unchanged for every reconciled row.

**Accept.** The ratio codes rebuild reconcilable to the captured pf1 golden; any intended delta is enumerated with a known-value fixture; `python -m pytest atx-impl\db\tests\test_fundamental_ratios.py -q` stays green.

### S3-3 — Standardized-template coverage report + gated coverage check

**Root cause.** Nothing measures how much of the ~300 annual / ~100 quarterly template a filing actually populates.

**Fix.** A coverage reader/view over `fundamental_standardized` (migration **0106** registers the view / catalog rows) reporting, per `security_id`×`period_end`×`basis`: populated item count vs the canonical ~300 annual / ~100 quarterly total, how many items came from `sum` / `coalesce_priority` / `first_non_null` vs a direct tag, and the exception count from S3-1. Register a **gated** coverage quality check (minimum template fill on the proof slice).

**PIT.** (A) coverage is computed as-of the standardized rows' `available_at`; (D) deterministic given the fact.

**Accept.** The coverage report returns populated/total per security-period on the proof slice; the coverage check fires below threshold on a sparse fixture and passes on a well-populated one.

---

## Sequencing & expected compounding

**S3-0 → S3-1 → S3-2 → S3-3.** S3-0 (rule schema + engine + `fundamental_standardized`) is load-bearing — every downstream task reads the template it builds. S3-1 (routing + exception report) makes the template *honest*: every filer tag is accounted for, not silently dropped. S3-2 (ratio rebuild) is the reconciliation-gated payoff and must follow S3-0 so there is a template to read. S3-3 (coverage) measures the result and turns it into a gated SLO. The compounding: once the warehouse holds one comparable per-item fact, **PF2-S4** vintages it (`as_first_reported` vs restated on `fundamental_standardized`), **PF2-S5** specializes the same rule format per industry profile (INDL vs FS), and **PF2-S9** joins the comparable surface for multiples + cross-vendor reconciliation. This is the sprint that converts the warehouse from "as-reported extraction" to "comparable standardized financials."

---

## Risks / guardrails

- **Silent ratio drift (central risk).** Reading standardized inputs must not quietly change a pf1 ratio value. Mitigate with the S3-2 reconciliation gate (S4-1 byte-identity model): no value moves without an enumerated, tested, documented reason.
- **Over-combination fabricates data.** A too-eager `sum` / `coalesce_priority` can invent a value a filer never reported. Keep `missing_policy` explicit — a documented gap (exception report + coverage shortfall) is always better than a fabricated item.
- **Rule grammar as an eval hole.** Mirror S4: a fixed, closed `combination_rule` dispatch table, unit-tested per rule; never `eval`/`exec`.
- **Blast radius / scope.** Do not edit the pf1-S1 item dimension or PF2-S4's ratio-vintage region; stay strictly within migrations **0103–0106**; split schema vs index across numbers per the S5g/S5k WAL precedent; preserve a timestamped DB+WAL backup before any live apply.

---

## Bench / acceptance

- `db/seeds/standardization_rules.csv` covers the ~300 annual / ~100 quarterly template `item_id`s; every rule row carries a `combination_rule` + `sign_rule` + `scale_rule` + `missing_policy` and a known-value unit fixture.
- Every fixture filer tag either standardizes onto an `item_id` or lands in `fundamental_standardization_exception` — none silently dropped.
- The 53+ ratio codes rebuild **reconcilable to the pf1 golden**; any intended delta enumerated + fixture-tested.
- Coverage report returns populated/total per security-period; the gated coverage + exception-rate checks are green on the proof slice, red on sparse / all-unmapped fixtures.
- `python -m pytest atx-impl\db\tests\test_standardization.py -q` green (and the full `atx-impl\db\tests -q` suite green before commit).
- **Live smoke** on the operator-run ~1yr recent proof slice, recorded in the ledger: `fundamental_standardized` row count, template-coverage %, exception count, and `run_id`.
- `PARITY_GAP.md` status updated (the #1 standardization gap closed on the proof slice); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, migrations `0103–0106`, template item count, verification commands, live-DB smoke with exact counts + `run_id`, caveats/next).

**Process:** never `git add -A` (stage explicit paths); never push unless asked. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
