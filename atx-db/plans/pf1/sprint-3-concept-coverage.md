# Sprint PF-S3 — XBRL Concept Coverage → Full Canonical Set

**Goal:** widen concept extraction from ~200 to the full canonical dictionary + bank/insurance/REIT
overlays; lift the 5-security xbrl-extra ceiling; reconcile statement-map gaps. Reserved migrations
`0069–0074`.

**Mandate / Owns:** `db/fundamental_statements.py` (concept catalog + statement map),
`db/fundamentals.py` `DEFAULT_CONCEPTS`, NEW `db/seeds/concept_map.csv`,
`db/tests/test_concept_coverage.py`.

**Must NOT touch:** the ratio engine internals (`db/fundamental_ratios.py` — PF-S4); `item_registry`
(PF-S1 — depend on it, don't edit). PF-S1 owns the `fundamental_item` dimension + aliases; PF-S3
maps concepts *to* those `item_id`s and never mutates the registry.

**Depends on:** PF-S1 (new concepts must resolve to `item_id`s through the registry) and PF-S2 (the
wide backfill that materialises the new concepts runs incremental / resumable through the
orchestrator, not a hand-run loop).

---

## Baseline / where the cycles go (audited this session)

The fundamentals feed is at Compustat *breadth* on the concepts it carries but not at *coverage* on
the concepts it could carry. Four measured walls, all in the two owned modules:

| Wall | Location | State | Class |
|---|---|---|---|
| **Extraction concept set is a hand-curated ~200-concept list** | `fundamentals.py:28` `DEFAULT_CONCEPTS` (31 concepts after S44) → `fundamental_statement_map` (`fundamental_statements.py:155`, 137 `ALL`-template rows) | `DEFAULT_CONCEPTS` is the fetch filter; only concepts in it land in `sec_company_facts`, so the map's own breadth is never fully exercised. The `_statement_category` bucketer (`fundamental_statements.py:431`) is substring-based, not dictionary-driven. | Coverage ceiling |
| **37 overlay rows are a standing allowlist failure** | `fundamental_statements.py:428–467` (bank 1501–1515 = 15, insurance 1601–1610 = 10, REIT 1701–1712 = 12) | Every overlay row is `vendor-only` / `extension` / `nareit` taxonomy or `__VENDOR_ONLY__` / `__EXTENSION__` sentinel with `is_active = False`. They exist so the templates are *declared*, but they trip `bad_fundamental_statement_map_rows` (`quality.py:1710`, threshold 0.0) and `loaded_xbrl_concepts_without_statement_map` (`quality.py:1739`) as documented, tolerated exceptions. | Standing quality failure |
| **xbrl_metric universe (5 securities) << ratio universe (1,395)** | `fundamental_xbrl_metrics.py` `_INSTANT_SQL`/`_DURATION_SQL` read `xbrl_filing_facts` (cached inline XBRL, ~1,000 concepts but a handful of securities); `_COMPANYFACTS_*_SQL` (S44) lift the *same* `CONCEPT_MAP` to the ~1,400-security `sec_company_facts` universe | The balance/flow-concept extractor's inline-XBRL path is capped at the ~5 securities whose primary-document facts are cached offline. The companyfacts path (S44) reaches the full universe but only for the narrow `CONCEPT_MAP` (`fundamental_xbrl_metrics.py:39–72`), not the full canonical set. | Universe gap |
| **IFRS is excluded — correctly** | `fundamentals.py:71` `SUPPORTED_FACT_TAXONOMIES = ("us-gaap", "dei")` | `ifrs-full` foreign private issuers have no us-gaap map and collide on canonical names (ifrs-full `Assets` vs us-gaap `Assets`), so they are dropped at load. This is a *deliberate* boundary, not a gap. | Intentional boundary |

**Scout finding (grounding).** Concept coverage plateaued at **~200** distinct XBRL metrics in
`xbrl_concept_catalog`; the xbrl-extra (inline-XBRL) codes are capped at **5 securities** by the
offline `xbrl_filing_facts` cache. The S44 precedent widened `DEFAULT_CONCEPTS` from **16 → 31**
concepts and lifted the S10 ratio families to the broad companyfacts universe — PF-S3 is the
generalisation of that move to the *full* canonical dictionary.

**Already good — do not regress:**

- us-gaap / dei concept mapping and the 3-way COALESCE priority chains (revenue ASC-606 → legacy →
  SalesRevenueNet, ST-debt components, share-count dei-then-us-gaap) — `fundamental_statements.py`.
- the four-date period model (`period_start ≤ period_end ≤ filed_date`, `available_at = filed +
  22h`) enforced in `normalize_companyfacts` (`fundamentals.py:350`).
- the statement-map overlays for bank / insurance / REIT templates and the `industry_template`
  keying (`source, taxonomy, concept, industry_template`) — they are the scaffold PF-S3 completes,
  not replaces.

---

## PIT / determinism contract (S3)

Applies clauses **(A)(B)(C)** of the ROADMAP shared contract. Specifically:

- **(A) Bitemporal correctness.** Every newly-extracted concept row carries the same
  `available_at = filed_date + 22h` knowledge-time stamp and `run_id` as today's rows; widening the
  concept set adds *rows*, never changes the availability semantics. No new concept may be visible
  before its filing's `available_at`.
- **(B) Append-only, catalogued migrations.** PF-S3 uses only reserved range `0069–0074`; each new
  table/view (coverage report, unmapped-item report) seeds `table_catalog` + `field_catalog` in the
  same migration. Statement-map *rows* are code-defined constants re-materialised by the existing
  refresh, not a migration; the seed CSV is data, not DDL.
- **(C) Offline / no-network tests.** The companyfacts **re-fetch is OPERATOR-RUN** (network) — it
  is documented as a step, never invoked in pytest. The two offline deliverables are (1) the loader
  change to `DEFAULT_CONCEPTS` and (2) the `concept_map.csv` seed; tests exercise both against
  in-memory DuckDB with fixtures only. No SEC calls in the test path.

The concept-set widening is inert until the operator re-runs the companyfacts backfill with the new
`DEFAULT_CONCEPTS`; the loader change and the seed are what land in-repo and under test.

---

## Tasks

### S3-0 — Expand `DEFAULT_CONCEPTS` to the full canonical us-gaap set

**Root cause:** `DEFAULT_CONCEPTS` (`fundamentals.py:28`) is a 31-entry hand-list. Because
`normalize_companyfacts` filters facts to `concept in concepts` (`fundamentals.py:367`), the
`fundamental_statement_map`'s own 137 `ALL`-template concepts are never fully fetched — the map
declares more canonical inputs (SG&A, R&D detail, PP&E gross, intangibles, deferred tax, treasury
stock, APIC, ...) than the fetch filter admits. Coverage is bounded by the *smaller* of the two
lists, and today that is the fetch list.

**Fix:**
1. Derive the full us-gaap concept set from PF-S1's item-dim aliases (income / balance / cashflow)
   — the union of every `taxonomy='us-gaap'`/`dei` concept the `fundamental_item` registry maps to
   an `item_id`. Materialise it as `DEFAULT_CONCEPTS` (or a `CANONICAL_CONCEPTS` frozenset the
   default derives from) so the fetch filter and the statement map draw from one source.
2. Emit `db/seeds/concept_map.csv` — one row per `(taxonomy, concept, canonical_metric, item_id,
   statement_type, industry_template)` — as the offline, reviewable projection of the map. This is
   the seed PF-S4/PF-S8 read; it is data, not DDL.
3. Keep IFRS excluded: do not add `ifrs-full` to `SUPPORTED_FACT_TAXONOMIES`; the canonical set is
   us-gaap + dei only.
4. Document the operator re-fetch: after this lands, the operator re-runs the companyfacts backfill
   (`symbol_source='loaded_facts'`, `companyfacts_zip=...`) so the wider concept set is extracted
   over the already-loaded universe. Record it in the ledger as an operator smoke, not a test.

**PIT:** (A) unchanged availability semantics; (C) the seed + loader change are offline, the
re-fetch is operator-run.

**Accept:** `DEFAULT_CONCEPTS` covers every `ALL`-template active concept in the statement map (no
map concept is unfetchable); `concept_map.csv` round-trips (every row's `item_id` resolves in the
PF-S1 registry, every `canonical_metric` matches the map); IFRS still absent from the taxonomy
allowlist.

---

### S3-1 — Statement-map completion + overlay reconciliation

**Root cause:** two standing quality failures. (1) `loaded_xbrl_concepts_without_statement_map`
(`quality.py:1739`) fires for any newly-fetched concept with no active map row — widening S3-0's
fetch set will surface *more* such concepts unless each is mapped. (2) The 37 overlay rows
(`fundamental_statements.py:428–467`) carry `vendor-only` / `extension` / `nareit` taxonomies and
`__VENDOR_ONLY__` / `__EXTENSION__` sentinel concepts with `is_active=False`; they are the
documented `bad_fundamental_statement_map_rows` allowlist entries.

**Fix:**
1. Map every concept newly admitted by S3-0 to `statement_type` / `statement_section` / parent
   `item_id` in `FUNDAMENTAL_STATEMENT_MAP_ROWS`, so no fetched us-gaap/dei concept is left
   unmapped. Respect the existing COALESCE-priority convention (`concept_priority`) for alias chains.
2. Add real overlay concepts where a verified us-gaap tag exists — promoting them off the
   `vendor-only` sentinel to an `is_active=True` us-gaap row:
   - **bank:** interest income (`InterestAndDividendIncomeOperating`), loan-loss reserves /
     provision (`ProvisionForLoanAndLeaseLosses`, and any allowance concept confirmed against the
     catalog), total loans (`LoansAndLeasesReceivableNetReportedAmount`).
   - **insurance:** premiums (`PremiumsEarnedNet`), loss reserves / unpaid-claim liability, combined
     ratio (kept derived: `combined_ratio = loss_ratio + expense_ratio`).
   - **REIT:** FFO (`nareit:FundsFromOperations`), AFFO (extension), NOI (Nareit definition, kept as
     extension/derived per §2.7).
3. Reconcile the allowlist: any overlay row that *stays* `vendor-only` / `extension` (no verified
   us-gaap concept) must be explicitly enumerated as a permitted exception the coverage report
   accounts for — not a silent tolerance. Rows that gain a real us-gaap concept leave the allowlist.

**PIT:** (B) map rows are code constants re-materialised by the existing refresh; overlays keyed on
`industry_template` (BK/IS/RT) so they never collide with the `ALL` rows.

**Accept:** `loaded_xbrl_concepts_without_statement_map` = 0 for the S3-0 fetch set;
`bad_fundamental_statement_map_rows` reconciled to an *explained* allowlist (each remaining entry
enumerated with reason); bank/insurance/REIT overlays present and, where a verified us-gaap concept
exists, `is_active=True`.

---

### S3-2 — Lift the 5-security xbrl_metric ceiling

**Root cause:** `fundamental_xbrl_metrics.py`'s inline-XBRL path (`_INSTANT_SQL` /
`_DURATION_SQL`) reads `xbrl_filing_facts`, which is populated offline for only ~5 securities, so
the `xbrl_metric` universe is capped there. The S44 companyfacts path (`_COMPANYFACTS_*_SQL`)
already reaches the ~1,400-security universe but only for the narrow `CONCEPT_MAP`
(`fundamental_xbrl_metrics.py:39–72`), so balance-concept coverage across the full universe is
limited to that subset.

**Fix:**
1. Widen the balance/flow `CONCEPT_MAP` used by the **companyfacts** extractor to the full canonical
   set from S3-0 (instant balance concepts and annual-window duration flow concepts), so the
   `xbrl_metric` universe over `sec_company_facts` matches the fundamentals universe rather than the
   inline-XBRL cache. Preserve the consolidated-total selection (companyfacts values are already
   entity-level; instants carry NULL `period_start`) and the 350–380-day annual-window filter for
   durations.
2. Leave the inline-XBRL path unchanged for the securities it does cover (it stays the higher-fidelity
   dimensioned source); vintage dedup already reconciles overlap under one source.
3. Document precisely what stays **network-gated**: the inline-XBRL substrate (`xbrl_filing_facts`)
   is only as wide as the operator's cached primary-document downloads; growing *that* universe is an
   operator fetch, out of scope for the offline change. The companyfacts widening is the offline lever.

**PIT:** (A) `available_at` from `acceptance_datetime`/`filing_date` unchanged; (C) offline SQL
change; universe growth via companyfacts is operator-run backfill, tested on fixtures.

**Accept:** the companyfacts-derived `xbrl_metric` universe (distinct `security_id`) **≥** the ratio
universe (`fundamental_ratios` distinct securities, ~1,395); the inline-XBRL cap is documented as
network-gated, not silently tolerated.

---

### S3-3 — Coverage report + quality check

**Root cause:** there is no single artifact that answers "what fraction of the canonical dictionary
do we actually extract?" Coverage today is inferred from two separate quality checks, and the
xbrl_metric-vs-ratio gap is only visible by hand-comparing universes.

**Fix:**
1. Add a coverage view/table (reserved migration in `0069–0074`, catalogued): **% of item-dim items
   with ≥1 extracted concept** (join PF-S1 `fundamental_item` → `xbrl_concept_catalog` via the map),
   broken down by statement_type and industry_template.
2. Add an **unmapped-item report**: item-dim items with zero mapped active concept, and fetched
   concepts with no active map row — the union that must gate to empty (excluding the enumerated
   overlay allowlist).
3. Add an **xbrl_metric-vs-ratio universe gap** check: distinct securities in the xbrl_metric
   universe vs the ratio universe; assert ≥.
4. Wire a `SqlQualityCheck` for the unmapped report so a future unmapped concept re-trips as a
   detected failure, not a silent one.

**PIT:** (B) new view/table catalogued in its migration; (D) the report is a pure projection, same
inputs → same rows.

**Accept:** coverage report emits a single % against the canonical target; unmapped report = 0
(modulo the enumerated overlay allowlist); universe-gap check green.

---

## Sequencing & expected compounding

1. **S3-0 first** — it defines the canonical concept set; everything else maps against it.
2. **S3-1** — completes the statement map for that set and reconciles the overlays.
3. **S3-2** — lifts the extraction ceiling so the wider concepts actually materialise across the
   full universe.
4. **S3-3 last** — the report + gate can only be meaningful once the set, the map, and the backfill
   are in place.

**Compounding.** PF-S3 **unblocks PF-S4** (new ratios — DuPont, coverage, accruals, per-share suite —
need the new concepts as inputs) and **PF-S8** (quarterly-TTM stitching needs the wider coverage to
stitch trailing windows). It consumes **PF-S1** (item dim + aliases define the canonical set) and
**PF-S2** (the wide re-backfill runs incremental/resumable, so re-extracting the full universe is a
watermark-driven job, not a from-scratch loop).

---

## Risks / guardrails

| Risk | Mitigation |
|---|---|
| **A new S3-0 concept has no map row** → it reappears as `bad_fundamental_statement_map_rows` / `loaded_xbrl_concepts_without_statement_map`. | S3-3's unmapped-item report **gates** the sprint: it must be 0 (modulo the enumerated overlay allowlist) before commit. S3-1 maps every S3-0 concept in the same change. |
| **IFRS leaks in.** Adding `ifrs-full` to widen coverage would collide on canonical names (ifrs-full `Assets` vs us-gaap `Assets`). | Keep `SUPPORTED_FACT_TAXONOMIES = ("us-gaap", "dei")` untouched; the canonical set is us-gaap + dei only. Assert IFRS absence in a test. |
| **Re-fetch treated as a code step.** The companyfacts re-fetch is network. | It is **operator-run**, documented in the ledger with exact counts + `run_id`, and **never** in the pytest path (clause C). Tests use fixtures. |
| **Overlay allowlist quietly grows.** Tolerating unmapped overlays hides real gaps. | Each remaining `vendor-only`/`extension` overlay row is **enumerated** with a reason in S3-1; the coverage report accounts for exactly that set, so a new unexplained entry fails the gate. |
| **xbrl_metric widening double-counts vs inline-XBRL.** | The existing vintage-dedup under one source reconciles companyfacts vs inline-XBRL overlap; verify with a fixture that has both. |

---

## Bench / acceptance

- **Concept coverage ≥ canonical target** — S3-3 report emits the % of item-dim items with ≥1
  extracted concept; record the number.
- **xbrl_metric universe ≥ ratio universe** — distinct securities in the companyfacts-derived
  xbrl_metric universe ≥ the ratio universe (~1,395); the inline-XBRL 5-security cap documented as
  network-gated.
- **Statement-map allowlist reconciled/explained** — `bad_fundamental_statement_map_rows` and
  `loaded_xbrl_concepts_without_statement_map` reduced to an enumerated, reasoned exception set;
  bank/insurance/REIT overlays present.
- **Tests green:**
  `python -m pytest atx-impl\db\tests\test_concept_coverage.py atx-impl\db\tests\test_fundamental_concept_dictionary.py -q`
- **Ledger row appended** to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains, verification
  commands, operator re-fetch smoke with exact counts + `run_id`, caveats/next); `PARITY_GAP.md`
  status updated.

**Process:** never `git add -A` (stage explicit paths); never push unless asked; new module ⇒ new
`test_*.py`; `python -m pytest atx-impl\db\tests -q` green before commit. Commit trailer EXACTLY:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## Out of scope

Ratio-engine internals and the formula library (PF-S4); the `item_registry` itself (PF-S1 — depend,
don't edit); dimension-aware XBRL calculation-linkbase validation and the 1,364 standing linkbase
fails (PF-S7); identifier spine (PF-S5); valuation multiples (PF-S6); restatement lineage columns
(PF-S8). Growing the inline-XBRL `xbrl_filing_facts` substrate beyond its cached securities is an
operator fetch, not an offline PF-S3 change. IFRS remains excluded.
