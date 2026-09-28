# Task V6 whole-branch adversarial review (mega-alpha v6, b1887951..HEAD ce10f008)

Reviewer: Opus 5.5, read-only. I did not edit (except this file), build, run a binary, switch branches or touch
C:/atx. Inputs: review-v6-branch.diff (all 23 code files, read in full for the C++ and the fitter, skimmed for the
Python tests and generated recipe), `.agents/cpp/agent.md`, the seven per-task reviews/re-reviews, progress.md
(top ~210 lines), studies/v6l_train.sh and studies/nav_summ.py. I also read the pool-2 source around every hunk,
and read (only) receipts, recipes, summaries and the S2 daily CSV of the build-equity cells named below with small
Python one-liners. Nothing was written under build-equity. I build on the per-task reviews and do not repeat them.

**Counts: 0 Critical, 1 Important, 9 Minor.** Verdict at the end: FIX REQUIRED (evidence only, no code change).

---

## 1. The code paths the accepted stack exercises (traced; no defect found)

Accepted stack: library v6 x ew-theme-v1 (weights b900602d) x aim-partial-v5 theta .05 dust .1 fixed rate,
`--cadence 1 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache --neutralize price-risk-v1`.
Measured cell: `build-equity/mega-nav-v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc`, produced by NAV exe **212d9e22
(v6-0)**, 42.8 s, 339 MiB. Its summary shows 754/754 decisions rebalanced, 0 neutralization skips, 75,667 zeroed
special-tier short aims (about 100 per decision), 3,406 capped fills out of 2.0 M, 1,949 absent-blocked orders,
S2 write-offs 0. `strategy_nav_replay.cpp` is abbreviated nav.cpp, `strategy_target_replay.cpp` tr.cpp.

| path | status | evidence |
|---|---|---|
| Delta orders at DECIDE | correct | nav.cpp:818-822 places `order = planned * NAVpost`, then `anchor_order` (:786-790) sets `anchor = held_d`. A zero plan, or a special-tier name under a blocking book, stays a target order. |
| Delta EXECUTE, capped fills | correct | :632-660. The request is `order - anchor`. A capped fill adds `filled` to both `held` and `anchor`. A complete fill lands on `order + (held - anchor)`, and the cash identity holds to rounding. The flag cannot go stale on a live order: every new order passes `anchor_order`, a kept order passes `clamp_kept_order` (:760-770), and a write-off clears `active`. |
| Residual carry-over, `clamp_kept_order` | correct; **not exercised** by the accepted stack | Cadence is 1 and no rebalance was skipped. So every residual is re-planned (plan changed) or cancelled (dust band, rebalance) at the same session's DECIDE, which runs after EXECUTE. A residual never reaches a second EXECUTE, and :824-825 is unreachable. The +0.975 therefore does not depend on these semantics (see disclosure D6). |
| Stale write-off | correct | `carry_absent` (:507-520) zeroes `held`, `order` and `active`. `delta`/`anchor` are then stale but never read. An absent name is a nonmember, so the same session's DECIDE plans 0 and places a target exit (tr.cpp:264 requires presence for the decay). |
| Exit-rate decay and snap | correct | tr.cpp:237-267. Present nonmember: `next = current * .95`, a delta order under the delta basis. Snap inside `dust/N_d`: planned 0, a target order, so the exit ends flat. Absent nonmember: immediate exit. N_d = 0 gives an infinite band (snap). `validate_config` refuses exit_rate outside (0, 1], or below 1 without aim-partial-v5 and dust > 0. |
| Locate-in-aim before neutralization | correct | The tiers of decision t are classified at nav.cpp:933 (fields row t visible at 22:00, decision at 23:00). The mask is filled at :936-937 before `form_desired_target` (:941). tr.cpp:369-372 zeroes the member shorts before `neutralize_price_risk`. The refusal without fields is at nav.cpp:1744-1746, and `replay_nav` also goes through it (:1780). Without that refusal, `tiers.tier` (empty) would be read out of bounds, so the refusal is load-bearing and present. |
| C2 `hold_zero` | not reached | tr.cpp:379-383 passes `no_short` as the hold on the industry branch only. price-risk-v1 calls `neutralize(.., {}, {}, ..)`. |
| Fixed-rate liquidity cache | correct | `fill_liquidity` (:449-462) runs before any book's MARK, and MARK only cancels orders, so every active and present name is formed. The debug assert at :638 cannot fire: ADV is a finite sum over validated cells. The `per_name_rates` guard became `rate == PerNameV1` (:811). With `cache.on()` it would have written into the empty `rate` vector on the fixed path, so the change is required and correct. Reserve and admission both use `liquidity_cached` (:346, :1717): C2 review I2 is closed. |
| nav_summ post-ramp (C4) | correct | Recomputed from the loc cell's S2 CSV, `mean(gross[63:])` = **0.78340**, which matches the ledger's .7834. Gross is .65 at row 40, .616 at row 63 (the March-2020 dip), .83 at row 100, and the rows 120+ mean is .7852. The 63-row cut excludes the ramp adequately. |

## 2. Findings

### Critical
None.

### Important

**I1: "D2 for C2" is recorded as satisfied, but no artifact supports it. The C2 approval's gates 2 and 3 are still open, and V6-F / V6-U will run on the v6-2 binary.**
- Where:
  - progress.md:10-11 ("their parent cell was produced by the v6-2 binary with price-risk-v1: identical statistics
    to the v6-0 cell -> D2 for C2 satisfied");
  - task-V6C2-review.md §5, gates 2-3 ("The approval holds on the root closing gates 1-3");
  - progress.md:20 (golden (a) only "expressible at base").
- What is on disk: only three NAV runs bind exe **f55537fc (v6-2)**:
  - `...-loc-nind-v1`
  - `...-loc-nind-v2`
  - `mega-nav-v6l-ew6-...-loc`, whose weights a24ca205 differ from every v6-0 cell.
- No v6-2 run of any price-risk-v1 cell on the ew-theme-v1 weights (b900602d) exists. The accepted loc cell and
  every other v6l cell bind 212d9e22 (v6-0).
- So:
  - the "identical statistics" comparison has nothing to stand on;
  - golden (a) (strategy_price_exposures_test.cpp:458) was never run against a base binary, and it is a
    Python-replica golden;
  - the end-to-end SHA A/B was never done.
- The C2 change to the price-risk-v1 path is small, and I agree it is order-neutral by inspection. C2 folded
  `neutralize_target` and `neutralize_price_risk` into one `neutralize` / `price_risk` body, moved `e[r] = target`
  ahead of `factor`, and collapsed the stats reset (strategy_price_exposures.cpp:409-494, `e[r]` at :419). That is still an inspection
  argument, which is exactly what the gate exists to replace.
- Failure scenario: a silent v1 byte drift in v6-2. V6-F (L re-derivation, paired against the v6-0 loc cell) would
  then measure "binary + L", not L. The same holds for V6-U paired against the loc cell. Either way the final
  scorecard statistic and its paired dSR would be confounded, with no way to tell afterwards.
- Fix (verification only, not a trial):
  1. Re-run the loc cell with the current binary into a new directory. For example:
     `COMP=ew-theme-v1 ORDER_BASIS=delta EXIT_RATE=.05 LOCATE_AIM=1` with v6l_train.sh `nav`, after moving the
     output name aside, or by hand with the exact recipe flags and the same combined (`mega-v6lw-train-ew-1`).
     Expect about 45 s and 340 MiB.
  2. SHA-256-compare every `daily_*.csv` and `events_*.csv` against the v6-0 loc cell. All must be identical.
  3. In `summary.json`, only `recipe_sha256` may differ. In `recipe.json`, only `exit_rule` -> `exit_rate_rule` and
     the aim_partial nonmember clause may differ.
  4. Replace the progress.md sentence with the two directories and the SHAs.
  5. If anything else differs, stop before V6-F.

### Minor
- **m1: IC-binary confound for V6-U.**
  - The ew-theme-v1 combined behind every accepted NAV cell came from IC exe **647c71a7** (pre-W):
    `mega-v6lw-train-ew-run1` and `mega-v6l-train-u2-run1` receipts.
  - V6-U's u and w passes ran on **b1c1ba07** (post-W): `mega-v6u-train-u-run5`, `mega-v6uw-train-ew-run1`.
  - The unthemed path is bit-identical by construction:
    - `blend = result.signal.data()` with the same expression;
    - `present = nullptr`;
    - `themes = 0` admits the same bytes.
  - That identity has never been checked on real data.
  - Fix, before reading V6-U: run the v6l ew weighted pass again with b1c1ba07 (`WPASS=4`, new directory; about
    35 s and 505 MiB). Then SHA-compare `train_combined.f64`, `_finite.u8` and `_member.u8` against
    `mega-v6lw-train-ew-1`.
- **m2: `v6l_train.sh:102` silently falls back to the wrong reference.**
  - `[ -d "$REFN" ] && ... && REF=$REFN`: a mistyped `REFN` quietly pairs against the v5.1 parent.
  - An unset `REFN` pairs against the v6l parent, not the loc cell.
  - `DSR_N` defaults to 14 (:78).
  - For V6-F and V6-U the paired dSR is the acceptance criterion. Pass `REFN=<loc cell>` and the correct `DSR_N`
    explicitly, and check the printed "paired vs" line.
  - Fix sketch: `exit 3` when `REFN` is set but missing, and require `DSR_N` for any cell other than the parent.
- **m3: stale limitations text under the delta basis.** `summary.json` `limitations` (nav.cpp:1124) still says
  "working orders keep decision-NAV dollar targets" when the order basis is delta. The recipe's `order_basis_rule`
  is correct. Do not change the binary before V6-F; disclose (D7) and fix afterwards.
- **m4: `v6l_train.sh` checks little of the run.** It verifies neither the bounded runner's receipt `exit_code`
  nor pipefail. Only the presence of `summary.json` is checked (:101). This is adequate only because NAV writes
  `summary.json` last on success. Optional: check the receipt `exit_code == 0`.
- **m5: `prepare_recent_research.py role --universe linked-operating-v1` flags.**
  - `--max-seconds` defaults to 300, above the 180 s rule. Pass it explicitly.
  - `--check-fields` is still optional (W m10). The lo1 manifest does carry the crosscheck: it is equal on
    6,499,185 cells, and `link_member_cells` is equal. Keep passing it.
- **m6: V6-U changes `mkt_ret`.**
  - fields-v6b is rebuilt for the lo1 role, and `mkt_ret` row d uses the role's decision membership of d-1.
  - So under V6-U the market for `bac` and `res_mom_12_1` is equal-weight over linked-operating members.
  - The inherited field caveat ("ADV top-N (ETFs included)") is false for that role.
  - Disclose (D14). The V6-U effect is universe, plus market proxy, plus re-admission.
- **m7: FMA contraction under AVX2.**
  - Every byte-identity claim holds for SSE2 builds: equity-dev (Debug, the research binary) and equity-rel (no
    /arch). Those builds have no FMA and no fast-math. Ties are broken by index, and the pooled IC bands are
    single-writer.
  - clang-cl with `/arch:AVX2` may contract `a*b + c` (fp-contract on by default). Affected spots include
    `within[k] += x*x` and the unchanged `current + theta*gap`.
  - Any future AVX2 equity build needs `-ffp-contract=off` before golden (a) or D2 can be reused.
  - Not a current defect. Debug vs Release equality is expected but was not measured.
- **m8: no test covers the full accepted stack.**
  - No gtest combines delta, exit < 1, locate-in-aim and the liquidity cache.
  - The interactions go through orthogonal state (planned / order / desired / liquidity), and I traced them.
  - Optional: a lockstep bit-identity test of cache on vs off under the full stack.
- **m9: L re-derivation policy is undeclared (V6-F).**
  - Gross is not linear in L: the dust and exit bands are absolute `dust/N_d`, and locate-in-aim rescales to the
    zeroed gross. So L = 1/0.78340 = 1.2765 will not land post-ramp gross exactly on 1.
  - Declare before the run:
    - the rounding;
    - that the derivation is single-shot (no second L cell), or, if a second cell is allowed, that it counts in
      DSR N;
    - the gross acceptance band, if any.

## 3. Cross-lane integration (C1 x C2 x W x L x fitter)

- **Reserve.** It uses `liquidity_cached(base)` (nav.cpp:1717), the same predicate as `validate_nav_input` (:346)
  and `LiquidityCache` (:905). It is charged at the pinned role geometry, and the geometry is re-asserted after
  load.
- **ConstructionDay.** The C2 fields and `locate_zeroed` are appended; no positional init exists.
  `NavReplayDay` grows about 24 B (and so does the reserve, through `sizeof`). The new fields are not CSV columns,
  so no CSV byte changes.
- **`form_desired` signature.** `no_short` defaults to `{}`. Length is checked (tr.cpp:365). The target replay
  passes an empty span, so its path is unchanged.
- **Merged help and parse.** The help text is coherent. `parse_neutralize(value, cfg.target)` sets the ind-v2
  windows, and `validate_config` refuses any other pair under ind-v2.
- **W runner gate vs NAV.**
  - The runner accepts v2 iff the block is present, and an old runner refuses v2.
  - NAV admits the themed combined (`signal_semantics` unchanged) and records `composition_redistribution` in
    `source_bindings`. I verified this in the ew6 cell's summary.
  - nav_summ `load_weights` reads `provenance.weighted_standalone_turnover`, which v2 files carry.
- **Library v6 DSL vs evaluator.**
  - `Pow` is `std::pow` in the VM, fusion, oracle and streaming paths, and in the parser's constant folding
    (parser.hpp:308).
  - `TsCountNans` is registered and typechecked, and implemented in the VM, streaming engine and oracle.
  - The coverage check after the u pass (cbop .570 >= cfoa .546) proves `pow(NaN, 0) = 1` in practice.
- **Fitter tau source.** ew-theme-v6 uses this run's own admission rows (`rows[k]["tau"]`), i.e. library-v6
  turnover, which is literal to rule (c). Note that V6-L selected its fast sleeves (decay removed) from v5.1 taus,
  while V6-W shrinks by v6 taus, so the two "fast" sets differ. ew6 is rejected, so no reported statistic is
  affected (disclosure D12).

## 4. Look-ahead / point in time: none found

- **grp_ff12.** SIC acceptance before the mark of d-1; used at row d (tr.cpp:381). The NAV loads it through
  `used_field`, which refuses `point_in_time != true`.
- **Universe classifier.**
  - Bridge qualification is `t_lo = max(first day >= start, first mark >= available_at)`, `t_hi = first day >
    end_incl`, which is correct.
  - SIC uses `advance(marks[t-1])` with 550-day staleness, the same clock as grp_ff12 (crosscheck equal).
  - class_status is the qualifying row's own value; `current_ticker_verified` rows are excluded.
- **res_mom_12_1.** `delay(., 21)` wraps the whole in-window fit (generate_fund_ic_v6.py:362).
- **mkt_ret.** Row d uses closes d-1, d and membership d-1.
- **Seasonality.** close[t-231] / close[t-252].
- **EAR.** Unchanged from v5.1; only the smoothing form changed.
- **no_short.** Uses the decision-t tiers, the same clock as the existing locate rule.

## 5. Determinism, /W4 /WX

- Lockstep books share only the construction, the tiers and the liquidity cache, and each is formed from the base
  config (all books' `liquidity_window` / `min_vol_pairs` are the base values). Each book keeps its own
  `NameState`.
- The IC theme planes are written by one band per cell, so the pooled result equals the serial one (tested).
- v6-2 compiles clean under /W4 /WX. Warnings are per translation unit, not per path, so untested branches carry no
  extra warning risk.
- build-equity is Debug, so asserts are live. The new asserts (nav.cpp:638, tr.cpp:246) are unreachable on
  validated input.
- No UB found:
  - id casts come after the range and floor checks;
  - slot indices are < kGroupSlots;
  - `exit_band = inf` is well-defined;
  - `tiers.tier` is read only when tiers are active.
- FMA caveat: m7.

## 6. RAM and time (limits 180 s / 1536 MiB; receipts)

| run | wall | peak RSS | exe |
|---|---|---|---|
| loc cell NAV (accepted) | 42.8 s | 339 MiB | 212d9e22 |
| nind-v1 / -v2 NAV (+grp_ff12, 49.6 MiB) | 34.9 / 38.6 s | 390 MiB | f55537fc |
| ew6 NAV | 35.5 s | 339 MiB | f55537fc |
| v6l u pass, cold cache | 121.9 s | 1060 MiB | 647c71a7 |
| ew6 w pass (admit estimate 2304) | 35.0 s | 1199 MiB | b1c1ba07 |
| V6-U u pass run5 (lo1, fields-v6b) | 78.1 s | 1061 MiB | b1c1ba07 |
| V6-U w pass ew | 30.0 s | 505 MiB | b1c1ba07 |

- **V6-F at L ~1.28.** L changes no allocation (`aim_leverage` only scales the aim). Role geometry is unchanged at
  5627 x 1155, so expect about 45 s and 340 MiB.
- **V6-U NAV.** Same geometry and fewer members (1675-1843 per scored day, 40.3% dropped), so no more than the
  loc cell.
- The tightest phase is a cold u pass at 122 s (68% of 180 s). The runner is resumable across bounded passes.

## 7. Could the reported statistics mislead? (inputs and definitions checked)

1. **+0.975 is at L 1 (gross .761 all rows, .783 post-ramp).** The sqrt-impact cost is superlinear, so V6-F at
   L ~1.28 will be lower (the prereg expects .94-.95). The headline must be V6-F, not the loc cell.
2. **DSR.**
   - v6l_train.sh passes one directory, so nav_summ uses the Lo single-cell variance with N = `--dsr-n`
     (flagged in its output).
   - N = 26 counts NAV cells only. It omits:
     - the 38 admission candidates (res_mom_12_1 a re-trial);
     - the library-design degrees of freedom (V6-L was written with the v5.1 TRAIN scorecard HAC t visible).
   - The independence assumption is conservative for the construction cells, which are correlated around
     rho .95. It is optimistic for the omitted signal-level search.
3. **Acceptance.**
   - The rule is sign-only, and every accepted increment has |t| < 1: library +.156 (SE .189), delta x.05 +.044
     (.125), loc +.060 (.101).
   - The cumulative +0.216 over the v5.1 parent is not statistically established on TRAIN. S2 HAC t is 1.77.
   - All numbers are TRAIN 2020-2022, in sample and max-selected.
4. **Scope of `net_sharpe`.** It includes the deployment ramp (about the first 40-60 rows, gross 0 -> .6). The
   ramp is the same across cells, so paired comparisons are fair; absolute levels include it.
5. **`mean_held_share` 1.029 > 1** is by design: decaying nonmembers linger about 58-63 sessions. The same effect
   raised net leverage before locate-in-aim.
6. **`zeroed_special_short_aims`** also counts guard-skipped decisions (C1 M2). There were 0 skips in the loc cell,
   so it is exact there.
7. **Two `recipe.json` spellings.**
   - The v6-0 cells (all 10 construction cells) carry `exit_rule`, and their aim_partial text says "nonmembers
     exit to 0" although the exit rate was .05.
   - The v6-2 cells carry `exit_rate_rule`.
   - Statistics are unaffected (ledger ruling).

## 8. Disclosures the handoff must carry (explicit list)

- **D1. Locate-in-aim rescale.** Gross is rescaled to the zeroed entry gross, 1 minus the zeroed special-short
  mass (about 100 names per decision), not to 1. After price-risk-v1, a zeroed name keeps `-fitted * scale`,
  which can be long. The shared construction applies to every book, so flat-300 and engine-tiers also lose those
  shorts. Swap-fin's post-block remains the safety net.
- **D2. Recipe key rename mid-grid.** `exit_rule` became `exit_rate_rule`, and the stale aim_partial clause is in
  the 10 v6-0 cells. Statistics are unaffected.
- **D3. Admit estimate 2304 MiB.** This is the IC runner's admission envelope for ew-theme-v6 theme planes. The
  runner's RSS cap stayed 1536, and the real peak was 1199-1257 MiB.
- **D4. hold_zero approximations (ind ids; code merged, cells rejected).**
  - The held set is `no_short && aim == 0` at entry, a superset of "zeroed" that adds exact-zero special aims.
  - With holds present, group neutrality is approximate (slot sums `h_G m_G - n_G c`).
  - A held name keeps `-(c + z-fit)`, so the unblocked books can short it slightly.
  - The small-group fallback pool can itself hold fewer than 5 names (C2 M1; nil on FF12).
- **D5. Golden (a) is a Python-replica golden.** Base equality of price-risk-v1 rests on I1's end-to-end A/B.
- **D6. Paths not exercised.** Cadence 1 with no skipped rebalance means delta residual carry-over beyond one
  session and the kept-order delta-to-target conversion were never exercised on real data. They are tested in
  gtests only.
- **D7. Limitations text.** Under the delta basis, `summary.json` limitations text says "decision-NAV dollar
  targets" (m3).
- **D8. Absent nonmember re-decay.** A nonmember that is absent at a decision gets an immediate exit order. If it
  reappears before the fill, the next decision replaces the order with a decay (C1 M4).
- **D9. Exit decay lingering.** Exit decay keeps exited names for about 58-63 sessions: held_share > 1, and more
  unhedged nonmember exposure.
- **D10. L policy.** L is derived from S2 post-ramp gross .78340 of the loc cell. Declare the rounding and the
  single-shot policy (m9).
- **D11. DSR N scope and Lo variance.** See §7.2. Acceptance is sign-only and within noise (§7.3).
- **D12. Library v6 proxies.**
  - res_mom is standardized in-window CAPM alpha against the equal-weight ADV-universe market, not the
    Blitz-Huij-Martens FF3 residual. It is a re-trial of v4.2.
  - cbop relies on `pow(NaN, 0) = 1` and is a cash-flow-statement approximation.
  - value_composite uses 3 of 4 ratios.
  - smax uses MAX1, not MAX5.
  - bac has no vol-quintile conditioning.
  - ind_mom / within_ind_mom keep the 21-session blackout.
  - The "fast" sets differ between L (v5.1 taus) and W (v6 taus).
- **D13. V6-W (rejected; code merged).**
  - `signal_semantics` is unchanged for a themed blend; `composition_redistribution` is the marker.
  - Per-name redistribution undoes the (c) shrink where the slow members are missing.
  - reversal_seasonality stays unshrunk under literal (c).
- **D14. V6-U.**
  - The rehearsal bridge is `scope_complete false`, so unbridged operating stocks drop. ADRs of linked filers stay.
    REITs stay. Royalty trusts 6792/6795 are excluded (controller ruling, before the u pass).
  - `membership_recipe` still names the ADV top-N rule.
  - `mkt_ret` and group ranks are recomputed over the restricted members (m6).
  - Admission is re-run, so V6-U = universe + market proxy + admission set.
- **D15. Binary provenance per cell.**
  - The loc cell and the construction grid: 212d9e22 (v6-0).
  - The C5 and ew6 cells: f55537fc (v6-2).
  - The ew combined: IC 647c71a7.
  - The V6-U IC runs: b1c1ba07.
  - Plus the I1 and m1 A/B results once run.

## 9. Verdict

**FIX REQUIRED, evidence only; no C++ change is needed.** Every code path the accepted stack exercises traced
correct. No look-ahead, UB or cross-lane semantic defect was found, and RAM and time fit the limits.

Required list:
1. **I1.** Re-run the loc cell on the v6-2 NAV exe (f55537fc) into a new directory. SHA-compare all `daily_*.csv`
   and `events_*.csv` against the v6-0 loc cell (they must be identical). Allow only the documented
   `recipe.json` / `recipe_sha256` differences. Replace the unsupported progress.md:10-11 sentence with the
   directories and SHAs. This must happen before V6-F is read.
2. **m1 (strongly recommended before V6-U is read).** The IC exe b1c1ba07 vs 647c71a7 SHA A/B of the v6l ew
   weighted pass.
3. **m2 / m9.** Before V6-F, pass `REFN` and `DSR_N` explicitly and declare the L rounding and the single-shot
   policy.

After item 1 passes: **MERGE-READY (owner gate)**, carrying disclosures D1-D15.
