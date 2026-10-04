# Pre-registration stub: library v8x4b (fund_industry_ic_v8x4b) = v8x3b + 3 new member(s)

Written by `research_cycle.py add-alpha` (regenerated on every add into this library). Root registers it in the sprint pre-registration before any IC read of a new member. Nothing here was measured.

| id | theme | tier | prior sign | origin | trial | DSL |
|---|---|---|---|---|---|---|
| `value_composite_v49` | value | B+ | +1 | prior | re-screen (0 admission trials) | `(((group_rank(decay_linear((be / me_company), 21), grp_ff49) + group_rank(decay_linear((ni_ttm / me_company), 21), grp_ff49)) + group_rank(decay_linear((cfo_ttm / me_company), 21), grp_ff49)) / 3)` |
| `bm_v49` | value | B | +1 | prior | re-screen (0 admission trials) | `group_rank(decay_linear(((be / me_company) + (0 * log(be))), 21), grp_ff49)` |
| `net_payout_v49` | value | A | +1 | prior | re-screen (0 admission trials) | `group_rank(decay_linear((((dvc_ttm + prstkc_ttm) - sstk_ttm) / me_company), 21), grp_ff49)` |

- `value_composite_v49`: Fama and French (1992, JF); Lakonishok, Shleifer and Vishny (1994, JF); Israel, Laursen and Richardson (2021, JPM) composite value; Asness and Frazzini (2013, JPM) current price; within FF49: Ehsani, Harvey and Li 2023, FAJ (prior sign source: Fama-French 1992; Lakonishok-Shleifer-Vishny 1994; Israel-Laursen-Richardson 2021; form mean_k R(decay_linear(x_k, 21))). Formula: mean over k in {be, ni_ttm, cfo_ttm} of group_rank(decay_linear(k / me_company, 21), grp_ff12). Domain: numerators keep their sign (negative book equity, losses and cash burn rank as expensive, the literature's negative-E flag); NaN when any of the three ratios is NaN. Deviation: three yields, not four: adding ebit_ev (oi_ttm / (me_company + debt - che)) needs 8 extra fields, above the IC-runner plan budget of 5 (NEEDS_NEW_FIELD); equal-weight mean of within-FF12 ranks (the literature proposal averages within-FF12 z-scores and adds S/EV and intangible book); each ratio is decayed before its rank (v6 smoothing form, no 21-session blackout).
- `bm_v49`: Rosenberg, Reid and Lanstein (1985, JPM); Fama and French (1992, JF); robust pre-1963 (Linnainmaa and Roberts 2018), weak 2017-2020; within FF49: Ehsani, Harvey and Li 2023, FAJ (prior sign source: Rosenberg-Reid-Lanstein 1985; Fama-French 1992; form R(decay_linear(x, 21))). Formula: be / me_company. Domain: non-positive book equity -> NaN (Fama-French). Deviation: n/a.
- `net_payout_v49`: Boudoukh, Michaely, Richardson and Roberts (2007, JF) net payout yield; within FF49: Ehsani, Harvey and Li 2023, FAJ (prior sign source: Boudoukh-Michaely-Richardson-Roberts 2007; form R(decay_linear(x, 21))). Formula: (dvc_ttm + prstkc_ttm - sstk_ttm) / me_company. Domain: n/a. Deviation: net issuers (negative yield) kept.

Parent: v8x3b (`atx-impl/strategies/fund_industry_ic_v8x3b.json`), 55 members unchanged, replaced or removed: value_composite, bm, net_payout. Trials: 0 admission trial(s) and 3 re-screen(s); the cell's cross-cell N resolves from the ledger at scoring time (summ.dsr_n "ledger+1"). Spec: `scripts/specs/v8/lib-v8x4b.json`.

Ruling: <decision> -- <why> -- <cost if wrong>   (root, before `run --screen`)
