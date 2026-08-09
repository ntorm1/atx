# Sprint PF-S4 — Formula Library + Full Ratio/Derived Expansion

**Goal:** refactor the 53 ratio codes + 4 scores from Python lambdas into a declarative `formula_registry` (definition-as-data + academic citations); expand ratio families to the full Compustat/FactSet derived set. Reserved migrations 0075-0078.

**Mandate / Owns:** NEW `db/formula_library.py`, `db/fundamental_ratios.py` refactor to consume it, `formula_registry` table, `db/tests/test_formula_library.py`.

**Must NOT touch:** valuation-multiple price join (PF-S6 owns that `fundamental_ratios.py` region — anything joining `equity_daily_bars` / market cap into the wide frame), `item_registry` (PF-S1 owns `db/item_registry.py` and the `fundamental_item` dimension).

**Depends on:** PF-S1 (the registry resolves inputs via the item dim rather than the hard-coded `TTM_INPUTS` / `BALANCE_INPUTS` / `XBRL_BALANCE_INPUTS` / `XBRL_FLOW_INPUTS` maps), PF-S3 (new XBRL concepts — interest, receivables/payables detail, tax, minority interest — feed the new coverage / accrual / cash-conversion ratios).

---

## Baseline / where the cycles go — table

Everything the derived layer knows today lives as executable Python, not as data, in `db/fundamental_ratios.py`:

| Anchor | What is there today (real) | Why it costs us parity |
|---|---|---|
| `RATIO_DEFS` (lines ~267-485) | 53 `RatioDef` tuples; each carries `code`, `category`, `kind`, `unit`, `numerator_code`, `denominator_code`, an `inputs` gate tuple, and an `operands` **lambda** returning `(num, den)` | The formula is a closure — not queryable, not citable, not diff-able. "What is `ebitda_margin`?" is only answerable by reading a lambda: `lambda r: (r["oi"] + r["depreciation_amortization"], r["rev"])`. |
| The 4 score lambdas (lines ~105-264): `_altman_z_double_prime`, `_piotroski_f_score`, `_ohlson_o_score`, `_beneish_m_score` | Monolithic Python functions with all coefficients and gates inlined; the citations live only in docstrings | A distress score with no machine-readable coefficients/citation cannot be audited, re-parameterized, or compared to the paper. The coefficient set is buried in code, not stored as data. |
| `RatioDef.composite` (line ~90) | Score defs bypass `operands` and call one of the 4 lambdas | Scores are a special code path, not a formula-registry row like everything else. |
| Families | `RatioDef.category` string field enumerates the family taxonomy inline: profitability / leverage / efficiency / liquidity / cash_flow / payout / per_share / growth / health (docstring at line ~80) | The family list is a comment, not a governed enum; whole families (DuPont, coverage, accruals, cash-conversion-cycle) are simply absent. |
| Input maps (lines ~51-65, ~489-521) | `TTM_INPUTS`, `BALANCE_INPUTS`, `XBRL_BALANCE_INPUTS`, `XBRL_FLOW_INPUTS`, `GROWTH_PRIOR_KEYS` | Hard-coded canonical-metric strings — the exact wall PF-S1 tears down. The formula registry must reference **item ids**, resolved through PF-S1's dim, not these literals. |

**Family census today (53 codes, grounded in `RATIO_DEFS`):**

- **profitability (12):** `net_profit_margin`, `operating_margin`, `return_on_assets`, `return_on_equity`, `operating_return_on_assets`, `average_return_on_assets`, `average_return_on_equity`, `gross_margin`, `cost_of_revenue_to_revenue`, `ebitda`, `ebitda_margin`, `retained_earnings_to_assets`
- **leverage (9):** `assets_to_equity`, `liabilities_to_assets`, `liabilities_to_equity`, `long_term_debt_to_equity`, `long_term_debt_to_assets`, `net_debt`, `net_debt_to_assets`, `interest_coverage`, `equity_to_liabilities`
- **cash_flow (9):** `free_cash_flow`, `fcf_margin`, `operating_cash_flow_to_net_income`, `capex_to_revenue`, `operating_cash_flow_margin`, `operating_cash_flow_to_assets`, `operating_cash_flow_to_liabilities`, `capex_to_operating_cash_flow`, `operating_cash_flow_to_average_assets`
- **payout (4):** `dividend_payout_ratio`, `buyback_to_net_income`, `total_payout_ratio`, `retention_ratio`
- **per_share (1):** `book_value_per_share`
- **efficiency (5):** `asset_turnover`, `equity_turnover`, `fixed_asset_turnover`, `receivables_turnover`, `ppe_to_assets`
- **liquidity (5):** `current_ratio`, `quick_ratio`, `cash_ratio`, `working_capital`, `working_capital_to_assets`
- **growth (6):** `revenue_growth_yoy`, `net_income_growth_yoy`, `operating_income_growth_yoy`, `operating_cash_flow_growth_yoy`, `assets_growth_yoy`, `equity_growth_yoy`
- **health / score (4):** `altman_z_double_prime`, `piotroski_f_score`, `ohlson_o_score`, `beneish_m_score`

`kind` today is one of `ratio | level | difference | growth | per_share | score`; `unit` one of `ratio | currency | currency_per_share | score`. 12+9+9+4+1+5+5+6+4 = **53**.

**Already good — do not regress:**

- `compute_ratio_rows` (lines ~577-639) is a **pure** `DataFrame -> DataFrame` transform, unit-tested independent of DuckDB. The refactor keeps it pure; only the *source* of the formula table changes.
- Denominator gating via `is_meaningful` and `require_positive_denominator` (lines ~617-632): `level` sums, `difference` subtracts, `growth`/`ratio` divide and skip on a zero denominator; `is_meaningful` is false off a non-positive base. Preserve this branch logic byte-for-byte.
- Per-ratio `available_at = max(input availabilities)` (lines ~600-603) — the (A) no-lookahead guarantee. The registry must not change how availability is computed.
- `input_codes_json` lineage (line ~572) records `list(d.inputs)`. Every registry row keeps an explicit input list so lineage is unchanged.
- `_attach_prior_year` (lines ~652-689) pairs the ~365-day-prior row for growth / average-balance / YoY-signal formulas via `GROWTH_PRIOR_KEYS`. New YoY-flavored formulas extend that key set; the 350-380-day pairing window is untouched.

---

## PIT / determinism contract

Applies: ROADMAP **(A)** bitemporal correctness — every registry-produced row still sets `available_at = max(input.available_at)` and `as_of_date = period_end`; **(B)** append-only catalogued migrations — `0075-0078`, each seeding `table_catalog` + `field_catalog`; **(D)** determinism + provenance — `compute_ratio_rows` stays pure and every row keeps `input_codes_json`.

**REGRESSION GATE (non-negotiable):** the existing 53 codes must rebuild **BYTE-IDENTICAL** after the registry refactor — same `ratio_code` set, same `value`, same `numerator_value` / `denominator_value`, same `is_meaningful`, same `available_at`, same `ratio_id` hash, same row count over the same fixture. The port is a pure representation change (lambda → data), not a math change. New formulas ship as **additive** registry rows only; they never alter an existing code's output.

---

## Tasks — S4-0 .. S4-3

### S4-0 — `formula_registry` table + seed catalog (migration 0075 schema, 0076 index)

**Root cause.** There is no place to *store* a formula. The definition is a Python closure; the taxonomy is a comment; the coefficients are inlined constants.

**Fix.** Migration `0075` creates `formula_registry` (idempotent `CREATE TABLE IF NOT EXISTS`; seeds `table_catalog` + `field_catalog`). Columns:

- `formula_code` (PK, matches today's `ratio_code`), `family` (governed enum superset of the 9 families above), `kind` (`ratio | level | difference | growth | per_share | score`), `unit`.
- `numerator_item_ids` / `denominator_item_ids` — arrays of PF-S1 item ids (not raw strings). For scores these are null and the terms live in `expression`.
- `transform` — the reducer selector mirroring today's `kind` branches (`divide`, `sum`, `difference`, `pct_change`, `identity`), plus a declarative `expression` for multi-term formulas (DuPont, the coverage/accrual composites, and the 4 scores) — a small, closed, deterministic mini-grammar over named item ids + literals (no arbitrary `eval`; the evaluator is a fixed dispatch table, itself unit-tested).
- `is_meaningful_rule` — encodes `require_positive_denominator` and any per-formula gate (e.g. Beneish's `sales>0 and receivables0>0`) as data, not a Python `if`.
- `citation` — free-text academic / vendor citation (empty for the plain accounting ratios; required for scores and named academic formulas).
- `valid_from` (`DATE`) — the (A) bitemporal `valid_from` so a formula definition is itself as-of-able (feeds S4-3 and PF-S8 vintage history); default the sprint date.
- Standard provenance: `run_id`, `source_loaded_at`.

Migration `0076` adds the lookup index (`formula_code`, and a `family` index for the catalog surface). Schema and index are split into two migration numbers per the ROADMAP (B) WAL-replay precedent (S5g/S5k).

**PIT.** (B) idempotent + catalogued; the table carries `valid_from` so definitions are bitemporal from day one.

**Accept.** `formula_registry` exists, is catalogued, and is seeded (S4-1 populates rows). A fresh in-memory DuckDB migrates cleanly through `0076`.

### S4-1 — Port the 53 existing codes into registry rows (byte-identity gate)

**Root cause.** The 53 formulas are lambdas; porting them to data is the risk surface (a mistyped coefficient or flipped operand silently drifts a value).

**Fix.** `db/formula_library.py` holds the seed catalog: one row per existing `ratio_code`, built by translating each `RatioDef` mechanically — `operands` lambda → `numerator_item_ids`/`denominator_item_ids` + `transform`; `require_positive_denominator` → `is_meaningful_rule`. `fundamental_ratios.py` is refactored so `RATIO_DEFS` is *derived from* the registry (loaded via `formula_library.py`) rather than hand-written; `compute_ratio_rows` consumes registry-backed defs and produces identical output. The 4 composite scores become multi-term registry entries whose `expression` reproduces the exact coefficients from the current lambdas, **each WITH a citation:**

- `altman_z_double_prime` — Altman (1995), *Z''-score* emerging-markets / non-manufacturer variant: `6.56·WC/TA + 3.26·RE/TA + 6.72·EBIT/TA + 1.05·(book equity/TL)` (book equity, no price input; OI is the EBIT proxy). Cite Altman, Hartzell & Peck (1995).
- `piotroski_f_score` — Piotroski (2000), *F-score*: nine YoY binary strength signals summed 0-9 (4 profitability, 3 funding/liquidity, 2 efficiency). Cite Piotroski, *J. Accounting Research* (2000).
- `ohlson_o_score` — Ohlson (1980), *O-score*: nine-term bankruptcy-probability logit (GNP-deflator term omitted; a rank-preserving additive shift). Cite Ohlson, *J. Accounting Research* (1980).
- `beneish_m_score` — Beneish (1999), eight-variable *M-score* for earnings-manipulation risk (DSRI, GMI, AQI, SGI, DEPI, SGAI, TATA, LVGI). Cite Beneish, *Financial Analysts Journal* (1999).

**PIT.** (D) `compute_ratio_rows` stays pure; the registry only changes where the definition is read from. `input_codes_json` still records the same input list.

**Accept.** THE GATE: rebuild the 53 codes from the registry over the existing `test_fundamental_ratios.py` fixtures and assert row-for-row, value-for-value, `ratio_id`-for-`ratio_id` equality against the pre-refactor output (a golden snapshot captured before the port). Green = the port is sound.

### S4-2 — Expand families (each: registry row + citation + known-value fixture)

**Root cause.** Whole Compustat/FactSet derived families are absent; the current 53 stop at single-operand ratios and 4 scores.

**Fix — add, as additive registry rows only:**

- **DuPont decomposition** — `roe_dupont` as `net_profit_margin × asset_turnover × assets_to_equity` (a 3-term product `expression` over existing item ids; identity-check against `return_on_equity`); plus the 5-way extended DuPont (tax burden × interest burden × operating margin × turnover × leverage) where PF-S3 tax/interest concepts exist. Cite the DuPont identity (Brealey-Myers, standard corporate-finance text).
- **Coverage ratios** — `ebit_interest_coverage` (EBIT/interest), `ebitda_interest_coverage` (EBITDA/interest), `fixed_charge_coverage` ((EBIT+lease)/(interest+lease)), `cash_interest_coverage` ((OCF+interest+tax)/interest). Standard credit-analysis definitions (Graham & Dodd / Moody's methodology).
- **Accruals (Sloan)** — `total_accruals` and `working_capital_accruals` (ΔWC − Δcash − Δcurrent-debt + Δtax-payable, scaled by average assets). Cite Sloan, *The Accounting Review* (1996).
- **Cash-conversion cycle** — `days_sales_outstanding` (DSO), `days_inventory_outstanding` (DIO), `days_payables_outstanding` (DPO), and `cash_conversion_cycle = DSO + DIO − DPO`. Standard working-capital-management definitions (needs PF-S3 payables concept for DPO).
- **Per-share suite** — `eps_basic`, `eps_diluted`, `sales_per_share`, `cash_flow_per_share`, `fcf_per_share`, `tangible_book_value_per_share` (extends the lone existing `book_value_per_share`).
- **Additional quality / distress** — `montier_c_score` (Montier's six-flag earnings-quality C-score; cite Montier, 2008); `dechow_f_score` **only if** its inputs exist post-PF-S3 (cite Dechow, Ge, Larson & Sloan, 2011) — gate on input availability, do not emit a partial score.

Each new formula lands with: a `formula_registry` row (with `citation`), the item-id references (resolved through PF-S1), and a **known-value unit fixture** — a hand-computed expected value from a tiny synthetic statement so a wrong sign or unit fails immediately.

**PIT.** New formulas obey the same `available_at = max(inputs)` rule; YoY/average-balance additions extend `GROWTH_PRIOR_KEYS`, reusing the untouched 350-380-day pairing.

**Accept.** Every new formula has a registry row + citation + passing known-value fixture; none changes an existing code's output (S4-1 gate still green). No price-based multiple is added (that is PF-S6).

### S4-3 — Formula catalog surface (as-of / queryable)

**Root cause.** "What is EV/EBITDA?" (or `ebitda_margin`, or the Altman coefficients) is answerable today only by reading Python.

**Fix.** A read surface over `formula_registry` — an `asof`-style reader / view exposing `formula_code → family, kind, unit, numerator/denominator item ids, expression, citation, is_meaningful_rule, valid_from`, filterable by family and as-of `valid_from`. The definition of every ratio is now data you can `SELECT`, not a closure. (The view is catalogued like a table per (B)/S7a.)

**PIT.** (A) the reader honors `valid_from ≤ as_of_date`, so a formula's *definition history* is queryable — the hook PF-S8 uses for vintage-aware ratio history.

**Accept.** A query returns the full definition (with citation) of any formula code as-of a date; the surface is catalogued and covered by a test.

---

## Sequencing & expected compounding

S4-0 (table) + S4-1 (port under the byte-identity gate) land **first** — establish the registry and prove it reproduces the 53 codes exactly before adding anything. Then S4-2 (expand the families) — every addition is additive and independently fixture-checked. Then S4-3 (the queryable surface) — once rows exist, expose them.

Compounds into: **PF-S6** consumes the registry to add valuation multiples as new families whose numerator/denominator item ids include a price/market-cap input (the registry is already shaped for multi-item numerators); **PF-S8** consumes `valid_from` to serve vintage-aware ratio history ("this ratio's *definition* as-of that filing").

---

## Risks / guardrails

- **Risk: the port silently drifts a value.** Mitigate: the S4-1 byte-identity regression over the existing 53 codes (golden snapshot captured pre-refactor; row/value/`ratio_id` equality). This is the sprint's load-bearing test.
- **Risk: a new formula ships with a wrong sign or unit.** Mitigate: a per-formula known-value fixture (hand-computed expected) plus the required `citation` forcing a source of truth for the definition.
- **Risk: the `expression` grammar becomes an arbitrary-code hole.** Mitigate: a fixed, closed dispatch table (no `eval`), unit-tested against each score's known coefficients.
- **Do not add price-based multiples here** (P/E, P/B, P/S, EV/EBITDA, EV/Sales, market cap) — PF-S6 owns the price join; adding a price input to the wide frame in this sprint touches PF-S6's region.
- **Do not edit the PF-S1 item dim** or the hard-coded input maps' *replacement* semantics beyond consuming the resolver — item ids come from PF-S1.

---

## Bench / acceptance

- Every existing **and** new formula has a `formula_registry` row + `citation` (scores/academic formulas non-empty) + a unit test.
- The engine (`compute_ratio_rows`) reads the registry; no formula lambda remains hand-authored in `fundamental_ratios.py`.
- The **53 existing codes rebuild byte-identical** (row/value/`ratio_id`/`is_meaningful`/`available_at` equality vs the pre-refactor golden).
- Formula count materially up: **53 → ~80** (DuPont ×1-2, coverage ×4, accruals ×2, cash-conversion ×4, per-share ×6, quality/distress ×1-2 — landed as inputs allow, each additive).
- `python -m pytest atx-impl\db\tests\test_formula_library.py atx-impl\db\tests\test_fundamental_ratios.py -q` green (and the full `atx-impl\db\tests -q` suite green before commit).
- `PARITY_GAP.md` status updated; a ledger row appended to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, migrations `0075-0078`, formula count 53→N, verification commands, live-DB smoke with exact ratio-code count + `run_id`, caveats/next).
- Never `git add -A` — stage explicit paths. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
