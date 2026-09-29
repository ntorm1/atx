# Brief: task F-1

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task F-1: price fields and long-lookback fields

**Files:** `atx-engine/tools/prepare_recent_research.py:71`, create `atx-engine/tools/research_fields_price.py` and its test

**Interfaces (fields, all with clock `t-1 close`, point in time):**

| field | definition | note |
|---|---|---|
| `ret_overnight` | adjusted open(t) / adjusted close(t-1) - 1 | needs OD-6 (open in the export) |
| `ret_intraday` | adjusted close(t) / adjusted open(t) - 1 | same |
| `ceq_iss_5y` | log(ME(t) / ME(t-1260)) - log cumulative gross return over the same 1,260 sessions | Daniel-Titman 2006; needs price history from 2015 |
| `coskew_60m` | coskewness of monthly (21-session) returns with the equal-weight market over 60 months | Harvey-Siddique 2000 |
| `vol_126` | mean daily share volume over 126 sessions | optional denominator of `dtc_slow` if the DSL form is refused |
| `xrd0_ttm` | `xrd_ttm` with a missing value set to 0 when `sale_ttm` is finite | numerator of `op_rd` |

- [ ] **Step 1:** tests `test_field_at_t_unchanged_when_rows_after_t_mutate` (each field), `test_ceq_iss_split_invariant`,
  `test_coskew_matches_numpy_reference`. **Step 2:** implement with `PRODUCERS` as in C-3. **Step 3:** root builds fields
  v10 = v9 + these; v9 payloads byte-identical.

