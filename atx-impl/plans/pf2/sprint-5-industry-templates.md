# Sprint PF2-S5 — Industry-specialized templates

**Goal:** promote pf1-S3's bank/insurance/REIT *overlay map rows* into full normalized industry **templates** with their own item sets, entity→template routing, and ratio families; add the two missing profiles — **utility (rate base)** and **broker-dealer** — and derive **REIT FFO/AFFO**. Reserved migrations `0110–0113`.

**Mandate / Owns:** NEW `db/industry_templates.py` (the `industry_template` dim + entity routing + template item sets); a new region in `db/fundamental_statements.py` `FUNDAMENTAL_STATEMENT_MAP_ROWS` (utility/broker-dealer rows, REIT FFO/AFFO derivations); industry ratio families as new rows in `db/seeds/formula_registry.csv`; NEW `db/tests/test_industry_templates.py`.

**Must NOT touch:** the ratio-engine internals (`fundamental_ratios.py`) and pf1-S4's `formula_library.py` grammar/dispatch (`read_formula_registry_seed`, `_OPERAND_TERM_EVALUATORS`, `COMPOSITE_EVALUATORS` are consumed, not edited — new families are *rows*, not new opcodes); `item_registry.py` (PF-S1 — depend, don't mutate); the `ALL`-template rows and the existing `bank_statement`/`insurance_statement`/`reit_statement` overlay item_ids `1501–1712` (extend alongside, never renumber). PF2-S6 owns the TTM region of the same file — see Sequencing.

**Depends on:** pf1-S3 (the 37 overlay map rows + `industry_template` keying `("source","taxonomy","concept","industry_template")`), pf1-S4 (`formula_registry` declarative rows), PF2-S3 (the standardized surface the templates route over). Sequential **before PF2-S6** (both touch `fundamental_statements.py`); MAY run concurrently with PF2-S7/S8 (disjoint NEW modules) in isolated worktrees.

---

## Baseline / where the cycles go

The industry story stops at *overlays* — declared map rows, not templates with their own routed item sets and ratio families. Measured this session against the live warehouse and the two owned modules.

1. **The overlays are map rows, not templates.** `FUNDAMENTAL_STATEMENT_MAP_ROWS` (`fundamental_statements.py:448–486`) carries 37 overlay rows — bank `1501–1515` (15), insurance `1601–1610` (10), REIT `1701–1712` (12) — keyed on `industry_template` codes `BK`/`IS`/`RT`. They *declare* concepts (`InterestAndDividendIncomeOperating`→`interest_income_bank`, `PremiumsEarnedNet`→`premiums_earned`, `nareit:FundsFromOperations`→`ffo`) but there is no first-class `industry_template` **dim**, no per-template *required item set*, and no ratio family scoped to a template. Comparability across a peer group is not enforced.

2. **Routing is an inline CTE, not a governed surface.** Entity→template resolution lives only inside the `refresh` SQL as the `security_industry_templates` CTE (`fundamental_statements.py:737–760`): a `CASE` over `entity_classification.node_code` cast to INTEGER against `taxonomy.code = 'SIC'` — SIC `6000–6199`→`BK`, `6300–6411`→`IS`, `= 6798`→`RT`, else `ALL`. It is unauditable (no table you can query for "which template did entity X get, and why"), un-exhaustive (no assertion every routed entity resolves to exactly one template), and hard-codes the FactSet-style profile split in one query.

3. **Utility and broker-dealer are entirely absent; the FactSet-4 / Compustat split is incomplete.** Vendors run distinct statement schemas per industry — FactSet classifies every company into one of four profiles (**Commercial / Bank / Insurance / Other-Financial**); Compustat splits **Industrial (INDL)** vs **Financial-Services (FS)** and ships industry supplemental item sets (utilities, oil&gas, airlines, homebuilding, managed-care, …); S&P maintains a dedicated **Utility** item set with *Not-Available* markers for industrial items that don't exist in a utility column. We have `BK`/`IS`/`RT`/`ALL` — **no utility (rate base / regulated-asset) template and no broker-dealer template.**

4. **REIT FFO/AFFO are declared but not derived.** `ffo` (row `1701`) maps `nareit:FundsFromOperations` (taxonomy `nareit`, not a us-gaap primary-statement concept — a Nareit-standard-since-1991 footnote/press-release construct that strips GAAP real-estate depreciation); `affo` (row `1703`) is an `__EXTENSION__` sentinel, `is_active=False`. `ffo_per_share`/`affo_per_share` are `is_derived=True` rows *keyed off* `ffo`/`affo`, but FFO/AFFO themselves are never derived from us-gaap primaries when the vendor tag is absent — so a REIT with no `nareit:` tag emits nothing.

5. **Some overlay items are intentionally non-loadable and must stay explained.** `nonperforming_loans` (`1507`), `total_deposits` (`1510`), `tier1_capital` (`1511`), `insurance_float` (`1610`), `nav_per_share` (`1710`) etc. are `vendor-only`/`extension` sentinels (`__VENDOR_ONLY__`/`__EXTENSION__`, `is_active=False`) enumerated by `statement_map_overlay_exception_rows` / `StatementMapOverlayException` so they don't silently trip `bad_fundamental_statement_map_rows` / `loaded_xbrl_concepts_without_statement_map`. New templates must extend that allowlist discipline, not bypass it.

**Already good — do not regress:**
- **The `industry_template` keying + COALESCE join.** `FUNDAMENTAL_STATEMENT_MAP_KEY` and the `m.industry_template = 'ALL' OR m.industry_template = coalesce(it.industry_template,'ALL')` join (`:846`) let a template override an `ALL` row without colliding. New templates key the same way; `ALL` stays the industrial fallback.
- **The enumerated overlay allowlist.** `statement_map_overlay_exception_rows` / `unexplained_statement_map_overlay_rows` keep the non-loadable set *explained*, not tolerated. New utility/broker-dealer vendor-only items join that report; the gate stays "unexplained = 0".
- **The `is_derived` / `derivation_expr` mechanism.** `__DERIVED__combined_ratio` (`derivation_expr="combined_ratio = loss_ratio + expense_ratio"`), `__DERIVED__ffo_per_share`, `__DERIVED__nim` prove the declarative-derivation pattern. FFO/AFFO derivation reuses it — no new engine.

---

## PIT / determinism + production contract

ROADMAP clauses **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, **(D)** determinism apply in full; **(E)** schema-as-contract applies to every new table.

- **(B)** Migrations **0110–0113** only. `0110` — `industry_template` dim + `entity_industry_template` routing table (schema); `0111` — routing index + any `ADD COLUMN IF NOT EXISTS` on `fundamental_statement_map` for a `not_available` marker (split schema/index per the S5g/S5k WAL precedent); `0112` — `industry_template_coverage` report table (schema); `0113` — coverage index + gated quality-check catalog rows. Every new table/column seeds `table_catalog` + `field_catalog` in the same migration. Statement-map/formula rows are code/CSV constants re-materialised by the existing refresh + `seed_formula_registry`, not DDL. Never edit a landed migration; never renumber; stay strictly inside `0110–0113`.
- **(C)** All tests run against in-memory / template-copy DuckDB with fixture entities (one bank via SIC `6022`, one insurer `6311`, one utility `4911`, one broker-dealer `6211`, one REIT `6798`, one industrial). No SEC / companyfacts network in pytest. The ~1yr recent proof-slice re-refresh over the loaded universe is **operator-run** and recorded in the ledger.
- **(D)** Routing and template resolution are pure functions of loaded `entity_classification` + the map/dim constants → deterministic. FFO/AFFO derivations are pure `derivation_expr` over loaded inputs. Same inputs + same run params → same template assignment and same derived rows.
- **(E)** No template dim, routing table, or coverage table lands without a contract row + `table_catalog` entry; the drift check (PF2-S1) must stay green.

---

## Tasks

### S5-0 — Full industry-template model + entity routing *(the big one)*

**Root cause:** the FactSet-4 / Compustat-INDL-vs-FS split lives as ad-hoc `industry_template` code strings on 37 map rows plus an inline `CASE` in the `refresh` CTE (`security_industry_templates`). There is no template dim, no per-template required item set, no queryable routing, no utility, no broker-dealer.

**Fix:** NEW `db/industry_templates.py` promoting overlays to templates. (a) An `industry_template` **dim** (migration `0110`) enumerating `ALL`(=Commercial/Industrial), `BK`, `IS`, `UT` (utility), `BD` (broker-dealer), `RT` (REIT) — mirroring the FactSet-4 (Commercial/Bank/Insurance/Other-Financial) with UT/BD/RT as the Compustat-FS/supplemental refinements — each row carrying label, the `INDL`-vs-`FS` class, and its required-item-set reference. (b) An `entity_industry_template` **routing table** (`0110`) materialising the CTE logic into a queryable, PIT-stamped `security_id → industry_template` surface (extend the SIC `CASE` with utility `4900–4999` and broker-dealer `6200–6299`; keep the existing `6000–6199`/`6300–6411`/`6798` bands); the `refresh` CTE reads this table instead of re-deriving. (c) Add `UT`/`BD` template rows to `FUNDAMENTAL_STATEMENT_MAP_ROWS` (item_ids `1801–18xx` utility rate-base/regulated-asset, `1901–19xx` broker-dealer) — real us-gaap where a verified concept exists (`RegulatedAndUnregulatedOperatingRevenue`, `PublicUtilitiesPropertyPlantAndEquipmentRateBaseAmount`; broker `PayablesToBrokerDealersAndClearingOrganizations`, `SegregatedCashAndSecurities`), `__VENDOR_ONLY__` sentinels (allowance for rate base, net capital) enumerated in `statement_map_overlay_exception_rows`.

**PIT:** (B) dim+routing in `0110`, catalogued; routing rows PIT-stamped (`valid_from`, `run_id`), no as-of change to fact availability. (D) SIC→template is a deterministic pure map. (E) both tables carry contract rows.

**Accept:** a fixture with six entities (industrial/bank/insurer/utility/broker-dealer/REIT) routes each to **exactly one** template via `entity_industry_template`; utility/broker-dealer map rows resolve; every new non-loadable item is enumerated in the overlay exception report (`unexplained_statement_map_overlay_rows` = 0); the `refresh` CTE reads the table and reproduces the prior BK/IS/RT assignments byte-for-byte.

### S5-1 — REIT FFO/AFFO derivation

**Root cause:** `ffo` (`1701`) depends on a `nareit:FundsFromOperations` tag that is a footnote/press-release construct absent from us-gaap primaries; `affo` (`1703`) is an inactive `__EXTENSION__`. When the vendor tag is missing the REIT template emits no FFO, so `ffo_per_share`/`ffo_payout_ratio` have no input.

**Fix:** add `is_derived=True` map rows (in the same `FUNDAMENTAL_STATEMENT_MAP_ROWS` region) that derive FFO from us-gaap primaries per the Nareit definition when `nareit:FundsFromOperations` is absent — `derivation_expr="ffo = net_income + real_estate_depreciation_amortization - gains_on_property_sales"` (Nareit strips GAAP real-estate depreciation) — and AFFO — `derivation_expr="affo = ffo - recurring_capex - straight_line_rent_adjustment"`. Keep the vendor `nareit:` tag at higher `concept_priority` so a reported FFO wins over the derivation (COALESCE-priority precedent). Promote `affo`/`ffo` to `is_active=True` under the derived path; leave the truly-unavailable KPIs (`occupancy_rate`, `nav_per_share`) in the allowlist.

**PIT:** (D) derivation is pure over loaded inputs, deterministic; `available_at = max(input.available_at)`. (B) no new migration — code constants re-materialised by the existing refresh.

**Accept:** a fixture REIT with no `nareit:` tag but with net income + real-estate depreciation emits a derived `ffo`; a fixture REIT *with* the vendor tag keeps the reported value (priority); `ffo_per_share` and `ffo_payout_ratio` now have inputs on both.

### S5-2 — Industry ratio families in `formula_registry`

**Root cause:** `db/seeds/formula_registry.csv` (73 rows) has families `profitability`/`leverage`/`liquidity`/`efficiency`/`growth`/`health`/`payout`/`per_share`/`cash_flow` — **no industry-specific family**. Bank NIM, insurance combined-ratio, utility rate-base, REIT FFO-payout live only as inert overlay map rows, not as computable registry formulas.

**Fix:** append new `formula_registry.csv` rows (consumed by `seed_formula_registry`, no engine change) under new `family` values — `bank`, `insurance`, `utility`, `reit`: bank **NIM** (`net_interest_income / average_earning_assets`), **efficiency_ratio**, **npl_ratio** (`nonperforming_loans / total_loans`), **tier1_capital_ratio**; insurance **loss_ratio**, **expense_ratio**, **combined_ratio** (`loss_ratio + expense_ratio` via the existing `expression`/composite grammar); utility **rate-base** return metrics; REIT **ffo_payout** (`dividends / ffo`), **affo_per_share**. Each row uses the vetted `transform`/`inputs`/`expression` grammar (e.g. `divide`, `key:...|key:...`) and points at the S5-0/S5-1 canonical metrics; carry `valid_from`, `citation`, `is_meaningful_rule`.

**PIT:** (D) formulas are declarative rows dispatched by the unchanged `formula_library` interpreter — pure, deterministic. (C) grammar validated offline by `read_formula_registry_seed`.

**Accept:** the new rows load via `seed_formula_registry` and validate under `read_formula_registry_seed` (numeric_item_ids resolve in the PF-S1 registry); a fixture bank emits NIM + efficiency, an insurer emits combined-ratio, a REIT emits ffo-payout, each scoped to its template; no existing family regresses.

### S5-3 — Industry-template coverage report + gated quality check

**Root cause:** there is no artifact answering "does every routed entity resolve to exactly one template, and are that template's required items present or explicitly Not-Available?" — the same gap pf1-S3 closed for the `ALL` concept set, now needed per template.

**Fix:** add an `industry_template_coverage` report table (migration `0112`, catalogued): per `industry_template`, the count of routed entities, template-required items present, and required items marked *Not-Available* (the enumerated allowlist). Wire a `SqlQualityCheck` (severity `critical`, gated per clause G) asserting (a) every entity in `entity_industry_template` resolves to **exactly one** template and (b) every template-required item is either present or in the overlay exception report — so a future unmapped utility/broker-dealer item re-trips as a detected failure. Register catalog rows in `0113`.

**PIT:** (B) table + check catalogued in `0112`/`0113`. (D) the report is a pure projection — same inputs → same rows.

**Accept:** the coverage report emits one row per template with present/Not-Available counts; the gated check is green on the fixtures (0 multi-template entities, 0 unexplained missing items) and red on a fixture with an entity routed to two templates and a fixture with an un-allowlisted missing item.

---

## Sequencing & expected compounding

**S5-0 → S5-1 → S5-2 → S5-3.** S5-0 is load-bearing: the dim + routing + utility/broker-dealer item sets are what every downstream task scopes against, so it lands first. S5-1 (FFO/AFFO derivation) needs the REIT template's item set present. S5-2 (ratio families) consumes both S5-0's canonical metrics and S5-1's derived FFO. S5-3 (coverage + gate) records the outcome of all three and can only be meaningful once dim, routing, and families exist. **Compounding:** once overlays become routed templates with their own families, a bank emits NIM/efficiency/NPL, an insurer a combined ratio, a utility rate-base metrics, a REIT FFO-payout — template-correct — and the gated coverage check turns "we declared bank rows" into "every routed entity is provably one template with its required items present-or-explained," directly setting up PF2-S6's calendarization over the same statement surface.

---

## Risks / guardrails

- **A routed entity resolves to two templates (or zero).** The central risk: overlapping SIC bands or a null classification. Mitigate with the S5-3 gated exactly-one-template check and a deterministic priority order in the routing `CASE`; an unclassified entity falls to `ALL`, never to NULL.
- **Utility/broker-dealer allowlist quietly grows.** Every new `__VENDOR_ONLY__`/`__EXTENSION__` item must be enumerated in `statement_map_overlay_exception_rows`; `unexplained_statement_map_overlay_rows` must gate to 0, exactly as pf1-S3 held the bank/insurance/REIT allowlist.
- **FFO double-counts vendor vs derived.** Keep `nareit:FundsFromOperations` at higher `concept_priority` so the reported value wins; verify with a fixture carrying both tag and primaries — the derivation must not fire when the tag is present.
- **Cross-sprint collision on `fundamental_statements.py`.** PF2-S6 touches the TTM region of the same file. Run S5→S6 **sequentially in one tree**; never concurrently. Utility/broker-dealer rows append after `1712`; never renumber `1501–1712`.
- **Migration/WAL safety.** Split schema and index across `0110–0113` per the S5g/S5k precedent; preserve a timestamped DB+WAL backup before any live apply; stay strictly within the reserved range.

---

## Bench / acceptance

- Every routed entity resolves to **exactly one** template (bank/insurance/utility/broker-dealer/REIT/industrial); utility + broker-dealer templates present with real us-gaap where verified, allowlist enumerated for the rest.
- REIT **FFO/AFFO derived** from us-gaap primaries when the `nareit:` tag is absent, vendor tag winning when present; industry ratio families (bank NIM/efficiency/NPL/Tier-1, insurance combined/loss/expense, utility rate-base, REIT FFO-payout/AFFO-per-share) load and compute template-scoped.
- The `industry_template_coverage` report emits per-template present/Not-Available counts; the gated critical check is green on the live warehouse and red on planted-failure fixtures.
- **Tests green:** `python -m pytest atx-impl\db\tests\test_industry_templates.py -q` (and the full `atx-impl\db\tests -q` suite green before commit).
- **Live-DB smoke** recorded in the ledger: per-template routed-entity counts, the utility/broker-dealer item counts, a sample of derived FFO rows, and the `run_id`, from an operator-run ~1yr recent proof-slice re-refresh.
- **Ledger row appended** to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains, verification commands, live-DB smoke with exact counts + `run_id`, caveats/next); `PARITY_GAP.md` status updated (industry-templates axis: overlays → routed templates).

**Process:** never `git add -A` (stage explicit paths); never push unless asked; new module ⇒ new `test_*.py`; `python -m pytest atx-impl\db\tests -q` green before commit. Commit trailer EXACTLY: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
