# Sprint PF2-S7 — Segment data + footnote sub-ledgers

**Goal:** mine the ~14k **dimensional** facts pf1 deliberately discards — build normalized business/geographic/product/customer **segment** surfaces and pension/OPEB, deferred-tax, lease, and SBC **footnote sub-ledgers** from `xbrl_filing_dimensions`, each with a reconciliation-to-consolidated guard (tolerance, not equality). Reserved migrations 0117–0120.

**Mandate / Owns:** NEW `db/segments.py` (segment extraction + reconciliation), NEW `db/footnotes.py` (footnote sub-ledger normalization), the `segment_dim` / `segment_fact` and `footnote_pension` / `footnote_deferred_tax` / `footnote_lease` / `footnote_sbc` tables, `db/tests/test_segments.py`, `db/tests/test_footnotes.py`.

**Must NOT touch:** the consolidated-only extractor `fundamental_xbrl_metrics.py` (its `dimension_count = 0` rule stays — segments/footnotes are the *complement* set, a NEW surface, not a rewrite), the pf1 XBRL loader `xbrl_filing_contexts.py`, `fundamental_ratios.py`, PF2-S3's `standardization.py`. This sprint **reads** `xbrl_filing_facts` / `xbrl_filing_contexts` / `xbrl_filing_dimensions` / `xbrl_taxonomy_relationships` and the standardized surface; it never edits their producers.

**Depends on:** pf1's `xbrl_filing_facts` + `xbrl_filing_contexts` + `xbrl_filing_dimensions` substrate (the dimensional facts + axis/member decode) and pf1's `xbrl_taxonomy_relationships` / `xbrl_dimension_edges` (definition-linkbase axis→domain→member), plus **PF2-S3**'s `fundamental_standardized` consolidated template (the reconciliation-to-consolidated target). **PARALLEL-SAFE** with the PF2-S5→S6 chain and PF2-S8: disjoint NEW modules, disjoint reserved migration range. Run in an isolated worktree.

---

## Baseline / where the cycles go

The dimensional half of every filing is loaded and then thrown away. That is the whole opportunity.

1. **The consolidated extractor discards every dimensional fact — by design.** `fundamental_xbrl_metrics.py` selects entity-level facts with `WHERE coalesce(ctx.dimension_count, 0) = 0 AND coalesce(ctx.explicit_member_count, 0) = 0` — its docstring: *"a fact is the entity-level (non-segment) value iff its filing context has zero dimension members … dimensioned facts are product/geography/segment breakdowns and are excluded."* The **complement** — facts whose `filing_context_id` has ≥1 row in `xbrl_filing_dimensions` (equivalently `ctx.dimension_count > 0`) — is exactly where segment + footnote detail lives, and nothing reads it. This sprint mines that complement.
2. **The dimensional substrate is already fully decoded and sitting unused.** `xbrl_filing_dimensions` carries, per member row: `dimension_qname` / `dimension_concept` (the axis, e.g. `us-gaap:StatementBusinessSegmentsAxis`), `member_qname` / `member_concept` (e.g. a `*SegmentMember`), `context_element` (`segment` vs `scenario`), `member_kind` (`explicit` / `typed`), and `typed_member_value`; the parent `xbrl_filing_contexts` row carries `period_type` / `period_start` / `period_end` / `instant_date` and the `has_segment` / `explicit_member_count` counts. `xbrl_taxonomy_relationships` (definition linkbase, `child_concept_kind ∈ {axis,domain,member,table,line_items}`) and `xbrl_dimension_edges` resolve which members legally hang under which axis. No table joins facts to this today.
3. **Segment items live under known ASC 280 axes; footnote items under known plan/award/lease axes.** Segment sales/operating income/assets/D&A/capex are the *same* us-gaap concepts (`RevenueFromContractWithCustomerExcludingAssessedTax`, `OperatingIncomeLoss`, `Assets`, `DepreciationDepletionAndAmortization`, `PaymentsToAcquirePropertyPlantAndEquipment`) reported in a `us-gaap:StatementBusinessSegmentsAxis` / `srt:StatementGeographicalAxis` / `srt:ProductOrServiceAxis` / `us-gaap:MajorCustomersAxis` context instead of the default. Footnote detail is `DefinedBenefitPlan*` under `us-gaap:RetirementPlanTypeAxis`, DTA/DTL components, `Lessee*`/`FinanceLease*` maturities, and `ShareBasedCompensation` under `us-gaap:AwardTypeAxis`. All present, all dimensionalized, all unmined.
4. **KNOWN DATA-QUALITY CAVEAT — segments are noisy and this surface must state it.** Compustat Segments / ASC 280 carry **material measurement error**: many firms disclose no segment data; segment definitions drift year-to-year; and for many single-segment firms the reported **segment SIC ≠ the company SIC**. Segment sums therefore do **not** foot to consolidated exactly. This sprint builds a reconciliation-to-consolidated guard **with tolerance**, never an equality assert — the guard *flags* divergence, it does not *require* agreement.

**Already good — do not regress:**
- **The consolidated selection rule.** `fundamental_xbrl_metrics.py`'s `dimension_count = 0` / `explicit_member_count = 0` filter is correct and stays byte-identical. Segments/footnotes are strictly additive NEW tables over the discarded complement; nothing in the entity-level path changes.
- **The decoded dimension substrate.** `xbrl_filing_dimensions`' per-member `dimension_qname` / `member_qname` / `context_element` / `member_kind` decode and the deterministic `filing_dimension_id` / `filing_context_id` join keys are the exact inputs — read them as loaded, do not re-parse XBRL.
- **PIT lineage on facts.** Every mined row inherits `available_at` from the context (`coalesce(ctx.acceptance_datetime, f.acceptance_datetime, ctx.filing_date::TIMESTAMP)` — the same expression the consolidated extractor uses) plus `source_loaded_at` / `run_id`. No new as-of logic; reuse the proven one.

---

## PIT / determinism + production contract

ROADMAP clauses **(A)** bitemporal correctness, **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, **(D)** determinism apply in full; **(E)** schema-as-contract applies to every new table.

- **(A)** Every `segment_*` / `footnote_*` row carries `as_of_date` (= `period_end` / `instant_date`), `available_at` (= the context acceptance/filing timestamp), `source_loaded_at`, `run_id`, `is_latest_revision` (vintage dedup per security/axis/member/period exactly as `normalize_xbrl_metric_rows` does per security/metric/period). As-of readers gate `available_at ≤ as_of_ts`. No lookahead.
- **(B)** Migrations **0117–0120** only. `0117` = `segment_dim` + `segment_fact` schema; `0118` = `footnote_pension` / `footnote_deferred_tax` / `footnote_lease` / `footnote_sbc` schema; `0119` = indexes + reconciliation columns (split schema-vs-index per the S5g/S5k WAL precedent); `0120` reserved (segment SIC/NAICS crosswalk + quality-catalog rows). Every new table seeds `table_catalog` + `field_catalog` in the same migration. Never edit a landed migration; never renumber.
- **(C)** All tests run against in-memory DuckDB with hand-built `xbrl_filing_facts` / `xbrl_filing_contexts` / `xbrl_filing_dimensions` fixture rows (a synthetic filing with a consolidated total + a `StatementBusinessSegmentsAxis` breakout across two members, plus a `DefinedBenefitPlan*` pension context and a lease-maturity context). No SEC / Arelle network. The ~1yr live counts come from an operator-run smoke recorded in the ledger.
- **(D)** `compute_segments` / `compute_footnotes` are pure (pandas in → long DataFrame out), unit-tested without DuckDB; each derived row records `input_codes_json` (the axis/member qnames + source `filing_fact_id`s). Same inputs + params → same rows and the same reconciliation verdict.

**Data posture.** Injectable loader + pure engine + offline fixtures, then an operator-run **~1-year recent proof slice** with live segment/footnote counts + reconciliation pass/flag rates recorded in the ledger. No historical backfill in this sprint.

---

## Tasks

### S7-0 — Segment extraction from dimensional facts + reconciliation-to-consolidated guard *(the big one)*

**Root cause:** the ~14k dimensional facts (`ctx.dimension_count > 0`) are dropped by the consolidated extractor's `dimension_count = 0` filter, so there is no segment surface at all — business/geographic/product/customer breakouts, and their per-segment sales/operating-income/assets/D&A/capex, are invisible despite being fully loaded and decoded in `xbrl_filing_dimensions`.

**Fix (`db/segments.py`, migration 0117 + 0119):** build `segment_dim` (one row per decoded segment context: `filing_context_id`, `segment_type` ∈ {`business`,`geographic`,`product`,`customer`} inferred from `dimension_concept` against the ASC 280 axes — `us-gaap:StatementBusinessSegmentsAxis`, `srt:StatementGeographicalAxis`, `srt:ProductOrServiceAxis`, `us-gaap:MajorCustomersAxis` — plus `axis_qname`, `member_qname`, `member_label`, and a resolved `segment_sic` / `segment_naics` where reported; axis-legality checked against `xbrl_taxonomy_relationships` / `xbrl_dimension_edges`). Build `segment_fact` (per `security_id` × segment-member × period × item) mapping the segment-context us-gaap concepts to canonical items (`segment_sales`, `segment_operating_income`, `segment_assets`, `segment_dep_amort`, `segment_capex`) via the standardized concept map. Add a **reconciliation-to-consolidated** check: sum `segment_fact` per (security, item, period) and compare to the `fundamental_standardized` / `fundamental_xbrl_metric` consolidated total within a relative `reconciliation_tolerance` (default ~2%); write a `reconciliation_status` (`reconciled` / `flagged_divergent` / `no_consolidated`), **never** an equality assert. Capture the ASC 280 major-customer disclosure (customer name/type/address from `MajorCustomersAxis` members + `typed_member_value`).

**PIT:** (A) rows inherit `available_at` / `as_of_date` from the context; vintage dedup per security/axis/member/period. (D) segment-type inference + reconciliation are deterministic given loaded facts.

**Accept:** `segment_dim` + `segment_fact` populated on the proof slice; a fixture with a consolidated total + a two-member `StatementBusinessSegmentsAxis` breakout that foots produces `reconciled`; a fixture where the members diverge beyond tolerance produces `flagged_divergent` (not a failure); a single-segment fixture whose `segment_sic ≠ company_sic` is captured without error.

### S7-1 — Pension/OPEB + deferred-tax footnote sub-ledgers

**Root cause:** pension/OPEB and deferred-tax detail is all present in us-gaap but dimensionalized by plan (`us-gaap:RetirementPlanTypeAxis`) or scattered across component concepts, so the consolidated extractor never captures it — vendors (Compustat pension supplemental) normalize it into a sub-ledger.

**Fix (`db/footnotes.py`, migration 0118):** build `footnote_pension` from `DefinedBenefitPlan*` facts keyed on the `us-gaap:RetirementPlanTypeAxis` member (pension vs OPEB via `PensionPlansDefinedBenefitMember` / `OtherPostretirementBenefitPlansDefinedBenefitMember`): projected benefit obligation (`DefinedBenefitPlanBenefitObligation`), plan assets (`DefinedBenefitPlanFairValueOfPlanAssets`), funded status, service cost (`DefinedBenefitPlanServiceCost`), interest cost, discount rate (`DefinedBenefitPlanAssumptionsUsedCalculatingBenefitObligationDiscountRate`), expected long-term return. Build `footnote_deferred_tax` capturing both the **balance-sheet** level (`DeferredTaxAssetsNet` / `DeferredTaxLiabilities`) and **income-statement** level (`DeferredIncomeTaxExpenseBenefit`, `CurrentIncomeTaxExpenseBenefit`) plus DTA/DTL footnote components (dimensionalized or scattered). All PIT-safe, all offline.

**PIT:** (A) `available_at` from context; per-plan vintage dedup. (B) 0118 seeds `table_catalog` / `field_catalog`. (C) fixtures per family.

**Accept:** `footnote_pension` + `footnote_deferred_tax` populated on the proof slice; a pension fixture keyed on `RetirementPlanTypeAxis` yields PBO/plan-assets/funded-status rows; a deferred-tax fixture emits both balance-sheet and income-statement rows.

### S7-2 — Leases + SBC footnote sub-ledgers

**Root cause:** lease obligations + maturity schedules and stock-based-comp expense/tax-benefit are dimensionalized by lease class / award type (`us-gaap:AwardTypeAxis`) and split pre/post ASC 842, so they too fall outside the consolidated extractor.

**Fix (`db/footnotes.py`, migration 0118):** build `footnote_lease` capturing operating + finance lease liabilities and ROU assets post-ASC 842 (`OperatingLeaseLiability`, `OperatingLeaseRightOfUseAsset`, `FinanceLeaseLiability`) and pre-842 minimum-payment maturities (`OperatingLeasesFutureMinimumPaymentsDue*`, `CapitalLeasesFutureMinimumPaymentsDue*`), with a `lease_standard` flag. Build `footnote_sbc` capturing SBC expense (`ShareBasedCompensation` / `AllocatedShareBasedCompensationExpense`) + related tax benefit (`EmployeeServiceShareBasedCompensationTaxBenefitFromCompensationExpense`), keyed on `us-gaap:AwardTypeAxis` members (`EmployeeStockOptionMember`, `RestrictedStockUnitsRSUMember`) where present. **Out of scope (note it inline):** award-level executive-compensation detail (Execucomp) is a **separate product** — this sprint captures aggregate/award-type SBC only, not per-executive grants.

**PIT:** (A) `available_at` from context; per-class/award vintage dedup. (C) an ASC-842 operating-lease fixture + a pre-842 maturity fixture + an `AwardTypeAxis` SBC fixture.

**Accept:** `footnote_lease` + `footnote_sbc` populated on the proof slice; the ASC-842 vs pre-842 fixtures both route with the correct `lease_standard`; the SBC fixture emits expense + tax-benefit rows; the Execucomp out-of-scope note is written down.

### S7-3 — Segment/footnote coverage + reconciliation-gated quality checks

**Root cause:** once mined, there is no coverage metric or gate telling an operator which firms have usable segment/footnote data and which have segment sums diverging from consolidated beyond tolerance (the ASC 280 measurement-error caveat made operational).

**Fix (`db/footnotes.py` / `db/segments.py` quality registration, migration 0119/0120):** register coverage checks (segment/footnote row counts per security/period on the proof slice) and a **reconciliation-gated** check that flags securities where `segment_fact` sums diverge from the consolidated total beyond `reconciliation_tolerance` — emitted as `warning` (the divergence is *expected* noise, not a hard failure), carrying the per-security divergence in the check details. Surface the coverage + flag counts to `PARITY_GAP.md`.

**PIT:** (A) status derived from loaded facts, no lookahead. (B) 0120 seeds any catalog rows. (D) coverage + flag assignment deterministic.

**Accept:** coverage checks green on the proof slice; the reconciliation check `warning`s on a planted-divergent fixture and passes on a footing one; `PARITY_GAP.md` updated with segment/footnote coverage + reconciliation-flag counts.

---

## Sequencing & expected compounding

**S7-0 → S7-1 → S7-2 → S7-3.** S7-0 (segment extraction + reconciliation guard) is the load-bearing task — it establishes the dimensional-fact mining pattern (`dimension_count > 0` + axis/member decode + `available_at` inheritance + vintage dedup) that S7-1/S7-2's footnote sub-ledgers reuse wholesale, and it builds the reconciliation-to-consolidated machinery S7-3 gates on. S7-1 and S7-2 are independent footnote families sharing `footnotes.py`, so they slot sequentially within that module. S7-3 is last because it reports the outcome of S7-0's reconciliation. The compounding: mining the discarded complement turns "consolidated extraction only" into "consolidated + segment + footnote depth," and the tolerance-based guard makes the ASC 280 measurement error *visible and gated* rather than silently absorbed.

## Risks / guardrails

- **Do NOT assert segment-sum == consolidated.** The central risk is treating noisy ASC 280 segment data as if it foots. Reconciliation is a **tolerance-banded flag** writing `reconciliation_status`, never an equality assert; divergence beyond tolerance is a `warning`, not a failed load. State the measurement-error caveat in the module docstring and the ledger.
- **Do NOT re-mine the consolidated set.** Segments/footnotes read strictly the `dimension_count > 0` complement; the `dimension_count = 0` entity-level path in `fundamental_xbrl_metrics.py` is untouched. A fact must not appear in both a `fundamental_xbrl_metric` row and a `segment_fact` row for the same period.
- **Axis mislabeling.** Segment-type inference keys on the decoded `dimension_concept` against the known ASC 280 axes and is legality-checked via `xbrl_taxonomy_relationships`; an unrecognized axis routes to an `unclassified` segment_type (captured, not dropped, not guessed).
- **Substrate breadth.** The inline `xbrl_filing_facts` cache is only as wide as the operator's cached primary-document downloads (a handful of securities); the proof slice reflects that. Growing the cache is network-gated and out of scope, exactly as the consolidated extractor documents.
- **Migration/WAL safety.** Split schema and index across 0117–0119 per the S5g/S5k precedent; timestamped DB+WAL backup before any live apply. Stay strictly within **0117–0120**.

## Bench / acceptance

- **Segment surface** populated: `segment_dim` + `segment_fact` with segment SIC/NAICS on the proof slice; reconciliation-to-consolidated emits `reconciled` / `flagged_divergent` / `no_consolidated` per (security, item, period).
- **Footnote sub-ledgers** populated: `footnote_pension`, `footnote_deferred_tax`, `footnote_lease`, `footnote_sbc` on the proof slice; Execucomp out-of-scope note recorded.
- **No equality-assert reconciliation** — divergence is a tolerance-banded `warning` carrying per-security detail; `reconciliation_tolerance` documented.
- `python -m pytest atx-impl\db\tests\test_segments.py atx-impl\db\tests\test_footnotes.py -q` green (and the full `atx-impl\db\tests -q` suite stays green before commit).
- **Live-DB smoke** recorded in the ledger: segment/footnote row counts, reconciliation pass/flag rates, and the `run_id`, on the ~1yr proof slice.
- **Ledger row appended** to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains, verification commands, live-DB smoke with exact counts + run_id, caveats/next); `PARITY_GAP.md` status updated with the segment/footnote coverage + reconciliation-flag split.

**Process:** never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
