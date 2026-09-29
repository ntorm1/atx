# Library v7.1 = parent library + wave 2 (P4 pre-registration text; root appends it to v7-prereg.md)

Declared 2026-09-29, before any IC, TRAIN or return read of any wave-2 candidate. Read-only; nothing built or run.
**Hygiene.** P4 computed TRAIN 2020-2022 member-cell coverage and field distributions from fields-v9 payloads (no returns, no
2023+ cell). Seen and unused: post-2022 source row counts (atx-db docs), v6.1/spo-v1 cell results (progress.md). No v7
candidate has a return statistic. Budgets: the house estimator (generate_price_volume_ic96_v2 parse + peak_slots).

## 1. Candidates: 4 admission trials, appended in draft §2 rank order minus eap_8k; R1: cross-section rank

| order | id | theme | tier | raw dir | final DSL (prior_sign +1, sign embedded) | turnover [est] | score |
|---|---|---|---|---|---|---|---|
| 1 | ins_opp | ownership_flow (new) | B- | +1 | `rank(ins_opportunistic_net)` | low | .42 |
| 2 | inst_best_ideas | ownership_flow (new) | C+ | +1 | `rank(decay_linear(inst_best_ideas, 21))` | very low | .30 |
| 3 | ftd_fail | short_interest | C+ | -1 | `rank((-1 * ftd_shares_ratio21))` | moderate | .16 |
| 4 | ea_overdue | earnings_momentum | C+ | -1 | `rank((-1 * max(sign((-1 * ea_days_to_expected)), 0)))` | high (event) | .11 |

**ins_opp** (sec-ins-opportunistic-net126-cmp3y-v1, payload 1b2566dd...). Citation: Cohen, Malloy and Pomorski (2012, JF
67(3)), "Decoding Inside Information"; prior as in the draft.
- Respelling: `rank((ins_opportunistic_net / shares_out))` -> `rank(ins_opportunistic_net)`. The field is already (P - S
  shares of opportunistic insiders, trade dates t-126..t-1, visible by acceptance) / shares_out[t] (units "ratio", domain
  [-1, 1]), so the draft's own conditional applies. A second division would rank net / shares_out^2, a size tilt.
- Deviations: shares, not CMP's dollars / market cap; directors and officers on original Form 4s, 10%-owner filings dropped
  (W5a rule 1); routine = a month traded in each of Y-1..Y-3, opportunistic = each year but no repeated month.
- Shape (TRAIN): 66.6% exactly 0, 30.6% < 0, 2.8% > 0; per date median 40 buyers (min 25) vs ~530 sellers (min 383) of
  ~1,779 members. Zeros rank at ~.64 (average ties), so the member mostly shorts net sellers.

**inst_best_ideas** (13f-asof45-best-ideas-cps-v1, 043ec020...). Citation: Cohen, Polk and Silli (2010, LSE WP), "Best Ideas";
Anton, Cohen and Polk (2021 WP); crowding: Brown, Howard and Lundblad (2022, RFS).
- No respelling (field = the draft's sum over filers of max(0, w_m - mw)). Added deviation: the sum grows with breadth, and
  single-security filers (w = 1) form the tail (TRAIN p50 .54, p99 9.1, max 56.1).
- Clock: V(P) = latest deadline filing + 46 h, one session for all names (values 47-150 d old); the decay spreads the step.

**ftd_fail** (sec-ftd-sum21-over-shares-v1, fa5a48a8...). Citation: Evans, Geczy, Musto and Reed (2009, RFS), "Failure Is an
Option"; Miller (1977, JF); Autore, Boulton and Braga-Alves (2015, FinRev); "Informed short selling, fails-to-deliver, and
abnormal returns" (JEF 2016). Sign logic and harvestability as in the draft (net value expected in the long leg).
- No respelling. Denominator: shares_out (restated, 90-day lag) at the last role session <= S*(t). Numerator: CNS fail
  balances summed over the 21 settlement dates ending at S*(t), the latest fully visible one (no row = 0; 21 x mean balance).

**ea_overdue** (sec-ea-days-to-expected-yoy364-consume45-fb91-v1, 86df20c0...). Citation: Johnson and So (2018, JFQA 53(6)),
"Time Will Tell"; Bagnoli, Kross and Watts (2002, JAR).
- Respelling (the draft's alignment rule): W5a's field is signed ("sessions from t to E*; 0 = expected today; negative =
  overdue"; NaN beyond 63 sessions overdue). The predicate is 1{x < 0} = max(sign(-x), 0), replacing the draft's
  `rank((-1 * max(sign(((ea_days_since + ea_days_to_expected) - 94.5)), 0)))`; ea_days_since is dropped.
- Why: E* (earliest pending yoy_364 date > last announcement + 45 d; else + 91 d) stays at a missed date, so since + to is
  constant within a cycle (TRAIN p50 62, max 95): the draft cut fires on .011% of cells and 0 of 52,476 overdue cells.
- Density: 4.08% of finite member cells flagged, 9-369 names per date (median 34).

## 2. Static budgets (limits: 5 extra fields, 7 peak slots, 314 prior bars, 4,096 bytes): no exception needed

| id | extra fields | peak slots | DAG nodes | prior bars | bytes | draft spelling |
|---|---|---|---|---|---|---|
| ins_opp | 1 | 2 | 2 | 0 | 27 | 2 fields, 3 slots |
| inst_best_ideas | 1 | 3 | 4 | 20 | 39 | unchanged |
| ftd_fail | 1 | 3 | 4 | 0 | 31 | unchanged |
| ea_overdue | 1 | 4 | 8 | 0 | 53 | 2 fields, 4 slots, 11 nodes |

- Ops rank, decay_linear, max, sign: max/sign pass only via L7b's nincr POLICY_OPS (pool-10, uncommitted) -- a dependency.
- Library: roster +4, referenced extras 36 -> 40, families 9 -> 10 (declare ownership_flow in v7.1 only: the runner refuses an
  empty declared family; v7.0 WIP declares 9). Resident-field capacity (6), max slots (8) and lookback unchanged.

## 3. Coverage on lo1 member cells and data hazards

| field | 2020 | 2021 | 2022 | 2020-22 | distribution, TRAIN member cells |
|---|---|---|---|---|---|
| ins_opportunistic_net | .973 | .968 | .967 | .969 | §1; min -.101, max .041 |
| inst_best_ideas | .998 | .997 | .999 | .998 | > 0 on all finite cells |
| ftd_shares_ratio21 | .935 | .9995 | .9999 | .978 | 0.8% zero; p50 1.8e-4, p99 .033, max 51.9 (unit defect; rank bounds it) |
| ea_days_to_expected | .961 | .956 | .958 | .958 | < 0: 4.08%; exactly -1: .67%; 0: 1.25% |

- **FTD stale window.** 2020-11-30..2020-12-21: all member cells NaN for 16 sessions (Oct-Nov files dated 2020-12-19, 60-day
  staleness). Rule: NaN, never 0. ew-theme-v1 is "missing-or-unoriented-neutral; no-redistribution", so the member adds nothing
  and its weight does not move; 740 of 756 IC days remain (floor 250). No fill, no carve-out.
- **FTD lag, vintage.** S*(t) trails t ~22-37 d (half-month publication + 7 d); vintage_risk rows; unmapped = no fail (as is).
- **Reg SHO.** No member reads regsho_threshold_days63 (.43, NYSE pending): a 0-trial input; its rebuild changes no v7.1 byte.
- **Form 4.** Classification starts with trade year 2018 (stage 2015q1 + 3 years); windows before 2018-01-01 are NaN, and
  TRAIN windows start mid-2019 or later. No Form 4 in 365 d, FPIs and ADRs -> NaN, not 0. |ratio| > 1 -> NaN (rule 2; 0 cells
  outside the domain, 127 out of rule). Form 4 shares are not split-restated; rule 2 catches the extreme cases.
- **8-K clock.** Acceptance can trail the wire release by one session (19-21% of intraday 2.02s, and post-market 8-Ks after
  22:00 UTC), so such a name reads -1 for one session. Rule: the cut `< 0` stays; the -1 cells (.67%) are at most 16% of
  flags, and moving the cut would be a variant.
- **8-K coverage.** FPIs (6-K) and REITs without 2.02 are NaN (domestic filers only); is_primary uses a later-10-Q label (small
  presence look-ahead, W5a concern 2). COVID: the overdue share peaks at 21.7% on 2020-05-01, genuine delays, no carve-out.
- **13F.** Filer type unclassified; late amendments excluded; a NaN in the window blanks the decay for 21 sessions (.998 cov).

## 4. Runner limits: v7.1 runs on fields-v9 as-is

fields-v9 (manifest sha256 8fd00e9f44b475116f483e133c03fe031618390060b8374180278f1cd7b8769b): 63 rows vs the row cap 64
(strategy_ic_runner.cpp:737); v7.1 references 40 extras vs the library cap 64 (:511); all four fields point_in_time. No new
field, no rebuild; the 55 v8 payloads are byte-identical in v9 (ledger), so parent candidates must hit the cache (§6 Q5).

## 5. Acceptance text (mirrors wave 1)

Library v7.1 = <parent: v7.0 if wave 1 is accepted, else v6.1> + ins_opp, inst_best_ideas (ownership_flow), ftd_fail
(short_interest), ea_overdue (earnings_momentum): 4 admission trials (eap_8k dropped before any read; inst_breadth_chg,
k8_item_material_21, regsho_threshold_days63 at 0 trials). Admission v4-prior-v1 unchanged (prior sign; |rho| <= .90 by
(tier_rank, roster_order), new members last; tau .70; 250 finite days; HAC t veto -2). ew-theme-v1 unchanged (refit on lo1,
+1). One construction cell = the reference cell's construction (aim-partial-v5, L 1.247 fixed) on fields-v9; N = <N>.
**Acceptance of v7.1 as a whole:** P2 paired S2 net dSR > 0 vs <REFERENCE: v7.0 cell if wave 1 is accepted, else the v6.1
cell -- root rules before the run> (sign-only inside one SE; Memmel SE and LW CBB reported) AND R6' mechanics. Otherwise wave
2 is rejected whole, its members are not re-proposed in v7, and the ranking is not re-tuned on TRAIN. Admitted but losing
members are disclosed, never dropped one by one. The freeze still needs cell-count DSR >= .95 at N = <N> and S2 net >= 1.0,
with effective-N DSR (ONC) and PBO reported beside them.
**Theme weights.** If ins_opp or inst_best_ideas is admitted, ew-theme-v1 goes from 9 to 10 themes: each existing theme drops
from 1/9 to 1/10 (.1111 -> .1000), and ownership_flow's .1000 is split equally among its admitted members (.05 each if both).
If neither is admitted, the book stays at 9 themes. An admitted ftd_fail (ea_overdue) takes 1/(n+1) of short_interest's
(earnings_momentum's) weight, where n = that theme's admitted members in the reference cell.
**Identity before the cell:** (i) the parent's entries are a byte-identical prefix; only two theme descriptions, the
ownership_flow family and 4 field declarations change; (ii) fields-v9 pinned by the sha above; (iii) the IC pass reproduces
the parent's orientation / train_daily_ic rows byte for byte (a miss aborts, no trial); (iv) the reference cell on fields-v9
reproduces its S2 daily CSV bit for bit. SHAs per draft §3.6.

## 6. Open questions for root (with recommendations)

- Q1 Parent if wave 1 is rejected: recommend v6.1 + the four (rejected wave-1 members would mix two tests); same DSLs and 4
  trials either way. Suggested ids: generate_fund_ic_v71.py / fund_industry_ic_v71.json.
- Q2 ea_overdue one-session false flags (8-K lag): recommend keeping `< 0` (the alignment rule verbatim); disclose the .67%.
- Q3 inst_best_ideas mixes breadth and conviction: recommend keeping the draft's sum; a per-holder mean is a second variant.
- Q4 ins_opp is seller-dominated (11:1): recommend keeping the registered net measure; a buy-only leg is a later hypothesis.
- Q5 The 64-row cap binds at wave 3: recommend a lean fields manifest (referenced + new fields) over a C++ change.
- Q6 Root copies the JEF 2016 FTD authors and the draft's "t n/v" values before the freeze; this is not a variant.
