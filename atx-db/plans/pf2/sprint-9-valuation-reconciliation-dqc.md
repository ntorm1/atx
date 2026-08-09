# Sprint PF2-S9 — Populated valuation multiples + cross-vendor reconciliation + DQC-196

**Goal:** turn pf1-S6's empty `valuation_multiples` scaffold into a real PIT-safe surface by loading a ~1-year recent price×fundamental overlap slice; add a `fact_disagreement` cross-vendor reconciliation (warehouse XBRL/standardized vs an injectable Sharadar SF1 / SimFin baseline) with a >99% agreement gate; and extend pf1-S7's DQC *subset* toward the ~196 approved XBRL-US rules. Reserved migrations 0124–0127.

**Mandate / Owns:** `db/valuation_multiples.py` (population pass over the scaffold pf1-S6 created), NEW `db/fact_disagreement.py`, a `db/xbrl_validation.py` DQC extension (additive `rule_family`/`rule_code` rows), `db/tests/test_valuation_multiples.py`, `db/tests/test_fact_disagreement.py`.

**Must NOT touch:** the `fundamental_ratios.py` core / `formula_registry` internals (PF2-S3/S4 own the standardized-input + vintage regions — consume them), the pf1-S6 `pricing_bulk.py` / `disambiguate_vendor_collisions` repair (reuse, never re-key), and pf1-S7's `absolute_tolerance = 1.0` (never loosen to make a DQC/calc row pass). This sprint populates and reconciles; it does not rewrite the derivation or the linkbase check.

**Depends on:** pf1-S6 (the `valuation_multiples` / `market_cap` scaffold + injectable `pricing_bulk.py` bar loader), pf1-S5 (identifier spine — the `security_identifier_history` `TICKER` crosswalk that lets a broad-price line meet a fundamental line), PF2-S3 (standardized facts, the reconciliation subject), PF2-S6 (calendarized TTM denominators), pf1-S7 (the DQC subset this extends). Runs **after PF2-S3/S4** because it reads `fundamental_ratios.py`'s standardized/vintaged inputs.

---

## Baseline / where the cycles go

The three most-cited residual gaps in fact #7 of the ROADMAP all live here; each is concrete and measured.

1. **`valuation_multiples` is a scaffold emitting ~0 live rows — a window mismatch, not missing math.** `equity_daily_bars` broad price is a **2012-03→2015-02 sample (~4.8M rows / 9,118 securities)**; `sec_company_facts` fundamentals are **2017–2026**. Every P/E, P/B, P/S, EV/EBITDA numerator therefore has no `(security_id, trade_date)` price to pair with a fundamental period, so the PIT join returns ~0 rows. `shares_outstanding_history` already holds the PIT share leg (`share_count`, `share_count_type ∈ {shares_outstanding, shares_diluted_avg}`, `effective_date`, `available_at`, `is_latest_revision`) over 2017–2026 — only the *price* leg is missing over the fundamentals window. Resolving this is S9-0 and is the sprint's center of mass.
2. **The join key is not bare `security_id` equality.** pf1-S39's `disambiguate_vendor_collisions` keyed broad-price lines by `security_identifier_history.id_type = 'TBLTICKERHISTORY_SECURITY_ID'` (and unambiguous tickers to `CIK`), while fundamentals key off SEC CIK. A price line meets a fundamental line only through the `security_identifier_history` `TICKER` crosswalk (`ON h.id_type = 'TICKER'`, the same edge `security_master.py` resolves on) — a naive `equity_daily_bars.security_id = shares_outstanding_history.security_id` join silently drops the modern universe.
3. **No cross-vendor reconciliation exists.** There is no `fact_disagreement` table. The warehouse's XBRL/standardized facts have never been checked like-for-like against an independent vendor baseline, so a mis-standardized sign/scale (the #1 PF2-S3 risk) has no external tripwire. The canonical hard case is the documented **Compustat-vs-IBES EPS divergence** (GAAP vs street basis; restated vs as-first-reported vintage) — a reconciliation that compares the wrong basis or vintage manufactures false disagreements.
4. **DQC coverage is a thin subset.** pf1-S7 ships only sign / member-on-wrong-axis / non-negative-concept checks written to `xbrl_validation_results` under new `rule_family` values (the calc-linkbase family stays `rule_family='calculation_linkbase'`, `rule_code='calc_sum_parent_equals_weighted_children'`). The XBRL-US Data Quality Committee library is **~196 approved rules (plugin v30.0.0, June 2026; v29 = 185)** — families DQC_0004 (Assets = Liabilities + Equity), DQC_0015/DQC_0080 (negative-value US-GAAP/IFRS), DQC_0018/DQC_0135 (deprecated / extensible-enumeration element selection), DQC_0041/DQC_0104 (dimensional/axis-member validity), DQC_0043–0062 (cash-flow), DQC_0118 (financial-statement-tables calculation). Most of that is unported.

**Already good — do not regress:**
- **The honest deferral discipline.** pf1-S7 never buried the 1,364 calc failures by widening `absolute_tolerance` (default `1.0`). New DQC families are *additive* rows; they do not touch the tolerance or the calc-linkbase family.
- **The pf1-S6 PIT rule.** `multiple.available_at = max(price.available_at, fundamental.available_at)`; `equity_daily_bars` stamps `available_at = trade_date + 22h`; `market_cap` uses raw `close` × then-outstanding shares (a level struck on the day), not `adjusted_close`. The population pass consumes these — it never overwrites a leg's `available_at`.
- **The `disambiguate_vendor_collisions` repair + `security_ids_for_symbols` resolver.** Global, idempotent, the system of record for recycled-ticker / share-class splits. Reuse; do not copy the body.

---

## PIT / determinism + production contract

ROADMAP clauses **(A)** bitemporal correctness, **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, **(D)** determinism+provenance apply in full; the pf2 **~1-year recent proof-slice** posture governs the data-dependent surfaces.

- **(A)** Every emitted multiple sets **per-fact `available_at = max(price.available_at, fundamental.available_at)`** and picks the `shares_outstanding_history` revision that is the latest `is_latest_revision = TRUE` row with `available_at ≤ price.available_at` — a share count filed *after* the price date is never selected (no lookahead). `as_of_date` = the price `trade_date`. `fact_disagreement` rows carry the `available_at` of the warehouse fact and the vendor baseline's own as-of; agreement is evaluated only where both are knowable.
- **(B)** Migrations **0124–0127** only, each seeding `table_catalog` + `field_catalog` in the same migration, split schema-vs-index per the S5g/S5k WAL precedent. Never edit a landed migration or renumber.
- **(C)** All tests run against in-memory / template-copy DuckDB with a small fixture price panel + fixture fundamental panel + an **injected offline vendor baseline file** (a Sharadar SF1 / SimFin CSV the operator supplies behind `--vendor-baseline-file`). No SEC / Nasdaq / vendor network in pytest; the live overlap slice + live reconciliation are operator-run and recorded in the ledger.
- **(D)** `compute_*` transforms stay pure (pandas in → long DataFrame out), unit-tested independent of DuckDB; every row records `input_codes_json` + source lineage. Same inputs + params → same rows and the same agreement verdict.

---

## Tasks

### S9-0 — Populate valuation multiples over a ~1yr price×fundamental overlap slice *(the big one)*

**Root cause:** pf1-S6 built the math (`valuation_multiples.py`, the `market_cap` + `valuation_multiples` tables, the nine-member family) but there is no overlapping price, so the join is empty. The 2012–2015 broad sample never meets the 2017–2026 fundamentals.

**Fix:** run pf1-S6's injectable `pricing_bulk.py` loader on an operator-supplied **~1-year recent** OHLCV archive (default filter `trade_date ≥ overlap_start`), landing into `equity_daily_bars` with a distinct `source` and `available_at = trade_date + 22h`, then **reuse** `disambiguate_vendor_collisions(store, source)`. In `valuation_multiples.py`, join price→fundamental **through the `security_identifier_history` `TICKER` crosswalk** (not bare `security_id` equality), compute `market_cap = close × PIT share_count`, and emit the full family — `price_to_earnings`, `price_to_book`, `price_to_sales`, `enterprise_value`, `ev_to_ebitda`, `ev_to_sales`, `fcf_yield`, `earnings_yield`, `dividend_yield` — reading TTM denominators from PF2-S6's calendarized inputs and debt/cash from the item-dimension metric. `is_meaningful = FALSE` (never dropped) on non-positive denominators. Migration **0124**: an overlap-slice provenance table (`valuation_overlap_slice`: source file, security count, date span, `run_id`) + lookup indexes, catalogued — no delta to the pf1-S6 `market_cap` / `valuation_multiples` schema.

**PIT:** each multiple's `available_at = max(price.available_at, fundamental.available_at)`; the share revision is the latest knowable at `price.available_at`; `as_of_date = trade_date`.

**Accept:** on a fixture overlap panel (2 securities × a handful of shared dates, known EPS/BVPS/sales/debt/cash) every family member computes to expected value; `market_cap = close × share_count`; a share revision filed after the price date is not selected; a negative-EPS security emits `price_to_earnings` with `is_meaningful = FALSE`; the crosswalk join yields rows where bare `security_id` equality yields zero. Live smoke: state real row counts + coverage over the operator overlap slice.

### S9-1 — `fact_disagreement` cross-vendor reconciliation

**Root cause:** no external tripwire on the standardized facts. A mis-standardized sign/scale/basis passes silently because nothing compares the warehouse to an independent vendor.

**Fix:** NEW `db/fact_disagreement.py` loads an **injectable offline** Sharadar SF1 / SimFin baseline (`--vendor-baseline-file`, never network) into a staging table, aligns each vendor line to a warehouse standardized fact on `(cik/security_id, fiscal_period, canonical item)` **like-for-like on basis and vintage** — GAAP-standardized vs as-first-reported vs most-recently-restated must be matched, per the Compustat-vs-IBES EPS lesson — and writes one `fact_disagreement` row per compared fact (warehouse value, vendor value, relative diff, `basis`, `vintage`, `agree` boolean, `available_at`, `run_id`). Emit a `quality_check` computing agreement = agreeing / compared, **gated at >99%**. Migration **0125**: `fact_disagreement` + `vendor_baseline_facts` staging tables + catalog rows.

**PIT:** (A) compare only where warehouse `available_at ≤ vendor as-of` (or vice-versa) so no vintage crosses knowledge cuts. (C) fixtures inject a matching baseline and one planted divergence (a GAAP-vs-street EPS mismatch that must be classed a *basis* difference, not a warehouse error). (D) verdict deterministic.

**Accept:** the reconciliation runs offline against an injected baseline; agreement ≥ 99% on a clean fixture (gate green) and < 99% on a fixture with a planted real disagreement (gate red); a GAAP-vs-street EPS pair is tagged a basis difference, not counted as disagreement.

### S9-2 — DQC rule expansion toward ~196 (documented ported-vs-skipped)

**Root cause:** pf1-S7 ships only a small SQL-expressible DQC subset; sign/axis/cash-flow rules that are *also* SQL-expressible are unported, so real filing errors pass.

**Fix:** in `db/xbrl_validation.py`, port additional SQL-expressible DQC families as **new `rule_family`/`rule_code` rows appended to `xbrl_validation_results`** (mirroring `refresh_xbrl_validation_results`' `DELETE WHERE rule_family = <family>` + re-INSERT idempotency, deterministic `validation_id`): DQC_0004 (Assets = Liabilities + Equity foots), DQC_0015/DQC_0080 (negative-value US-GAAP/IFRS), DQC_0018 (deprecated element in use), DQC_0041/DQC_0104 (axis-member validity via `xbrl_filing_dimensions` + `xbrl_taxonomy_relationships`), DQC_0043–0062 (cash-flow sign/classification). Document — inline and in this file — exactly which DQC_nnnn were ported and which were **skipped** (those needing full Arelle/XULE calculation semantics beyond SQL, e.g. DQC_0018 extensible-enumeration DQC_0135, DQC_0118 financial-statement-tables calc). Migration **0126**: DQC concept-list / axis-membership catalog seed rows. **Do NOT loosen pf1's `absolute_tolerance`.**

**PIT:** (C) each ported rule gets a passing and a failing fixture. (D) pure SQL over loaded facts → deterministic rows.

**Accept:** the expanded DQC families run and write rows under their new `rule_family` values; each ported rule has a passing + a failing fixture; the ported-vs-skipped rationale is written down; the calc-linkbase family and its tolerance are untouched.

### S9-3 — Valuation + reconciliation coverage report + gated quality check

**Root cause:** with the surfaces populated there is still no single honest report of *how much* of the universe has multiples, what agrees cross-vendor, and how wide the DQC coverage is.

**Fix:** emit a coverage report + `quality_check` over the overlap slice: `(securities with ≥1 valuation multiple) / (securities with ≥1 fundamental over the window)`, the overlap date span, the `fact_disagreement` agreement ratio (the >99% gate), and the DQC ported-family count. Wire the counts into the existing `DatasetLoadResult` details so the orchestrator (PF2-S10) and `PARITY_GAP.md` surface real coverage, not a vacuous claim. Migration **0127**: reserved split (report/index + any `resolution`/coverage columns, catalogued) per the S5g/S5k WAL precedent.

**PIT:** the coverage query respects `available_at` (counts only knowable rows) — reported coverage is knowable coverage, not lookahead-inflated.

**Accept:** the coverage `quality_check` row is written with overlap security count, fundamental count, ratio, date span, agreement ratio, and DQC family count; state exact counts in the ledger; the >99% agreement gate is a first-class check.

---

## Sequencing & expected compounding

**S9-0 → S9-1 → S9-2 → S9-3.** S9-0 (load overlap + populate) is load-bearing: with no overlapping price there is no multiple, no market cap, and reconciliation has fewer facts to check. S9-1 (cross-vendor recon) then earns external trust in the standardized facts the multiples divide into. S9-2 (DQC expansion) widens the internal-consistency net independently but shares the `xbrl_validation.py` surface, so it slots third. S9-3 reports the outcome of all three. The compounding: overlap → real multiples → an external >99% tripwire on the standardized inputs → a materially wider DQC net → one honest coverage number. The warehouse goes from "~0 valuation rows, no external check" to "populated PIT-safe multiples, cross-vendor-reconciled, DQC-hardened."

---

## Risks / guardrails

- **Crosswalk mis-join (primary).** Joining broad price to fundamentals on bare `security_id` silently drops the modern universe or, worse, mismatches a recycled ticker. **Mitigate:** always route through the `security_identifier_history` `TICKER` crosswalk and the reused `disambiguate_vendor_collisions`; a fixture proves crosswalk-join rows where equality-join yields zero.
- **Look-ahead by stale-price / late-share valuation.** Selecting a share revision filed after the price date, or valuing against a price stamped after the fundamental, leaks the future. **Mitigate:** the strict `available_at = max(...)` rule + a dedicated PIT test.
- **False cross-vendor disagreements from basis/vintage mismatch.** Comparing GAAP-standardized to street EPS, or as-first-reported to restated, manufactures divergences. **Mitigate:** reconcile like-for-like on `basis` + `vintage`; the Compustat-vs-IBES EPS pair is the canonical fixture.
- **Never loosen tolerance.** `absolute_tolerance` ends the sprint at `1.0`; DQC rows are additive `rule_family` values, never a widened band on the calc-linkbase family.
- **Offline / injectable only.** No SEC / Nasdaq / vendor network in pytest; the vendor baseline and the overlap archive are operator-supplied files; live runs are ledger-recorded smoke.
- **Migration/WAL safety.** Split schema vs index across 0124–0127; preserve a timestamped DB+WAL backup before any live apply; stay strictly within the reserved range.

---

## Bench / acceptance

- Valuation multiples emit **real rows** over the ~1yr overlap slice with correct PIT `available_at = max(price_av, fundamental_av)`; the full nine-member family computes to expected values on the fixture panel; `is_meaningful = FALSE` on non-positive denominators.
- `fact_disagreement` runs offline against an injected Sharadar/SimFin baseline; the **>99% agreement gate** is green on a clean fixture and red on a planted-divergence fixture; basis/vintage matched like-for-like.
- The expanded **DQC families run** and write additive `rule_family`/`rule_code` rows to `xbrl_validation_results`; ported-vs-skipped rationale documented; `absolute_tolerance` stays `1.0`.
- `python -m pytest atx-impl\db\tests\test_valuation_multiples.py atx-impl\db\tests\test_fact_disagreement.py -q` green, and the full `python -m pytest atx-impl\db\tests -q` suite stays green before commit.
- **Live-DB smoke** on the overlap slice recorded in the ledger: bars landed, `market_cap` rows, multiple rows per family, overlap coverage ratio + date span, cross-vendor agreement ratio, DQC ported-family count, with exact counts + `run_id`.
- `PARITY_GAP.md` status updated (valuation/reconciliation/DQC gaps); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live-DB smoke with exact counts + run_id, caveats/next).

**Process:** never `git add -A` (stage explicit paths — the tree carries unrelated dirty/untracked files); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
