# T34a: breadth field check for library v5.1 (`opex_at`)

Explorer, read-only, 2026-09-27. No build, no IC runner, no data panels read. Evidence comes from manifest JSON
metadata, source code, and a static check with the v4.2 generator's own validator (`generate_fund_ic_v42.v4().parse`/
`peak_slots`, imported with `-B`; the lead-lag probe added `max` to an in-memory copy of the registry only).

## 0. Verdict

**`opex_at`: GO via a derived DSL on existing fields-v6 columns. A new `opex_ttm` producer item is NO-GO for v5.1.**

- The literal `opex_ttm` field does not exist, and neither do `xsga_ttm` or `cogs_ttm`. `at`, `sale_ttm`, `oi_ttm` and
  `gp_ttm` do exist.
- Operating costs = `sale_ttm - oi_ttm`, which is COGS + SG&A (incl. R&D) + D&A + other operating items. So `opex_at`
  can be written with no producer change: 4 extras (limit 5) and an estimated 4 slots (library max 7).
- `--plan-only` estimate: +512 B over v4 (the composition row only), giving 1,449,071,914 B = 1.449 GB = 1,381.9 MiB,
  which is within 1,536 MiB (154.1 MiB headroom). Candidate cache: 37 hits + 1 miss, because the fields sha is unchanged.
- A true `opex_ttm` item is a one-item change only on paper. It also needs a new metric, events-v3, fields-v7,
  re-bound v4.1 pins and a cold cache (§2). Use that route only if the controller rules the D&A-inclusive proxy
  unacceptable.

## 1. fields-v6 TRAIN inventory

- Manifest `build-equity/recent-fast-train-2020-2022-v2-fields-v6/manifest.json`
  - sha256 `32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8` (prefix 32565c32 matches)
  - schema `atx.research-role-fields/v1`, status complete, role `210fff96…` (1155 dates x 5627 names)
  - events input `fundamental-events-v2` manifest `74ed9a50…` (28 items)
- The 40 fields, in manifest order: si_shares, si_dtc, iv_atm_21d, iv_atm_63d, iv_atm_126d, earn_recent, shares_out,
  mkt_ret, be, at, at_lag4, lt, che, debt, sale_ttm, gp_ttm, oi_ttm, ni_ttm, ni_q, ni_q_lag4, be_lag1q, be_lag1q_lag4,
  cfo_ttm, capx_ttm, xrd_ttm, dvc_ttm, prstkc_ttm, sstk_ttm, txt_q, txt_q_lag4, shrs_q, shrs_q_lag4, noa, noa_lag4,
  sue, fscore, me_company, grp_sic2, grp_ff12, grp_ff49.
- Presence: `opex_ttm` NO, `xsga_ttm` NO, `cogs_ttm` NO, `at` YES. Also present: `sale_ttm`, `oi_ttm`, `gp_ttm`, `xrd_ttm`.
- Member coverage (`coverage.finite_member_frac`): at .596, sale_ttm .543, oi_ttm .463, gp_ttm .365, xrd_ttm .221.
  The derived `opex_at` is finite on at most .463 of member cells (bound by oi_ttm); gpa is at most .365.
- The producers agree: `prepare_research_fields.py:344-376` FUND_ITEMS and `build_fundamental_events.py:113-117` ITEMS
  hold the same 28 items, with no opex, sga or cogs item. DURATION_METRICS (`:89-93`) has `cogs`, used only as the gp
  fallback (`:648-654`), and no `operating_expenses` or `sga`.

## 2. What a real `opex_ttm` item would need (not recommended for v5.1)

Seed `atx-db/src/atx_db/seeds/statement_map.csv`. The C:/atx copy (sha `dbdf27af…`) equals the pool-2 copy; the line
numbers below are file lines.

| line | concept | canonical_metric | prio | note |
|---|---|---|---|---|
| 58-64 | CostOfGoodsAndServicesSold, CostOfRevenue, CostOfGoodsSold, CostOfServices, 2 ex-D&A | cogs | 10-60 | one concept per period (goods-only for split filers) |
| 65 | CostsAndExpenses | operating_expenses | 20 | total costs incl. COGS; mapped, never loaded |
| 173 | OperatingCostsAndExpenses | operating_expenses | 30 | total operating costs incl. COGS |
| 174 | OperatingExpenses | operating_expenses | 10 | usually EXCLUDES cost of revenue (gross-profit presenters) |
| 261 | SellingGeneralAndAdministrativeExpense | sga | 10 | |
| 190 | OtherSellingGeneralAndAdministrativeExpense | sga | 20 | residual line |
| 175 | OperatingIncomeLoss | operating_income | 10 | the only `oi_ttm` concept |

- **No CF-R re-staging is needed.** All seven concepts are in the CF-R extraction allowlist (`plan.json` of `ee099c73…`,
  268 concepts). So there is no new `DEFAULT_CF_MANIFEST_SHA256` (`build_fundamental_events.py:74`). The concepts are
  dropped only because `load_concept_map` (`:296`) keeps METRICS concepts and nothing else.
- **Loading `operating_expenses` as is would be wrong.** The seed metric mixes two definitions and ranks the
  COGS-excluding `OperatingExpenses` first, while Novy-Marx uses COGS + XSGA.
- Minimal correct item (the Novy-Marx literal):
  1. `build_fundamental_events.py:89-93`: add `"sga"` to DURATION_METRICS (seed rows 190/261; no concept collision).
  2. `:113-117`: add `"opex_ttm"` to ITEMS. It is USD through the ITEM_UNITS default (`:118`) and is not a
     ZERO_FILL item (`:120`).
  3. In `compute_items` (`:716`), after `:744`: `cogs = st.dur("cogs").ttm(anchor, TOL_ANCHOR)`,
     `sga = st.dur("sga").ttm(anchor, TOL_ANCHOR)`, then `v["opex_ttm"] = cogs + sga` if both exist, else NaN. Also
     update `fundamental_events_schema.md` and the tests.
  4. `prepare_research_fields.py:375`: add `("opex_ttm", "USD", "cost of revenue plus SG&A, trailing twelve months")`
     to FUND_ITEMS, giving 29 items and **41 fields**.
- A totals-first rule (CostsAndExpenses or OperatingCostsAndExpenses, else COGS + SG&A) needs a new seed metric in
  atx-db, which the tier1-v2 session owns: a metric cannot report which of its concepts resolved.
- Cost of this route:
  - The code-hash lock forces a fresh `fundamental-events-v3`: prepare, 4 event chunks and finalize, as for v2.
  - Then fields-v7 TRAIN and VAL runs and a T25-style audit.
  - The frozen v4.1 pins must be re-bound (fields 32565c32 becomes a new sha).
  - The candidate cache goes fully cold: the cache dir is `<cache>/<fields_sha>` (`strategy_ic_runner.cpp:921`), and
    the ledger's cold pass is 86 s / 1,012 MiB.
  - Coverage is capped by cogs (about gp_ttm's .365), and COGS stays single-concept, so it is understated for split
    filers.
  - **So it is not a one-item addition in effect: NO-GO for v5.1.**

## 3. Candidate DSLs, extras and slots

- **Extras** = compiled fields outside {close, raw_close, volume} (`strategy_ic_runner.cpp:570-581`; generator
  `generate_fund_ic_v4.py:334-341`). The per-candidate limit is 5 (`generate_fund_ic_v42.py:45,219`).
- **Slots** = the native `Program::num_slots`: the SlotPool peak, where each node acquires a slot before its children
  are retired (`atx-engine/src/alpha/bytecode.cpp:68,119`). The runner charges only the library maximum
  (`strategy_ic_runner.cpp:581,610`). The Python estimate uses the same post-order rule, and my hand trace of
  `linearize` gives the same peaks (4 and 4).

| candidate | DSL (canonical) | fields | extras | est. slots | nodes | prior bars |
|---|---|---|---|---|---|---|
| gpa (v4 ref) | `decay_linear(group_rank(((gp_ttm / at) + (0 * log(at))), grp_ff12), 21)` | at, gp_ttm, grp_ff12 | 3 | 4 | 11 | 20 |
| **opex_at (recommended)** | `decay_linear(group_rank((((sale_ttm - oi_ttm) / at) + (0 * log(at))), grp_ff12), 21)` | at, grp_ff12, oi_ttm, sale_ttm | 4 | 4 | 13 | 20 |
| opex_at + opex>0 guard (optional) | `decay_linear(group_rank(((((sale_ttm - oi_ttm) / at) + (0 * log(at))) + (0 * log((sale_ttm - oi_ttm)))), grp_ff12), 21)` | same 4 | 4 | 5 | 16 | 20 |
| opex_at (brief text; needs fields-v7) | `decay_linear(group_rank(((opex_ttm / at) + (0 * log(at))), grp_ff12), 21)` | at, grp_ff12, opex_ttm | 3 | 4 | 11 | 20 |

- The v4 library has max 7 slots, max 5 extras per candidate, and 35 declared extras, all referenced. The v4
  fundamental ratios typically use 3-5 extras and 4-5 slots. Every opex_at variant fits and none raises a library
  maximum.
- All four fields of the recommended DSL are already declared in `fund_industry_ic_v4.json` `fields`. The v4.2 check
  `referenced <= declared` therefore holds with the v4 field list copied unchanged.
- Spec for T34b:
  - id `opex_at`, theme `profitability_quality`, tier B, raw_prior_direction +1, ranking `within_industry_grp_ff12`,
    smoothing 21.
  - citation "Novy-Marx (2011, Review of Finance) operating leverage"; formula `(sale_ttm - oi_ttm) / at`.
  - domain "non-positive total assets -> NaN".
  - deviation "operating costs = revenue - operating income (COGS + SG&A incl. R&D + D&A + other operating items); the
    paper uses COGS + XSGA, excluding DP; OperatingIncomeLoss is single-concept".
  - Generator base: `positive(div(sub(F['sale_ttm'], F['oi_ttm']), at), at)`, which renders byte-identically to the
    DSL above.

## 4. `--plan-only` delta

`--plan-only` prints each role's `required_bytes`: the `admit()` budget (`strategy_ic_runner.cpp:590-638`, printed at
`:1930-1936`). The budget is 32 MiB + cells x (72 + 8 x max_slots) + composition + 512 x (d + n) +
cells x (8 x capacity + 1) + labels for h = 5, 21, 63 + the 4-worker envelope. Composition = 4096 + 9 x cells +
40 x d + 32 x n + **512 x candidates** (`strategy_ic_composition.cpp:38-52`). The TRAIN role has d = 1155, n = 5627
(cells 6,499,185), score window 399..1155, workers 4.

| library | cands | max_slots | capacity | required_bytes | MiB | headroom to 1,536 MiB |
|---|---|---|---|---|---|---|
| v4 (ledger "1.449 GB", reproduced) | 37 | 7 | 5 | 1,449,071,402 | 1,381.9 | 154.1 |
| **v5.1 = v4 + opex_at** | 38 | 7 | 5 | **1,449,071,914** | 1,381.9 | 154.1 |
| sensitivity: max_slots 8 | 38 | 8 | 5 | +51,993,480 | 1,431.5 | 104.5 |
| sensitivity: capacity 6 | 38 | 7 | 6 | +51,993,480 | 1,431.5 | 104.5 |

- The delta is **+512 B**, one composition row. A cold extra field adds no admission bytes: residency is bounded by
  capacity, the most extras of any one candidate, which stays 5. The new-field variant (3 extras) would not raise it
  either.
- A cold field costs only I/O and VM time. The derived form's fields are already referenced by v4 (sp uses sale_ttm;
  opbe and ebit_ev use oi_ttm; gpa uses at). So `loaded` stays the 35 v4 extras and `planned_loads` grows by at most 2.
- Runtime: 37 hits + 1 miss. The v4.2 u pass (37 hits + 3 misses) measured 30 s / 964 MiB, so expect 30 s or less.
- Expected plan JSON: candidates 38, max_compiled_slots 7, required_lookback 272 (unchanged), research_fields
  resident_capacity 5, candidate_cache showing 1 miss (opex_at).

## 5. Root `--plan-only` command

Run this after T34b commits `fund_industry_ic_v5.json`. The guard needs a clean tree.

```bash
cd C:/atx-wt/pool-2
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
L=atx-impl/strategies/fund_industry_ic_v5.json; LS=$(sha256sum $L | cut -c1-64)
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v6
FS=32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8
IC=build-equity/bin/atx-equity-strategy-ic.exe
"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 \
  --min-free-mib 512 --output build-equity/mega-v51-plan-run --bind $IC --bind $L --bind $R2 --bind $FD/manifest.json \
  -- $IC --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD \
  --train-fields-sha256 $FS --max-memory-mib 1536 --min-names 1000 --workers 4 \
  --candidate-cache build-equity/mega-candidate-cache --plan-only
```

Accept iff train `required_bytes` ≤ 1,610,612,736 (expected 1,449,071,914), `max_compiled_slots` 7, 38 candidates,
`resident_capacity` 5 and exactly 1 cache miss. `--output` is optional under `--plan-only` (`:1889`), and
`build-equity/mega-v51-plan-run` does not exist yet.

## 6. Optional `ind_lead_lag_w` (Hou 2007): expressible, but do NOT build in v5.1

- **Ops.** All are native: group_mean (`registry.cpp:51`), sign (`:23`), max (`:32`, MaxP), delay, rank, decay_linear.
  The generator validator lacks `max` (`generate_fund_ic_v4.py:212`, plus the v4.2 rows), so T34b would need a fourth
  registry row, cross-checked like `V42_REGISTRY_ROWS`.
- **The brief's text is not canonical.** It has unparenthesized `x * sign(...)`, `rank(me_company) - 0.7` and `a / b`,
  and the validator rejects it: "expected ')' at token 22". The canonical form has 2 extras, 21 nodes, an estimated
  6 slots and 10 prior bars:
  `decay_linear(rank(delay((group_mean((((close / delay(close, 5)) - 1) * sign(max((rank(me_company) - 0.7), 0))), grp_ff12) / group_mean(sign(max((rank(me_company) - 0.7), 0)), grp_ff12)), 1)), 5)`
- Flags:
  1. **s5 smoothing is an exemption** from the s21 default (`SMOOTHING = 21`, `:58`). It needs a `smoothing_reason` and
     an entry in the recipe's `smoothing_exemptions`. A weekly industry signal will very likely fail the v4-prior-v2
     τ_k ≤ 0.08 screen: v4.2 rejected ind_adj_rev_5 (.181) and eap (.200).
  2. **Followers do not exclude leaders.** Every industry member, leaders included, gets the leaders' return. Contrary
     to the brief, no comparison op is needed: the NaN mask `+ (0 * log(sign(max((0.7 - rank(me_company)), 0))))` is NaN
     for rank ≥ .7. But that variant, with the matched denominator of item 3, has 29 nodes and an estimated 10 slots.
     Library max_slots would go from 7 to 10, adding 155,980,440 B: 1,605,052,354 B = 1,530.7 MiB, only 5.3 MiB
     headroom, so it is not admissible in practice.
  3. **Denominator bias.** group_mean drops NaN cells, so a leader with a NaN 5-day return still counts in the
     denominator. The ratio is then scaled by N_mask / N_ret, differently per industry. Fix: use
     `group_mean((lead + (0 * r5)), grp_ff12)` as the denominator.
  4. **Leader definition.** Leaders come from a cross-sectional `rank(me_company)`, not Hou's within-industry size split
     (`group_rank(me_company, grp_ff12)`).
- Research §4.C and ruling R-5 already defer it (weekly, mid-cap followers, about 58% post-publication decay).
  Recommendation: document it, do not build it.

## 7. Concerns and notes for T34b and the controller

1. **The v4.2 pattern asserts must change.** `generate_fund_ic_v42.py:194-195` require `theme not in
   WITHIN_INDUSTRY_THEMES` and `ranking == 'cross_section'`. But `profitability_quality` is a within-industry theme
   (`generate_fund_ic_v4.py:95`), and the brief's DSL uses group_rank on grp_ff12, as gpa, opbe and roa do through
   `expression()` (`:613`). v5 must allow `within_industry_grp_ff12` for opex_at, with `total == 38` (`:217`) and the
   ≤ 5 extras assert (`:219`).
2. **Redundancy.** opex/at = sale/at − oi/at, while gpa = sale/at − cogs/at, so both share asset turnover. Expect a high
   rank correlation with gpa, and the fitter's redundancy screen may drop opex_at. It still counts as +1 disclosed trial
   (the v5.1 family).
3. **Definition.** `sale_ttm - oi_ttm` includes D&A and other operating items (impairments, restructuring). FF12 Money
   (banks and insurers, whose `Revenues` include interest) is ill-defined but ranked only within its own industry. The
   optional opex > 0 guard (§3) drops cells where OI ≥ revenue; it estimates 5 slots, within 7.
4. **Sign and tier.** Sign is +1: high operating costs to assets → higher returns (Novy-Marx 2011). Tier B per the
   brief. The mega-cap evidence is weak (JKP t 1.8), so "low priority" still stands.
5. **The literal-field route** breaks the "no new data producer" premise and byte comparability with v4.1 (§2).
