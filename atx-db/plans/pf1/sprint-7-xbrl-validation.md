# Sprint PF-S7 — XBRL Validation + DQC Hardening

**Goal:** make XBRL validation dimension-aware, properly resolving the 1,364 standing calculation-linkbase failures; add cross-table referential checks; add a DQC rule subset. Reserved migrations 0088-0091.

**Mandate / Owns:** `db/xbrl_validation.py` (dimension-aware), `db/quality.py` multi-table check type, `db/tests/test_referential_quality.py`.

**Must NOT touch:** the ratio/formula engine (`fundamental_ratios.py`, PF-S4's `formula_library.py`), `item_registry.py` (PF-S1). This sprint reads those tables; it never rewrites the derivation logic that produces them.

**Depends on:** PF-S3 (concept coverage + the `xbrl_concept_catalog` / `xbrl_taxonomy_relationships` surface the calc-linkbase check joins against). Runs concurrently with PF-S4 (disjoint primary modules — see ROADMAP "Derivation + trust wave").

---

## Baseline / where the cycles go

The trust story is the weakest of pf1's six axes and the reason is concrete, measured this session against the live warehouse.

1. **`xbrl_validation.py` sums parent-vs-child WITHOUT dimensional-context awareness.** The v1 SQL validator (`refresh_xbrl_validation_results`) joins a parent `xbrl_filing_facts` row to its calc-linkbase children on `security_id + accession_number + primary_document + context_ref + unit_ref` and compares `parent.numeric_value` to `sum(child.numeric_value * edge.weight)`. That join keys on `context_ref` as an opaque string. It never consults `xbrl_filing_contexts` / `xbrl_filing_dimensions` to understand what that context *means* dimensionally (segment/scenario axis + member). So when an issuer reports a parent total in the default (no-dimension) context but breaks its children out across explicit members of a `*Axis` — or vice-versa — the parent and children live in *different* contexts, the weighted sum is computed over an incomplete or mismatched child set, and the row is flagged `failed`. This is the entire mechanism behind the standing failure.

2. **The standing failure, precisely.** `failed_xbrl_calculation_linkbase_checks = 1,364 rows / 5 securities / 11 filings`, all `rule_family = 'calculation_linkbase'`, `rule_code = 'calc_sum_parent_equals_weighted_children'`, characterized in the prior tranche as **dimensional-context mismatch (as-reported quirk)** and **DEFERRED**. The deferral is honest — the tolerance was NOT loosened to bury it — but it is unresolved: 1,364 rows sit `failed` with no evidence attached to distinguish a genuine footing error from a dimensional artifact. Because the check is not dimension-aware, a *real* calc error inside a dimensional breakout would be indistinguishable from these artifacts and could hide in the same bucket. Resolving this is S7-0 and it is the sprint's center of mass.

3. **`quality.py` `SqlQualityCheck` cannot express cross-table referential rules structurally.** Every existing check is a single scalar SQL query returning one number compared to a threshold. Orphan detection today is done by hand-writing a `LEFT JOIN ... WHERE parent IS NULL` inside each check's `sql` string (see `orphan_equity_daily_bars`, `orphan_shares_outstanding_security_ids`, etc.). There is no first-class "every row in A resolves to a parent in B" check type, so referential rules are copy-pasted, easy to get subtly wrong (NULL-handling, `coalesce` semantics), and there is no catalog of which parent/child edges are guaranteed. The rows that the ratio engine consumes — every `fundamental_points` fact, every `fundamental_ratios` input — have no declarative orphan guard.

4. **No DQC rule suite.** The `xbrl_validation_results` table was built (migration S4d, `_xbrl_validation_results`) with a `rule_family` column *explicitly* so additional rule families could be appended — the docstring says "A future Arelle/DQC sidecar can append additional rule families to the same table." That future is this sprint. Today only `rule_family = 'calculation_linkbase'` is ever written. None of the XBRL US DQC checks (sign, member-on-wrong-axis, non-negative concepts) exist.

**Already good — do not regress:**
- **The honest deferral.** The 1,364 are `failed`, visible, and were never hidden by widening `absolute_tolerance` (default `1.0`). Do not "resolve" them by relaxing tolerance — resolve them by correct dimensional grouping. If S7 ends with tolerance still `1.0` and the artifact count explained, that is success.
- **The `xbrl_validation_results` audit trail.** Full per-row lineage: `validation_run_id`, `parent_fact_id`, `parent_value`, `child_weighted_sum`, `absolute_difference`, `tolerance`, `child_count`, `child_facts_json`, `source_url`, `run_id`. The deterministic `validation_id` (sha256 over run + rule + filing keys) and the `DELETE WHERE rule_family = 'calculation_linkbase'` + re-INSERT idempotency pattern both stay.
- **Existing single-table checks in `quality.py`.** The ~60 `SqlQualityCheck` rows (dup keys, bad-value guards, existing hand-rolled orphan checks) all keep passing. The new multi-table type is *additive*; it does not replace the working single-scalar checks.

---

## PIT / determinism contract

ROADMAP clauses **(A)** bitemporal correctness, **(B)** append-only catalogued migrations, **(C)** offline/no-network tests apply in full; **(D)** determinism applies to the validator transform.

- **(A)** Validation reads facts as-loaded; every emitted `xbrl_validation_results` row keeps `source_loaded_at` and `run_id`. Dimension resolution reads `xbrl_filing_contexts` / `xbrl_filing_dimensions` at their loaded state — no as-of lookahead is introduced.
- **(B)** Migrations **0088-0091** only. `0088` catalog/referential-edge seed rows if needed; `0089` the `xbrl_validation_results` columns for dimension-awareness + resolution (`ADD COLUMN IF NOT EXISTS`); `0090`/`0091` reserved (split schema-vs-index per the S5g/S5k WAL precedent, and DQC catalog rows). Every new column/table seeds `table_catalog` + `field_catalog` in the same migration. Never edit a landed migration; never renumber.
- **(C)** All tests run against in-memory DuckDB with hand-built fixture rows (a synthetic filing with a parent total, dimensional child breakouts across a `*Axis`, and one genuinely-wrong footing). No SEC / Arelle network. The 1,364-row triage evidence is produced by an operator-run live query recorded in the ledger, never in pytest.
- **(D)** The core comparison stays pure SQL over loaded tables → deterministic rows. **Do NOT suppress real errors by loosening tolerance; resolve artifacts by correct dimensional-context grouping.** Same inputs + same run params → same validation rows and the same real-vs-artifact split.

---

## Tasks

### S7-0 — Dimension-aware linkbase validation *(the big one)*

**Root cause:** the parent↔child join in `refresh_xbrl_validation_results` matches on `context_ref` as an opaque string and never resolves the dimensional signature of that context. A parent total reported in the default context is compared against children whose calc-linkbase edges are footed within dimensional member contexts (or the reverse), so the weighted sum is taken over a non-comparable child set. 1,364 rows fail for this structural reason.

**Fix:** make the check context/axis-member aware. Join each fact's `context_ref` through `xbrl_filing_contexts` (keyed `security_id + accession_number + primary_document + context_id`) to its dimensional signature in `xbrl_filing_dimensions` (segment/scenario axis + member). Group parent and children so a parent total sums **only over children sharing a comparable dimensional context** — same period/unit AND the same dimensional signature (default-vs-default, or the same explicit axis→member set), not merely the same `context_ref` literal. Where the calc linkbase itself is dimensionless (the common case), compare within the no-dimension context set. Triage each of the 1,364 failures into **real-error** vs **dimensional-artifact** and attach evidence per failure (the parent context signature, the child context signatures, and why they were previously incomparable). Resolve artifacts by the corrected grouping — **not** by widening tolerance.

**PIT:** (A) no as-of change; reads contexts/dimensions as loaded. (D) grouping is deterministic given loaded facts.

**Accept:** every one of the 1,364 standing failures is triaged real-vs-artifact with dimensional evidence; artifacts flip to `passed`/resolved under correct grouping while `absolute_tolerance` stays `1.0`; any residual real errors remain `failed` and each is individually documented. A fixture reproduces one artifact (parent-default vs child-dimensional) that the dimension-aware check resolves and one genuine footing error that it still catches.

### S7-1 — Multi-table referential check type in `quality.py`

**Root cause:** `SqlQualityCheck` is single-scalar-only; referential ("every row in A has a parent in B") rules are hand-written `LEFT JOIN … WHERE parent IS NULL` strings, duplicated and error-prone, with no declarative type and no coverage over the fundamentals rows the ratio engine consumes.

**Fix:** add a first-class referential check type (e.g. a `ReferentialQualityCheck` dataclass or a `child_table`/`child_key`/`parent_table`/`parent_key` factory that compiles to the vetted anti-join) sharing the existing `QualityResult` + `_table_exists` + `_passes` machinery, with correct NULL-key semantics (a NULL child key is skipped, matching the existing `security_id IS NOT NULL` guards). Register orphan-detection checks for the fundamentals DAG: every `fundamental_ratios` input row resolves to its statement-layer parent (`fundamental_points` today; `fundamental_statement_points` once PF-S3/PF-S8 land it — parameterize the parent so it upgrades cleanly), and every fact's item resolves in the canonical item dimension (`fundamental_item`, PF-S1). Guard the join so it no-ops (not fails) when the forward-looking parent table is absent, mirroring `required_tables` + `warn_if_missing`. Add migration **0088** only if catalog rows are needed to register the new check family.

**PIT:** (B) any catalog rows in 0088 seed `table_catalog`/`field_catalog`. (C) fixtures with a planted orphan.

**Accept:** the referential type expresses "A→B parent resolution" declaratively; new orphan checks are green on the live warehouse (0 orphans) and red on a fixture with a planted orphan; existing single-table checks unaffected.

### S7-2 — DQC rule subset

**Root cause:** the `rule_family` column exists to host more than calc-linkbase, but no XBRL US DQC rules are implemented, so sign errors and axis-misuse pass silently.

**Fix:** port a defensible, SQL-expressible subset of the XBRL US Data Quality Committee rules as new `rule_family` values written to `xbrl_validation_results`: (a) **sign checks** — concepts that must be non-negative as-reported are flagged when negative; (b) **member-on-wrong-axis** — a member appearing under an axis it does not belong to, resolved via `xbrl_filing_dimensions` + `xbrl_taxonomy_relationships` (definition-linkbase dimension edges); (c) **non-negative concepts** — a curated list of us-gaap concepts that DQC treats as sign-constrained. Document, in this file's companion note and inline, exactly which DQC rule numbers were ported, which were deliberately skipped (require Arelle/full calculation semantics beyond SQL), and why. Keep it a *defensible subset* — do not fake rules that need a full XBRL processor. Reserve migration **0091** for any DQC concept-list catalog rows.

**PIT:** (C) fixtures per rule (a negative concept that should be caught, a member on the correct axis that must not fire). (D) pure SQL → deterministic.

**Accept:** the DQC subset runs and writes rows under new `rule_family` values; each ported rule has a passing and a failing fixture; the skipped-rules rationale is written down.

### S7-3 — Validation report + wiring

**Root cause:** even once grouping is fixed, there is no `resolution_status` on `xbrl_validation_results` to record *why* a former failure is now considered resolved, and no report that reduces the visible failed count to the genuine-error residual.

**Fix:** add a `resolution_status` column (migration **0089**, `ADD COLUMN IF NOT EXISTS`, catalogued) taking values like `unresolved` / `resolved_dimensional_artifact` / `genuine_error`. Record dimension-aware results with that status so the headline failed-check count drops to the genuine-error residual, each residual row carrying its own documented reason (in `message` + `resolution_status`). Wire the counts into the existing `XbrlValidationDataset.load` `quality_check` emission and the `DatasetLoadResult` details so the orchestrator (PF-S2) and `PARITY_GAP.md` surface the real-vs-artifact split, not a raw 1,364.

**PIT:** (A) status is derived from loaded contexts, no lookahead. (B) 0089 catalogued. (D) status assignment deterministic.

**Accept:** `xbrl_validation_results` carries `resolution_status`; the failed-check count reported to `quality_check` is the genuine-error residual; each residual documented; `PARITY_GAP.md` updated with the split.

---

## Sequencing & expected compounding

**S7-0 → S7-1 → S7-2 → S7-3.** S7-0 (dimension-awareness) is the load-bearing task and must land first — the corrected grouping is what every downstream count depends on, and it is what earns production trust in the fundamentals the ratio engine consumes. S7-1 (referential type) is independent of S7-0's SQL but shares the `quality.py` surface, so it slots second. S7-2 (DQC subset) reuses the dimension resolution S7-0 builds (member-on-wrong-axis needs it), so it follows. S7-3 (report/`resolution_status`) is last because it records the outcome of S7-0's triage. The compounding: once the 1,364 collapse to a small documented residual, the calc-linkbase check becomes a *trustworthy* gate — a new real footing error surfaces instead of drowning in artifacts — and the referential + DQC families extend that trust from "the sum foots" to "the facts link and the signs/axes are sane."

---

## Risks / guardrails

- **Over-grouping hides a real error.** The central risk: making grouping so permissive that a genuine footing failure gets absorbed as a "dimensional artifact." Mitigate by keeping an explicit real-error residual, documenting each residual individually, and attaching per-failure evidence to every one of the 1,364 triaged rows (parent context signature, child context signatures, comparability verdict). If a failure cannot be *positively* explained as a dimensional artifact with evidence, it stays `genuine_error`.
- **Never loosen tolerance to make a check pass.** `absolute_tolerance` ends the sprint at `1.0`. The honest deferral is a feature; the fix is correct grouping, not a wider band.
- **High blast radius on the fundamentals DAG.** Validation sits over the facts the whole ratio engine reads. Land everything behind the existing `xbrl_validation.py` surface and as *additive* `rule_family` values / *additive* `quality.py` check types — do not change what an existing passing row means. New columns are `ADD COLUMN IF NOT EXISTS`; new checks default to no-op when a forward-looking parent table (`fundamental_statement_points`, `fundamental_item`) is not yet present.
- **Migration/WAL safety.** Split schema and index across migration numbers per the S5g/S5k precedent; preserve a timestamped DB+WAL backup before any live apply. Stay strictly within **0088-0091**.

---

## Bench / acceptance

- The **1,364 standing failures** are triaged with dimension awareness, reporting the real-error vs dimensional-artifact counts (evidence per triaged failure).
- New **referential checks green** on the live warehouse (0 orphans) and demonstrably red on a planted-orphan fixture.
- The **DQC subset runs** and writes rows under its new `rule_family` values; ported-vs-skipped rationale documented.
- **No tolerance-loosening hacks** — `absolute_tolerance` stays `1.0`; artifacts resolved by grouping and `resolution_status`, not by widening the band.
- `python -m pytest atx-impl\db\tests\test_xbrl_validation.py atx-impl\db\tests\test_referential_quality.py -q` green (and the full `atx-impl\db\tests -q` suite stays green before commit).
- **Live-DB smoke** recorded in the ledger: the pre/post failed-check counts (headline `1,364` → genuine-error residual), the real-vs-artifact split, and the `run_id`.
- **Ledger row appended** to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains, verification commands, live-DB smoke with exact counts + run_id, caveats/next); `PARITY_GAP.md` status updated.

**Process:** never `git add -A` (stage explicit paths); never push unless asked. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
