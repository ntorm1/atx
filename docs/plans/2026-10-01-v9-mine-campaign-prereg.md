# v9 mined campaign C1 (`v9-mine-c1`): pre-registration draft for owner decision OD-7

Status: DRAFT, declared 2026-10-01 before any read. Nothing of this campaign has run. Nothing here was derived from a
return, an IC or any statistic of the 2020-2023 window: it rests on the verb's code at `20e7bd19`, the alpha registry's
metadata, the field names and units of the fields v9 manifest, and calendar arithmetic. It binds only if the owner
grants OD-7 for it. Until then v8 pre-registration rule 10 ("v8 runs no mined campaign") stands and nothing here runs.

Companion files (lane MINE-RUN, wave AG):
- `scripts/specs/v9/mine-c1.json`: this registration as data;
- `docs/plans/2026-10-01-v9-mine-campaign-runbook.md`: the commands, in order;
- `scripts/research_mine.py` (`research_cycle.py mine lock|pool|probe|plan|run`): the plumbing.

## 0. The one decision

**OD-7 (C1).** "Grant campaign `v9-mine-c1` as registered in section 2, with every row of section 1 at its recommended
value."

A different value for any row is named in the grant. Root then edits this file and the spec before the lock. The
derived numbers (budget, hurdle) follow from the formulas in section 2. Rows marked *param* are fixed by another lane's
merged output (section 3), not by the owner.

## 1. Decisions for the owner

| # | choice | recommended | reason |
|---|---|---|---|
| D1 | campaigns on research-window-v2 | **one** (C1) | TRAIN has one confirm year (2023). A second campaign would read it again: MINE-3 refuses only the same recipe, and the BY level of .10 holds per read. |
| D2 | operator grammar | **stage 1 only**: the 11 templates `rank(f)`, `rank(ts_mean(f, w))`, `rank(delta(f, w))`, w in {5, 21, 63, 126, 252}; `--stage2-generations 0` | Plan section 13 bars a free-form search over all operators. Stage 2's op-swap draws from every same-bucket built-in op plus 5 literature ops, less a 6-op deny list. Stage 1 is a closed list, written out here in full (132 expressions). It does not depend on the seed. It is the configuration `StrategyMineCampaign.RulePinsOnTheTemplates` pins (stage 2 off, no racing). Every template fits the house budget (2-3 slots, lookback at most 252). Alternative: stage 2 at the verb defaults adds 96 trials (budget 228; z 3.696). |
| D3 | mined fields | **12**: `iv_atm_63d, iv_atm_126d, ea_delay_days, ins_net_buy_ratio, ins_n_buyers, ins_n_sellers, k8_count_63, k8_days_since_any, inst_breadth_chg, inst_own_chg_q, regsho_threshold_days63, sv_offexchange_share126` | The rule in item 4 picks breadth: fields v9 rows that no book member reads and that are continuous, per name and free of scale. If a field the book reads were mined, the search would mostly re-find members, which the rho rule and the marginal term then reject at a cost to the budget. |
| D4 | budget N | **132** = 11 x 12, equal to the capacity | Any slack above the capacity only raises the hurdle. 132 is under the ceiling in force (1,000, Ruling PM4-13). |
| D5 | windows | **discover [TRAIN begin, 2023-01-01), confirm [2023-01-01, TRAIN end)** | Label rows by calendar count: 734 and 228, above the floors (504 and 200). At N 132 and F 1.55, both gates bind at the same per-row strength (t / sqrt(rows) .203 discover, .205 confirm). A split at 2022-07-01 binds at .224 (607 / 355 rows), about 9% less power. |
| D6 | pool regressors | **1: the book composite** (the source cell's saved combined signal; the K6 regressor without `--themes`) | `mine pool` builds it today. The rho rule against every member stops copies of members, and the whole mined theme takes 1/T of the book. Weakness, stated: a candidate spanned by a theme composite, but by no single member and not by the book composite, can pass. Alternative: K6's theme composites as further regressors (up to 11). That needs an exporter that does not exist (C++, one task). |
| D7 | pool members | **every member with a positive composition weight in the source cell** (at most 64) | mined-v1 checks rho "to every member", and PM5-8 says every member is checked. |
| D8 | name and date floors | `--min-names` **1,000** (the u pass's floor; precedent A2 deviation 2), `--min-dates` **128** (the verb default) | The mined IC uses the book's name floor. Under PM5-8, a member pair with no date of 1,000 jointly ranked names fails the candidate. That costs promotions only, so the risk is conservative. |
| D9 | shortlist cap | **16** (the verb default) | The BY m that reaches the confirm read is at most 16. Under PM5-9, the rho step runs over the whole above-hurdle list before the cap. |
| D10 | racing | **off** (`--race-strides none`) | 132 full reads are affordable. Racing adds an early rejection and the rung-failure path that MINE-16 is changing, and the fixture pin configuration excludes it. |
| D11 | workers | **the largest of 4, 2, 1 that fits the memory cap** (the probe) | The worker count changes no bit: the golden digest is equal at 1 and 4 workers. Time needs more workers (D13). |
| D12 | memory cap | `--max-memory-mib` = **the probe's required MiB** at the chosen workers, rounded up to 64, **at most 7,680**. Runner RSS cap 8,192 MiB (its maximum), free floor 512 MiB. | The bounded runner cannot hold a process above 8,192 MiB. The 512 MiB margin covers what the model does not count (code, DLLs, allocator slack). The probe value is a *param* (MINE-MEM). |
| D13 | time cap | **600 s** (the runner's maximum) | [est] A 3-year u pass (about 50 candidates, 1,155 dates) took 82 s at 4 workers, so C1 takes about 260 s at 4 workers and about 1,050 s at 1. A run that hits the cap is a blind re-run with more workers, or it waits for a runner change (owner ruling). |
| D14 | entry into the book | **one add-alpha wave** of all admitted members: new registry theme `mined`, tier `C+`, origin `mined`, prior sign 1 with the discover sign embedded in the DSL. **One construction cell**, accepted on paired S2 net dSR > 0 against its parent AND mechanics AND turnover not higher. | This is R-2's precedent: a library wave judged whole as one trial. The turnover criterion guards against the fast `delta(f, 5)` templates. The lowest tier and one theme bound the mined share of the book. |
| D15 | order against the v9 prior wave | **campaign after the v9 prior-class wave's cell. Pool = the last accepted cell at the campaign. The mined wave comes next, with no wave between.** | The members are then checked against the very book they enter. The prior members (1 trial each) are judged before the mined ones. |

Fixed, not choices: the rule `mined-v1` as coded with PM5-8 and PM5-9 (item 7); the family alpha .05, the confirm t 2.0,
BY .10 and |rho| .70; the counting in N (E-33, item 10); and the verb's refusals (item 12).

## 2. Registration text

Root copies this block into the v9 pre-registration with the pins, as v8-prereg.md was filled.

```
# v9 mined campaign v9-mine-c1 (declared 2026-10-01, before any read; granted under OD-7 on <date>, ruling <id>)

1. Campaign. id v9-mine-c1; rule mined-v1; the only mined campaign on research-window-v2 (TRAIN
   [2020-01-01, 2024-01-01), seal 2024-01-01). Run by `research_cycle.py mine run scripts/specs/v9/mine-c1.json`
   at a committed tree; the spec is part of this registration.
2. Role. The construction role of the source cell (item 6): train-2020-2023-lo1 (or -lo3 if B0b won), with its
   fields v9 dir. Never a --delisting-returns role (the verb refuses it from metadata, Ruling E-10, MINE-8). No
   session at or after 2024-01-01 is opened (the verb refuses a sealed role and windows past TRAIN).
3. Theme. Every mined member shares the one theme `mined` (strategy_mine_rule.hpp kMinedTheme), a new registry
   theme; no mined member enters any other theme.
4. Grammar and fields. Stage 1 only (--stage2-generations 0, --race-strides none): for each field f the 11
   templates rank(f), rank(ts_mean(f, w)), rank(delta(f, w)), w in {5, 21, 63, 126, 252}, on the decision
   membership as the cross-section mask. Fields (12): iv_atm_63d, iv_atm_126d, ea_delay_days,
   ins_net_buy_ratio, ins_n_buyers, ins_n_sellers, k8_count_63, k8_days_since_any, inst_breadth_chg,
   inst_own_chg_q, regsho_threshold_days63, sv_offexchange_share126.
   Rule that produced the list (applied at the lock to the registry of record; a field the book then reads
   leaves the list, none is added): the fields v9 rows that no registry alpha reads, less USD or share levels
   (lt, noa_lag4), categorical codes (grp_sic2, ea_time_of_day), 0/1 indicators (ea_window_pre5,
   ea_window_post3, ins_cluster_buy, k8_item_material_21), the 13F holder count (inst_n_holders, scales with
   size), and ea_days_since, inst_own_share (read by the v8.0 library wave).
5. Budget and hurdle. N = 11 x (fields) = 132, fixed now (pre-registration rule 10, Ruling E-32a); at most the
   ceiling in force (kMinedMaxBudget). Discover hurdle: f2 / F >= z(N) = -Phi^-1(.05 / (2 N)) = 3.5544, F the
   overlap factor in force at N (item 13); with today's F 1.55 a raw marginal HAC t of 5.51.
6. Windows. Discover [2020-01-01, 2023-01-01), confirm [2023-01-01, 2024-01-01): the research window's TRAIN
   bounds (never typed: "{train_begin}", "{train_end}" in the spec) split at 2023-01-01. Labels are h 21 with
   delay 1 and mature inside their window (ic_screen maturity_end = window end): the last 22 discover decision
   rows carry no label, so no label return of the discover window lies in the confirm window. Mature label rows
   about 734 and 228 (calendar count; the verb records the exact numbers), above the rule's floors 504 and 200.
7. Rule mined-v1 as coded (strategy_mine_rule.hpp at the merged head), unchanged in id:
   (a) f2 = discover sign x marginal IC HAC t (Bartlett lag 21) against the pool's regressors; shortlist every
       evaluated trial with f2 / F >= z(N), by f2 descending (canonical hash breaks ties);
   (b) PM5-9: the greedy rho step runs over the whole above-hurdle list, then the cap of 16 applies;
   (c) rho: |mean daily correlation of centred ranks| <= .70 on the discover decision rows to every pool member
       and every earlier kept candidate; PM5-8: a pair with no date of at least min_names (1,000) jointly
       ranked names, or fewer than min_dates (128) joint dates, FAILS the candidate;
   (d) confirm: one read per kept candidate, the marginal IC HAC t on the confirm window oriented by the discover
       sign, counted only on its full window (mined_confirm_defined, at least 200 label rows), read as t / F_c;
       p = Phi(-t / F_c); Benjamini-Yekutieli over the m candidates read; admitted iff t / F_c >= 2 and
       p_BY <= .10 (F_c the confirm factor in force, item 13);
   (e) the sign is frozen from discover; theme mined.
8. Pool. atx.mine-pool/v1 built by `research_cycle.py mine pool` from the source cell (item 15 of section 1:
   the last accepted cell at the campaign): regressor `book` = the cell's saved combined signal; members = every
   library candidate with a positive weight in the cell's composition weights, each its candidate-cache payload
   named by the cell's w pass summary (at most 64). Pinned by SHA-256 before the run.
9. Caps. Workers: the largest of 4, 2, 1 whose probe fits. --max-memory-mib: the probe's required MiB at those
   workers (rounded up to 64), at most 7,680. Bounded runner: 600 s, 8,192 MiB RSS, 512 MiB free floor.
10. Counting. The campaign line (kind mining-campaign, count 0; Rulings E-33, E-33a) adds 0 to the construction
    N; its registry count (the records the campaign added, at most N) is printed beside N in the Appendix A block
    and gates nothing. The mined-wave cell (item 11) adds 1 to N; each listed mined member is one admission trial
    (review C-7). The DSR's N is the construction count (pre-registration rule 3), its cross-trial variance takes
    the wave cell like any v9 cell. A campaign that admits nothing adds no cell and no admission trial.
11. Entry. All admitted members enter in one add-alpha wave on the last accepted cell, with no other wave
    accepted between the campaign and it: --id mined_<canon16> --theme mined --tier C+ --origin mined
    --prior-sign 1, the DSL as written by the verb when its sign is +1 and "(-1 * (<dsl>))" when it is -1;
    citation "mined-v1 campaign v9-mine-c1 (ledger trial <tid>)". One construction cell: accepted iff paired S2
    net dSR > 0 against its parent AND mechanics AND turnover not higher. A member the wave's gate does not
    admit with its sign leaves the wave (0 extra trials). Rejected, the wave is not retried.
12. Order of reads. (1) the runner receipt; (2) the ledger line, appended by `mine run` before anything else;
    (3) the mechanics `mine run` prints and checks (trial identity, distinct = 132, racing-rejected 0, registry
    new records = distinct, hurdle z = z(132), recipe pins and windows = this registration); (4) only then,
    in order: the promotion count, rho passes and confirm reads (campaign.json promotions), the admitted members
    (mined_members.json), and last the numeric columns of trials.csv (diagnostic, select nothing). stdout.log of
    the run (it names the admitted count) is not opened before (3) passes.
13. Factors and memory in force (bound to lanes MINE-STAT and MINE-MEM, section 3): F = the overlap factor at
    N = 132, F_c = the confirm factor, the ceiling kMinedMaxBudget, and the memory model behind the probe, as
    merged and built before the lock; campaign.json records each (hurdle, recipe).
14. Voids. As section 2a of docs/plans/2026-10-01-v9-mine-campaign-prereg.md, which is part of this
    registration: a failed run without campaign.json is a blind re-run on the same spec; a complete campaign is
    final, and void (its members never enter the book; its ledger line stays) when an input differed from this
    registration, a statistic was read before the ledger line and the mechanics, a mechanics check failed for a
    defect of the campaign, a sealed session was opened, or the pool was not the source cell's book.

Appendix A addition (on every v9 result): mined campaigns <k> (v9-mine-c1: budget 132, registry count <C>,
admitted <a>).
```

### 2a. What voids a campaign, and what does not

- **Refused before launch (nothing to void).** `mine run` refuses an open `requires`, a value still to fill, an
  unlocked or different pin, an existing output or receipt dir, a dirty tree, or a built verb whose `--help` lacks an
  option the spec passes. The verb refuses a sealed role, a delisting-returns role, windows outside TRAIN or under the
  floors, a budget under the capacity or over the ceiling, a missing pool, and a footprint over the cap, all before any
  payload.
- **Failed run, no `campaign.json` (blind re-run allowed, not a new campaign).** The causes: a runner refusal (time,
  RSS, free memory) or a verb error. The verb prints counts only and writes no statistic before `campaign.json`, so no
  statistic was seen. Root renames the output dir (and the registry inside it) to `<output>.void-<k>`, records a ruling
  in progress.md, removes the cause without reading anything new (more workers, a code fix), and runs the same spec
  again. This is v8 rule 7's blind re-run. A changed pin (for example a rebuilt verb) is a new lock, recorded in the
  ruling.
- **Complete campaign, final.** A complete campaign is never re-run with another parameter (rule 5 of v8). Its ledger
  line stays whatever follows.
- **Complete campaign, void.** Its members never enter the book. A ruling records why, and the ledger line stays with
  its registry count, because a defect line cannot name a campaign line (`check_line`). A complete campaign is void
  when any of these is found:
  1. an input differs from the registration (a pin, a field, a window, the budget, a flag);
  2. a promotion, `mined_members.json`, `trials.csv` numbers or the run's stdout.log was read before the ledger line
     was appended and the mechanics passed;
  3. a mechanics check fails and its cause is a defect of the campaign (a cause in the check itself is fixed and the
     check is re-run);
  4. a session at or after 2024-01-01 was opened;
  5. the pool was not the source cell's book (members or composite).

## 3. Parameters bound to lanes MINE-MEM and MINE-STAT

| parameter | today (20e7bd19) | whose output | how it enters |
|---|---|---|---|
| overlap factor F at N = 132 | 1.55 (one constant, checked to N = 1,000) | MINE-STAT: the table from budget band to F in `strategy_mine_rule.hpp` | the discover hurdle reads f2 / F; campaign.json `hurdle.overlap_factor` |
| confirm factor F_c | the same 1.55 (the 200-row ratio is 1.70 at 99%: MINE-FIX concern 1) | MINE-STAT: a confirm factor if one is needed | confirm t / F_c and BY p |
| budget ceiling | 1,000 (`kMinedMaxBudget`, `MINED_MAX_BUDGET`) | MINE-STAT: the largest budget the table covers | `validate` and the verb refuse a budget above it; C1 (132) is under any ceiling |
| PM5-8, PM5-9 | not coded (an undefined pair passes; the cap comes before rho) | MINE-STAT | item 7(b), (c) |
| memory model, `required_bytes` | 7,568 MiB at 1 worker, 9,360 MiB at 4, for 52 members (formula of the MINE-FIX report at C1's geometry: 1,405 x 6,100, 12 fields, 1 regressor, 0 rungs, shortlist 16, T 132) | MINE-MEM: the reduced, exact model | `mine probe` reads it from the verb's own refusal; D11 and D12 |
| rung-failure status | n/a with racing off | MINE-MEM (MINE-16) | the identity in item 12(3) gains a rung-failed count of 0 |

Today's model at C1's geometry, in MiB, for workers 1 / 2 / 4:
- 48 members: 7,306 / 7,903 / 9,098
- 52 members: 7,568 / 8,165 / 9,360
- 57 members: 7,895 / 8,492 / 9,687
- 64 members: 8,353 / 8,950 / 10,145

Each member adds 65.4 MiB and each worker 597.5 MiB. The role with its 12 fields takes 1,054 MiB. The pool takes 66 MiB
per row.

So, before MINE-MEM, C1 fits the 7,680 MiB cap only at 1 worker and at most 53 members, and 1 worker probably exceeds
the 600 s runner cap (D13). **MINE-MEM's reduction is a precondition in practice.** One more term matters: under PM5-9
the rho step holds the whole above-hurdle list (at most 132 here), not 16 signals, so the model must bound it.

## 4. Preconditions (the spec's `requires`; `mine run` refuses while any is listed)

1. The owner grants OD-7 for C1. Root deletes the line and cites the ruling id in the commit.
2. MINE-MEM and MINE-STAT are merged and built, and the runbook's fixture acceptance (step 1) passed on that build.
3. The source cell exists. `pool_source` names its files, and `inputs.role` / `inputs.fields` are its role and fields.
   The freeze gate passed if V8-F is the source.
4. The field rule (item 4) is re-applied to the registry of record. The list can only shrink, and the budget follows as
   11 x fields.

## 5. What a grant does not allow

- No second campaign on research-window-v2, and no re-run of a complete campaign.
- No stage 2, no racing and no field outside the list without a new registration.
- No read of any statistic of the campaign before the ledger line and the mechanics.
- No hidden-data read: TRAIN only. Admitted members are judged on 2024+ only under `holdout_gate.py` and an owner
  ruling.

## 6. What was read to write this (no data)

The verb at `20e7bd19`: `strategy_mine.{hpp,cpp}`, `strategy_mine_rule.{hpp,cpp}`, `strategy_mine_promote.cpp`,
`strategy_mine_trials.cpp`, `strategy_mine_pool.{hpp,cpp}`, `strategy_mine_ledger.{hpp,cpp}`,
`strategy_research_role.{hpp,cpp}` and `equity_strategy_mine.cpp`. The ledger: `research_ledger.py` and the
`backtest_integrity.py` campaign lines. Rulings and reports: `task-MINE-FIX-report.md`, `review-mine.md`, plan task H-3
and section 13, progress E-6, E-32, E-32a, E-33, E-33a and PM4-13, the brief's PM5-7..PM5-10, v8-prereg.md, and the
A2 report's root command sequence. Also: the alpha registry (48 alphas, field tokens of their DSL), the field names
and units of the fields v9 manifest (`point_in_time` all true; no value, coverage or count was read), and one 3-year u
pass receipt's wall seconds (D13).
