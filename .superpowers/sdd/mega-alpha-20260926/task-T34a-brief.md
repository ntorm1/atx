### Task T34a: breadth field check (explorer, read-only)

**Lane:** EXP · **Model:** Opus 5.5 · **Depends on:** T29 · **Peak:** 0.3 GiB

- [ ] **Step 1:** Confirm in `prepare_research_fields.py:344-376` / `build_fundamental_events.py` whether `opex_ttm` (or `xsga_ttm` + `cogs_ttm`) exists in fields-v6; if not, list the concept chain rows in `atx-db/src/atx_db/seeds/statement_map.csv` (operating_expenses row 65 `CostsAndExpenses`; SG&A) and the producer change needed (a new item in `FUND_ITEMS`, +1 field).
- [ ] **Step 2:** Write the candidate DSL(s) and count extras/slots: `opex_at`: `decay_linear(group_rank(((opex_ttm / at) + (0 * log(at))), grp_ff12), 21)`, sign +1 (Novy-Marx 2011 operating leverage), theme `profitability_quality`, tier B. Optional `ind_lead_lag_w` (Hou 2007, weekly): `decay_linear(rank(delay(group_mean(((close / delay(close, 5)) - 1) * sign(max(rank(me_company) - 0.7, 0)), grp_ff12) / group_mean(sign(max(rank(me_company) - 0.7, 0)), grp_ff12), 1)), 5)` — flag that s5 smoothing is an exemption from s21 and that followers should exclude leaders (not expressible without a comparison op → document, do not build unless the controller rules).
- [ ] **Step 3:** Report `breadth-check-v5.md` with GO/NO-GO for `opex_at` (GO iff the field exists or is a one-item producer addition) and the expected `--plan-only` delta (+1 extra field ≤ capacity 5).

---

