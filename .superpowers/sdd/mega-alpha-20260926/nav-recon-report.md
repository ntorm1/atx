# NAV recon report: TRAIN v6 blend, baseline-v1, c1 f1 (nav-recon investigator, 2026-09-27)

Inputs read: `atx-impl/src/strategy_nav_replay.cpp`, `strategy_target_replay.cpp`, `nav-backtest-design.md`,
`build-equity/mega-nav-train-v6-c1/{recipe,summary}.json`, `daily_*.csv`, `events_*.csv`, the role and blend
manifests, and `studies/{proto_neutral,proto_neutral2,compose_study,construct_study}.py`. I read no real price or
signal payloads and ran no real-data processes. The only data I analysed are the small NAV output CSVs.
Deliverable: `studies/nav_recon.py`. Root runs it.

## 1. Verdict

**Status: DONE_WITH_CONCERNS. A data defect, realized by a declared NAV policy, dominates the loss. I found no NAV
coding defect.**

- One session decides the TRAIN result. On **2021-01-04** S1 gross was **-26.25%**, which is 62% of S1 gross
  variance. On that day the adjusted marks of **61 held names** realized **-$247.3M**, while their raw closes moved
  only about ±2%.
  - The close/raw factor jumps are exact split ratios: x10, x25, x50, x80, x1/4, x1/6, x1/8, x1/10. For example,
    `1816199` short had r_adj = +197.8 and r_raw = +0.013; `5788268` short had r_adj = +82.3 and r_raw = +0.041.
  - This means `close.f64` in the TRAIN role (`f64(raw-f32-close)*f64-cumulReturnFactor`) has a
    `cumulReturnFactor` discontinuity at the 2020-12-31 → 2021-01-04 boundary. The factor looks anchored
    differently in the two vendor segments.
- The NAV realizes this discontinuity on purpose.
  - The guard marks the intervals, but `realize()` still books the adjusted ratio. See strategy_nav_replay.cpp:295
    and :308-315, and design §5: "Guarded intervals: realize adjusted return".
  - The summary's `raw_minus_adjusted_pnl_sensitivity_dollars = +$247.5M` is exactly this loss.
- The other methods never saw it:
  - The target replay counts guarded intervals as missing (strategy_target_replay.cpp:171-175).
  - compose/construct_study exclude guarded cells.
  - proto_neutral nulls |r| > 0.8, which removes the x10 to x80 names.
- **Without that day, the NAV agrees with every earlier diagnostic.**

  | Scope | Gross ann | Vol | Gross SR |
  |---|---|---|---|
  | S1, ex 2021-01-04 | +3.67% | 11.8% | +0.31 |
  | S2, ex 2021-01-04 | +6.9% | 13.3% | +0.52 |
  | S1, outside 2021-01-04..04-30 | +4.78% | 12.3% | +0.39 |
  | S2, outside 2021-01-04..04-30 | +5.35% | 13.0% | +0.41 |

  The earlier target proxy gave +0.36 and the prototype 10.4% vol.
- **The "2x vol" is the same day.** One -26.25% day adds 0.2625² × 252/754 ≈ 0.023 per year of variance, and
  sqrt(0.118² + 0.023) = 0.192. That is the reported 19.2%. The mean works the same way: +3.67% - 26.25%/3 ≈
  -5.1%/yr, which is the reported value.
- **The S1 vs S2 gross gap (-0.27 vs -0.09) comes from the same event, amplified by S2's participation cap.**
  - The factor jump inflated a few phantom shorts. The largest is `1816199`, which went from -$0.6M to about -$122M.
  - S1 is uncapped and traded them back the next sessions. Turnover was 0.47 on 01-04 and 0.38 on 01-05, the
    second partly from re-scaling decision-NAV dollars after the loss.
  - S2 is capped at 1% of ADV and could not. S2 net leverage was **-0.336** on 2021-01-04, still -0.10 at the end
    of April, with gross up to 1.74 and $273M unfilled.
  - **84% of the cumulative S2-S1 gross difference (+8.1pp of +9.7pp) accrues 2021-01-05..04-30.** Outside that
    window S1 and S2 gross SR are 0.39 and 0.41.
  - S1/S2 daily gross correlation is 0.98, with the top-10 disagreement days all in Jan-Mar 2021.
  - So S2's "better" 2021 is idiosyncratic P&L on accidental ~$100M+ positions, not signal.
- A second, S3-only data artifact: on **2020-06-30**, 229 held names were absent for one session. S3 (K=1, adverse)
  wrote off 228 of them for **-$31.7M**, and 227 reappeared on 2020-07-01. This is a vendor presence hole, not
  delistings. S1 and S2 (K=5) carried them and were unaffected.
- The remaining gap to the prototype's 10.4% vol (NAV 11.8-12.3%) is plausibly construction. The NAV here is
  c1 f1 (full daily reset), while the prototype and the +.36 target proxy are c5 f.25, whose averaging lowers vol.
  Section B of the script measures this directly with the `proxy c5 f.25` line against the `study c1f1` line.

## 2. Code-reading findings (pool-2 @ 71cbec8f)

| Area | Finding | Where |
|---|---|---|
| Loop order | MARK (t>begin) → EXECUTE (begin<t≤end-2) → DECIDE (t+2<end) | strategy_nav_replay.cpp:458-486 |
| Timing | Decide at d: `current = held/nav_post`, order = `planned*nav_post` in decision-NAV dollars (:411). Fill at close d+1; a complete fill lands exactly on the order (:369). Mark at d+2 realizes `h*close[d+2]/mark` with mark = close[d+1]. **A decision at d earns close[d+2]/close[d+1]-1.** This matches the target replay (entry d+1, endpoint d+2; strategy_target_replay.cpp:199) and the studies (`fwd[d] = r[d+2]`). No off-by-one and no doubled lag. | :398-423, :350-385, :292-317 |
| Price for marks and fills | Adjusted `x.close` is used for held names (:294-295, :315) and flat names (:272). Raw is used only for the guard test, events and ADV dollars (:217). With a continuous factor this is correct total-return marking. With a broken factor, positions become phantom dollars. | as listed |
| Held-name returns | Cumulative from the last observed adjusted mark, so gaps are covered (:295). The borrow basis is pre-mark shorts x calendar days/365 (:326). | :295, :326 |
| Guard | `guarded_move` is identical to the target-replay guard (:169-172). The NAV only counts it, logs an event and adds `h*(r_raw-r_adj)` to the sensitivity (:308-314). It never changes the realized P&L. **This is the policy defect: correct to the design, unsafe for this vendor factor.** | :195-200, :308-314 |
| Stale / write-off | `carry_absent` / `mark_flat` follow design §5. S1 and S2 use eta=0, so write-off P&L is 0. 415 write-offs, gap-resolved +$2.2M, reappeared -$0.12M. Immaterial for S1/S2. S3: see §1. | :277-290, :257-274 |
| Target construction | NAV calls the same `desired_target` / `update_weights` through detail wrappers (strategy_target_replay.cpp:613-621). With c1 f1, planned = desired for members and 0 for non-members, the same as the target replay. Declared differences only: drifted marked weights as `current`, decision-NAV-dollar orders (hence gross 1.357 at the 2021-01-04 close after the -26% day), and the last two decisions dropped. | :386-393, :402-407 |
| Gross scaling / sign | S1 mean gross leverage 1.001 and net about 0 (max \|net\| 0.0027). Ex-worst-day gross is positive and correlated with the proxy history. No sign or scale defect. | CSV / summary |
| Identities | r = gross - cost - borrow, and cash + holdings = NAV, both to 1.3e-13. They pass because the phantom P&L is internally consistent. | :341-345, :439-442 |

**Confirmed by code plus output:**

1. **DATA (root cause).** TRAIN role `close.f64` has a split-ratio factor discontinuity at 2021-01-04 for at least 61
   held names. Evidence: 61 `guarded` rows at session_ns 1609718400000000000 in `events_linear-6bps-stale5-v1.csv`,
   all with r_raw of about 0 and r_adj at split ratios.
2. **NAV POLICY (amplifier).** strategy_nav_replay.cpp:292-317 realizes guarded intervals at the adjusted return,
   as declared in the recipe "guard" field and design §5. It turns the data defect into -26% of NAV, and under caps
   into months of phantom exposure. The same data is harmless in the target replay and the studies because they
   exclude guarded intervals.

**Found clean:**

- Timing and lag
- Mark series choice, given a sound factor
- Stale-carry and write-off accounting
- Baseline-v1 target construction
- Gross scaling and sign

## 3. Daily-CSV characterization (S1 unless stated)

**Worst and best days**

- Worst five: 2021-01-04 -26.25%, 2020-11-09 -4.47% (vaccine-rotation day, real), 2020-05-18 -3.47%,
  2022-11-10 -3.14%, 2020-03-19 -2.62%.
- Best day: +2.26% (2020-06-10).

**Variance concentration.** The top day holds 62% of S1 gross variance (S2 57%), the top 5 hold 67%, and the
top 10 hold 70%.

**Year split** (gross sum; SR). S1 and S2 ex-worst-day SRs are those given in §1.

| Year | S1 gross sum | S1 SR | S2 gross sum | S2 SR |
|---|---|---|---|---|
| 2020 | -1.85% | -0.13 | -1.64% | -0.12 |
| 2021 | -20.64% | -0.76 | -13.26% | -0.46 |
| 2021 ex 01-04 | +5.6% | | +13.0% | |
| 2022 | +7.21% | +0.54 | +9.30% | +0.65 |

**Turnover and leverage**

- Executed turnover mean 4.35%/day, median 3.8%, p95 6.2%. Deployment is 1.00 in S1; S2 takes 0.67 / 0.17 / 0.10
  over its first sessions because of the cap.
- Leverage (S1): gross 0.98-1.36, net within ±0.003.

**Stale names.** Mean 2.6 and max 229 (2020-06-30 hole). Max stale gross fraction is 8%. Write-off P&L is 0 (S1).

## 4. The script: `studies/nav_recon.py`

Python 3.12 + numpy only. Refuses any session at or after 2023-01-01. It reads only the TRAIN role, the TRAIN
blend `train_combined.*`, and the NAV `recipe.json` / `daily_*.csv`.

**Test results (synthetic only):**

- The mirror reproduces C++ fixtures 2, 3 and 4 (hand-computed drift/cost/borrow, gap carry, K=1 adverse
  write-off) to 1e-14 or better.
- Guard policies and ties are unit-checked. The fixture harness is in my scratchpad and is not committed.
- A full-size synthetic run (D=1155, N=5627) took **7.1 s at 453 MB peak RSS**.

**Command** (all defaults point at the paths in the brief; runs from any cwd):

```
python C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/studies/nav_recon.py
```

Options: `--role`, `--blend`, `--prefix train`, `--nav`, `--no-s2` (skips the volume load and the S2 mirror),
`--top 10`, `--f-window 2021-01-04:2021-04-30`. Output is about 50 lines.

**What it computes**

- `W[d]`: the baseline-v1 desired target at each scored decision. It uses the same tied-rank, demean and gross-1
  operations as `desired_target`, on blend members.
- **mirror**: an independent numpy re-implementation of the NAV book for S1 (flat 6 bps) and S2 (sqrt + 1% ADV
  cap, liquidity from the [t-63, t) window). It covers MARK/EXECUTE/DECIDE, decision-NAV dollar orders, stale carry
  K, write-offs, and borrow on pre-mark shorts x days/365. It runs three guard policies:
  - `adj`: the C++ behaviour
  - `raw`: realize the raw-close return on guarded intervals
  - `zero`: realize nothing on guarded intervals
- **fixed-weight series**: g[t] = W[t-L]·r[t], with guarded cells excluded (the study / target-proxy convention) or
  realized.
  - L = 2 is the common NAV/study timing.
  - L = 1 (same-close fill) and L = 3 (late fill) are the off-by-one probes.
- **proxy c5 f.25**: the target-replay observed component at its default cadence and fraction. It should reproduce
  the earlier "+.36".
- **factor scan**: per session, the count of present-adjacent names where d log(close/raw) jumps.
  - "big": > .10 and |adj| > |raw| + .10
  - "small": .01-.10 and |adj| > |raw| + .01
  - Plus held target |w| on flagged names and the unflagged factor P&L (includes legitimate dividends; look for
    spikes).
- **S2-S1 attribution** from the CSVs.

**What each section discriminates**

| Section | Question | NAV correct + data defect (expected) | NAV defect |
|---|---|---|---|
| A | Does the C++ NAV do what its design says? | mirror S1 adj vs NAV S1 gross corr ≈ 1.000000, TE and max\|diff\| ≲ 1e-8, same mean turnover, net max\|diff\| tiny. S2 is close, but not bit-level because sigma comes from cumsums. | Large diffs on specific dates. The dates, and section D names, locate the defect (timing, stale, scale). |
| B: lag probes | Off-by-one timing? | corrNAV1 (ex worst) highest for lag2 (≈0.9+), low for lag1/lag3 | lag1 or lag3 correlates best |
| B: guard policy | How much is the guard policy? | mirror raw/zero: gross SR ≈ +0.3 and vol ≈ 12%, vs adj -0.27 / 19% | raw ≈ adj, so the loss is not the guard |
| B: study vs mirror | Is NAV machinery (drift, scale, stale) the issue? | `study c1f1` (guard-excl) tracks `mirror raw` closely (corr ex worst ≈ 0.95+, similar SR) | Large residual means the machinery differs from the simple convention |
| B: proxy | Construction effect | `proxy c5 f.25` ≈ +0.36 (reproduces T replay). Its vol vs `study c1f1` vol shows how much of 11.8% vs 10.4% is c1f1 vs c5 f.25. | If it does not reproduce +.36, my loader/target differs from the earlier runs (check first) |
| C | Where is the loss by year? | 2021 flips from strongly negative to positive under guard=raw | Loss spread across years |
| D | Which days and names drive NAV-study disagreement? | 2021-01-04 with guard names (1816199, 5788268, 5183141, …), then 2021-01-05 (lev(t-1) 1.36), then small scale/stale days | Unexplained large days with "top" (non-guard) names |
| E | One-off or systematic? | 2021-01-04 dominates "big" (≥61 held, likely more universe-wide); other sessions ~0; "big cells excluding the top session" small | Many sessions with big jumps, or recurring small unflagged jumps at year starts |
| F | Why does S2 differ from S1? | Most of S2-S1 inside the window; S2 net lev strongly negative there; outside, S1 ≈ S2. mirror S2 raw ≈ mirror S1 raw minus the small deployment-cap effect. | Gap spread evenly |

Year-start rows (2019-01-02, 2020-01-02, 2021-01-04, 2022-01-03) are printed whatever they show. A 2019 or 2020
jump would contaminate the warm-up features only, not NAV P&L.

## 5. Recommendations for root and owner (no validation data read)

1. **Fix the data first.**
   - Rebuild the role close with a continuous factor. Re-chain `cumulReturnFactor` across vendor segments, or build
     adjusted closes from raw plus `returnFactor`.
   - Add a builder integrity gate (`prepare_recent_research.py`): per-session count of big factor jumps as in
     section E; flag or refuse above a small threshold. This is data integrity, not selection.
   - Then rebuild the TRAIN fields, IC and blend. Price features of the affected names are contaminated across the
     boundary for up to their lookback (≤252 sessions).
   - Forward returns in the IC runner are guard-excluded, so orientations are probably only mildly affected.
2. **Declare a safe guard policy in the NAV before any validation run.**
   - On a guarded interval, realize the raw-close return (the script's `raw`). Keep the sensitivity and events.
   - This never books a factor move the raw tape does not show. Real large moves (raw == adj, e.g. -81%) are
     unchanged.
   - Alternative: `zero`. The mirror reports both.
   - It is a correctness fix for a named data defect, set on TRAIN and before validation. Changing it after seeing
     TRAIN results should still be recorded as an owner ruling.
3. **S3:** disclose that K=1 treats the 2020-06-30 one-session vendor hole (229 names) as 228 delistings (-$31.7M,
   227 reappeared next session).
4. **Validation (2023-2024) may carry the same boundary artifact** at 2023-01-03 or 2024-01-02. I did not look,
   per the lane contract. Either the owner allows a pure integrity scan of the validation role (section E only, no
   returns or signals), or root relies on recommendation 2 declared beforehand.
5. The "binding constraint = alpha quality" conclusion is unchanged. Once the phantom day is removed, the v6 blend
   at c1 f1 is about 0.3-0.4 gross SR on TRAIN, before 4.5%/yr flat-300 borrow and about 2%/yr cost.

## 6. Housekeeping

- I did not commit the new files (`studies/nav_recon.py` and this report) in pool-2. pool-2 is the controller
  worktree on `feat/aes-codex-integration-20260925`, so root should commit them if wanted.
- No builds and no subagents were used.
