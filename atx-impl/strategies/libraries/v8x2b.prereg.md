# Pre-registration stub: library v8x2b (fund_industry_ic_v8x2b) = v80 + 2 new member(s)

Written by `research_cycle.py add-alpha` (regenerated on every add into this library). Root registers it in the sprint pre-registration before any IC read of a new member. Nothing here was measured.

| id | theme | tier | prior sign | origin | trial | DSL |
|---|---|---|---|---|---|---|
| `q5_eg_f49g` | investment_issuance | B | +1 | prior | admission trial | `group_rank(decay_linear((((((-0.029 * log((me_company / at))) + (0.516 * (cfo_ttm / at))) + (0.771 * ((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))) + (0 * log(delay(be_lag1q, 252)))), 21), grp_ff12f49)` |
| `ins_opp_buy` | ownership_flow | B- | +1 | prior | admission trial | `rank(max(ins_opportunistic_net, 0))` |

- `q5_eg_f49g`: Hou, Mo, Xue and Zhang (2021, RF) q5 expected investment growth, Table I Panel D slopes (tau = 1); S-12 FF49 financials (prior sign source: Hou, Mo, Xue and Zhang (2021, RF) q5 expected investment growth, Table I Panel D slopes (tau = 1); S-12 FF49 financials; form as in the DSL). Formula: q5_eg_f49 with the house ROE domain rule applied to the year-ago ROE as well: 0 * log(delay(be_lag1q, 252)). Domain: non-positive opening book equity now or 252 sessions ago -> NaN; non-positive me_company / at -> NaN (log). Deviation: as q5_eg (q = ME / AT, Cop = CFO / AT, dRoe vs the as-of ROE 252 sessions ago); repair of the unguarded year-ago ROE (XIMP A-1, rule 7).
- `ins_opp_buy`: Lakonishok and Lee (2001, RFS) insider purchases, not sales, are informative; Jeng, Metrick and Zeckhauser (2003, REStat); Cohen, Malloy and Pomorski (2012, JF 67(3)) opportunistic insiders (prior sign source: Lakonishok-Lee 2001; Jeng-Metrick-Zeckhauser 2003; form R(x)). Formula: max(ins_opportunistic_net, 0): net opportunistic purchases per share outstanding over t-126..t-1 (W5a field), net sellers and non-traders 0. Domain: NaN as ins_opportunistic_net; non-buyers tie. Deviation: buy leg only (Ruling W2-c's separate later trial); net within the window; shares, not dollars.

Parent: v80 (`atx-impl/strategies/fund_industry_ic_v80.json`), 50 members unchanged, replaced or removed: q5_eg_f49, ins_opp. Trials: 2 admission trial(s); the cell's cross-cell N resolves from the ledger at scoring time (summ.dsr_n "ledger+1"). Spec: `scripts/specs/v8/lib-v8x2b.json`.

Ruling: <decision> -- <why> -- <cost if wrong>   (root, before `run --screen`)
