# Brief: task A-1

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task A-1: alpha registry and one generator

**Files:**
- Create: `atx-impl/strategies/alphas/registry.json`, `atx-impl/strategies/libraries/v71.json`,
  `atx-impl/strategies/generate_library.py`, `atx-impl/strategies/test_generate_library.py`

**Interfaces:**
- `registry.json` schema `atx.alpha-registry/v1`: `alphas[]` of `{id, dsl, theme, tier, prior_sign, citation,
  prior_sign_source, form, origin, notes{formula, domain, deviation}, added_in}`; `fields{name: {formula_id, origin,
  producer, clock}}`; `themes{name: text}`; `tier_scores{"A": 1.0, "A-": 0.9, "B+": 0.8, "B": 0.7, "B-": 0.55, "C+": 0.4}`.
- `libraries/<name>.json`: `{id, parent, members[], budget_exceptions[], prereg}`.
- `generate_library.py --library NAME [--check] [--plan-json PATH]`: writes the library JSON and a slim recipe
  `atx.dsl-ic-experiment/v2`; static validation comes from the K1 plan rows, not from a Python parser.

- [ ] **Step 1:** tests `test_v71_library_byte_identical` (SHA `787c802e...`), `test_plan_rows_equal_static_validation` (48
  rows), `test_unknown_theme_refused`, `test_origin_required`.
- [ ] **Step 2:** seed the registry from `fund_industry_ic_v71.json` (48 entries, origin `prior`).
- [ ] **Step 3:** implement the generator. Generators v4 to v71 stay in the tree as frozen legacy with their tests.
- [ ] **Step 4:** root: the fitter on the slim recipe writes admission.json identical to `mega-weights-v71-ew` after
  dropping the recipe pin.

