### Task T34b: library v5.1 = v4 + `opex_at` (Python, optional)

**Lane:** D · **Pool:** pool-8 · **Model:** Opus 5.5 · **Depends on:** T34a = GO · **Tokens:** GEN (+FIELDS if the item is new) · **Peak:** 0.3 GiB

**Files:** Create `atx-impl/strategies/generate_fund_ic_v5.py` (copy the v4.2 pattern `generate_fund_ic_v42.py:66-84, 96-160, 219`: verify the frozen v4 copy against pinned SHAs, append Specs, assert ≤ 5 extras), `fund_industry_ic_v5.json`; fitter `V4_THEMES` unchanged (no new theme).

- [ ] **Step 1:** Add the Spec: id `opex_at`, theme `profitability_quality`, tier `B`, prior_sign +1, citation "Novy-Marx (2011, RF) operating leverage", DSL from T34a; `total == 38`.
- [ ] **Step 2:** `--check` byte comparison; `documents()` asserts; pytest for the added spec.
- [ ] **Step 3:** Report with the root `--plan-only` command; commit `feat(strategies): library v5.1 = frozen v4 + opex_at (T34b)`.

**Acceptance (root, T39):** `--plan-only` ≤ 1,536 MiB; else T35 opens. v5.1 is a separate disclosed family (admission 38; composition +1; construction: reference cell only, +1).

---

