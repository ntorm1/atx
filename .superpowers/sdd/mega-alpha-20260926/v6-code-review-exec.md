# v6 code review — EXECUTION side (combined signal -> desired -> NAV replay -> costs -> gate)

Reviewer: Claude Opus 5.5 (v6-review-exec), 2026-09-27. Worktree C:/atx-wt/pool-2, branch feat/aes-codex-integration-20260925,
HEAD b1887951. Read-only: no edits, builds, binary runs or data-pipeline runs. Evidence = code reading plus light column
aggregates (awk) over EXISTING TRAIN NAV CSVs/summaries in `build-equity/mega-nav-v5-*` (Phase-A disclosed-diagnostic ruling,
progress.md "v6 GOAL 2 START"). No 2023+ artefact was opened. Builds on task-T41-review.md; none of its findings is repeated.

Files: `atx-impl/src/strategy_nav_replay.cpp` (NAV), `atx-impl/src/strategy_target_replay.cpp` (TR),
`atx-impl/src/strategy_price_exposures.{cpp,hpp}` (PX), `atx-engine/src/book/replay_cost.cpp` (RC),
`.superpowers/sdd/mega-alpha-20260926/studies/{v5_train.sh,nav_summ.py}`, role manifest
`build-equity/recent-fast-train-2020-2022-v2/manifest.json` (ROLE).

## 0. Bottom line

1. **The cost drag is two things, not one.** At L1 (ref) S2 drag = trading .304 SR (linear .140, impact .163) + financing
   .131 SR; at L1.279 trading .325 (linear .142, impact .182) + financing .131. Financing is 30% of the drag; the
   ".43 SR cost drag" in the diagnosis mixes them. (Annualised from the S2 CSV columns trade_cost/borrow/long_financing ÷
   net vol; ref vol 3.40%, L1.279 vol 4.36%.)
2. **S2 is not too harsh.** Its lenient parts (constant 5 bps half-spread on a small-cap-heavy universe, no inter-day impact
   memory under ~20-day same-sign flow, mean ADV) roughly offset its harsh parts (spread charged on close fills, Y .6 vs
   Almgren 2005, GC fee). There is no honest room to relax it (§3).
3. **Two construction defects waste 25-35% of executed dollars, most of it at above-average cost per dollar:**
   (F1) every name with an active order trades back its full one-day price drift each day (17-18% of executed dollars in the
   reference and deployable cells); (F2) nonmembers are liquidated at rate 1 under a top-3000-by-ADV membership with no
   buffer (the planned forced stock is 19-22% of planned turnover; 19-26 fills/day are capped at the 1% ADV limit).
4. **Realistic execution-side gain is +0.05 to +0.10 net SR** (F1+F2+dust retune), and perhaps +0.1 to +0.2 if industry
   neutralisation pays off. That is not enough to take .71 to 1.0 alone: the signal side must add about +0.15-0.25 gross SR.
5. **The mechanics gate is the most fragile thing in the pipeline.** Steady-state gross at L1.279 is 1.026 (102 of 696
   post-ramp rows are above 1.05). Mean net is +.0148 against a .02 limit, and the locate rule drives it. Both limits need
   engineering before v6 levers are layered on.

## 1. Measured evidence (TRAIN, S2 = modeled-1bn-stale5-v1 + swap-fin-v1 daily CSVs; means per session)

| cell | planned τ (NAV) | planned forced (share) | unfilled | locate-blocked | executable = planned − unfilled − blocked | executed τ (NAV) | drift extra (share of executed) | capped fills/day | linear / impact bps per $ | cost/$ 2020 / 2021 / 2022 |
|---|---|---|---|---|---|---|---|---|---|---|
| ew t.05 d.1 fixed (REF) | .03117 | .00602 (19.3%) | .00402 | .00090 | .02625 | .03170 | .00545 (17.2%) | 18.9 | 6.00 / 6.95 | 14.8 / 11.7 / 12.1 |
| ew t.05 d0 fixed | .03141 | .00607 (19.3%) | .00406 | .00090 | .02645 | .03412 | .00767 (22.5%) | 19.2 | 6.00 / 6.84 | 14.7 / 11.6 / 12.0 |
| ew t.03 d.1 fixed | .02190 | .00460 (21.0%) | .00290 | .00054 | .01846 | .02444 | .00598 (24.5%) | 15.1 | 6.00 / 6.35 | — |
| ew t.08 d.1 fixed | .04115 | .00718 (17.5%) | .00491 | .00146 | .03477 | .03947 | .00470 (11.9%) | 22.5 | 6.00 / 7.53 | — |
| ew t.05 d.1 fixed L1.279 | .04160 | .00930 (22.4%) | .00681 | .00116 | .03363 | .04115 | .00752 (18.3%) | 26.1 | 6.00 / 7.64 | 15.7 / 12.3 / 12.8 |
| ew t.05 d.1 per-name | .02899 | .00584 (20.1%) | .00394 | — | — | .02920 | — | 16.7 | 6.00 / 8.36 | — |

Key contrast: moving from dust 0 to dust .1 cuts **planned** turnover by only 0.8% (.03141 → .03117) but cuts **executed**
turnover by 7% (.03412 → .03170). The difference, .0024 NAV/day, equals 445 dusted names/day × mean |h| 2.7e-4 × E|r| ≈ 2%,
i.e. exactly their one-day drift. The dust band's measured benefit is mostly suppressed drift reversion, not saved θ·gap
trades.

Other facts:
- ~2,560 fills/day against ~2,900 held names (L1.279 S2 `fills` 1,951,489 over 753 sessions).
- 56% of trading cost is impact.
- Participation p95 is .28% of ADV; the maximum is 1% (the cap).
- Neutralisation amplification: median 1.19, max 1.49; about 2,942 names are used.
- Ramp: the first 60 rows run at gross .73 (L1.279). Steady-state gross is 1.026 against an all-rows mean of 1.002.
  Mean τ over the first 20 sessions is .116 against .041 afterwards, so the reported τ mean is about 7% above steady state.

## 2. Ranked findings

| # | sev | file:line | defect | failure scenario | fix sketch | est. effect on net SR |
|---|---|---|---|---|---|---|
| F1 | **Important** | NAV:748, 759-761 (`order = planned * nav_post`, decision-NAV dollars); NAV:600 (`requested = order − held` after MARK drift, NAV:495/515); NAV:617-619; declared at NAV:1026-1029, 1055 | Orders are fixed dollar targets, so each EXECUTE trades back the full one-day drift of every name with an active order. The partial move uses θ = .05, but drift is corrected at rate 1: a daily 1-day-reversal trade at ~12 bps/$. | Measured drift extra is 17.2% of executed dollars (REF) and 18.3% (L1.279); 24.5% at θ .03, which is why lowering θ bought less than expected. It is larger in high-vol regimes (2020 cost/$ 15.7 bps). | Delta orders: at DECIDE store `delta_i = (planned_i − current_i)·nav_post`; at EXECUTE fill `delta_i − filled_i` so drift rides, and the next DECIDE corrects it at θ (GP-consistent: the state is drifted holdings). New option (e.g. `--order-basis delta`, default `target`) keeps v5 bytes. Re-derive L afterwards: dispersion drift raises gross. | executed τ −12-15%; **+0.03 to +0.05** |
| F2 | **Important** | TR:209-211 (`f64 next = 0` for `!live`, i.e. θ = 1 exit); NAV:762; ROLE `membership_recipe` = research-prior63-usd-adv-topn-v1, top_n 3000, lag 1, no hysteresis | Names crossing the ADV-rank-3000 boundary are liquidated at once while every other trade runs at θ .05. These are the least liquid members, so exits hit the 1% ADV cap: 6 + 0.6·σ·√.01 ≈ 20-25 bps/$ at σ 2.5-3%, about 1.8× the mean. Re-entries are then rebuilt slowly: a round trip. | Planned forced stock is 19.3% (REF) and 22.4% (L1.279) of planned turnover. Capped fills run 18.9 → 26.1/day and unfilled .0040 → .0068 NAV/day as L goes from 1 to 1.279 (+70% for +28% scale). My estimate: forced flow is 7-10% of executed dollars and 13-18% of trading cost (~.04-.06 SR). | `exit_rate` parameter: nonmember `next = current·(1 − θ_exit)`, snapped to 0 when `|next| ≤ dust/N_d` (TR:210-222, about 10 LOC + recipe/CLI key + fixture). Or a membership buffer in the role builder (enter ≤ 2,800 / exit > 3,200), which is heavier because it is a new role artefact. Watch S3: slower exits hold delisting-prone names longer. | **+0.02 to +0.05** |
| F3 | Important (decision) | NAV:1537-1548 (swap-fin-v1: long 40, short 20 + GC 30 / warm 100 / special 500, ACT/360); NAV:526-555 | Financing is 0.131 SR of drag. Long leg .21%/yr (40 bps on ~$0.5bn longs) = 37%. Short leg .37%/yr: GC .19%, warm .13%, special .04%. The code is correct. | The parent planned v6 against "cost drag" as if it were all trading; 30% of it cannot be cut by any turnover lever. | None in code. A 1x-gross book in a cash PB account pays no long spread (~.05-.07 SR). That is a scenario choice: pre-register it with the owner or leave it. It must NOT be the v6 route to 1.0. | 0 (disclosure) |
| F4 | Important (gate) | NAV:702-711, 757-758 (locate block applied AFTER the rule and neutralisation; no re-neutralisation) | Refused special-tier shorts leave the short leg under-filled, giving a systematic net-long bias. Locate-blocked planned flow is .0009-.0015 NAV/day. | Mean net is +.0106 (REF) and +.0141/.0148 (L1.279); max \|net\| is .031. A v6 composition with more short-interest tilt, or L > 1.3, pushes the mean past .02 and fails R6' at any SR. | Tiers are classified before the desired target is formed (NAV:868 vs 872). Pass them into `form_desired` and zero special-tier negative aims before `neutralize_price_risk`, so the regression re-balances net and beta. About 30 LOC. | ≈0 SR; protects the gate |
| F5 | Minor (gate) | T38 ruling L = 1/mean_gross(all rows); NAV:1716-1717 / nav_summ.py:173 | L is calibrated on an all-rows mean that includes the ~60-row θ-ramp (gross .73). Steady-state gross at L1.279 is 1.026; 102 of 696 post-ramp rows are above 1.05. The mean passes only because the ramp deflates it. | Any lever that raises steady-state gross by more than 2.4% (F1 drift dispersion, F2 slower exits) with L held fixed fails the gross cap. Each L re-derivation is +1 trial. | Calibrate L on post-ramp rows. Or warm-start deployment: build the book on the last pre-2020 warm-up sessions, charging the deployment cost once. This also removes the τ inflation (first 20 sessions τ .116). | 0; saves trials |
| F6 | Minor | PX:119-146 (market at PX:144 = equal-weight mean over ALL 5,627 role instruments, members or not) | Beta is measured against a micro-cap-heavy EW index, while the `low_beta` alpha uses the `mkt_ret` field. The book is neutral to the EW index, not to the cap-weighted market; residual cap-weighted beta is unmeasured. | Drawdowns in large-cap-led selloffs leak into book returns (2020 net +0.1% at the MDD). | Beta against `mkt_ret`, which exists in fields-v6, or a member-EW market; new neutralisation id. About 30 LOC. | ± small (vol ↓) |
| F7 | Minor | PX hpp:18 (vol63, ladv63 re-estimated daily); TR hpp:26 (windows not CLI-exposed); PX:409-412 (rescale by amplification, median 1.19) | Exposure z-scores jump when a large day leaves the 63-day window, and the jump feeds aim churn. 16% of target gross is projected out and the rest is scaled up 1.19×, amplifying residual noise. The four low-risk members (1/9 weight, τ .027-.089, neutralised HAC t −.46 to −1.90) are largely projected out by the vol/beta regressors. | Unquantified. The EMA filters most daily projection noise, so likely ≤5% of executed dollars. | New neutralisation id with vol126/252 and ladv252. Flag the low-risk overlap to the signal reviewer. | +0 to +0.02 |
| F8 | Minor (perf) | NAV:606-608 → 368-388 (window_liquidity, 2 log() per pair via guarded_move at NAV:348-353); cache only for per-name at NAV:840-846 | The fixed-rate path recomputes each active name's 63-row liquidity window separately in each of the 5 books: about 2.6k × 63 × 5 × 753 ≈ 6e8 iterations and ~1.2e9 log() per run. | Runtime grows linearly with books (up to 8, NAV:43). See C3. | Enable `LiquidityCache` for the fixed rate as well. Its own contract (NAV:402-407) says the result is bit-identical. | 0 SR; est. −30-40% wall |
| F9 | Minor (perf) | PX:107-146, 358-385 | Every decision re-reads 253 sessions × 5,627 instruments and takes 2 log() of each: ~2.1e9 log()/run. This is the other hot spot. | See C3. | Ring buffer of per-session logs/returns, advanced one session per decision. | 0 SR; wall ↓ |
| F10 | Minor (realism) | NAV:597-627; RC:96-111 | No minimum notional and no per-order fixed cost: ~2,560 fills/day averaging ~$16k, including sub-$1k fills. | Lenient but immaterial in dollars. | Optional min-notional rule (skip \|requested\| < $5k except exits). Also a realism fix. | ≈0 |
| F11 | Minor (reporting) | NAV:610-611; summary `unrationed_full_request_cost_dollars` | The unrationed cost re-prices each day's persisting capped residual at full size: 151.1M against 44.1M actual at L1.279. | It could be misread as a capacity cost. | Label it "sum of daily full-request quotes (residuals re-counted)" or drop it from reports. | 0 |

**Correctness checks that PASS (no finding):**
- Fill and cost timing: decide d → fill at close d+1 → the cost lands in r_{d+2} (NAV:575, 582, 631).
- No look-ahead in execution liquidity: the window is [t−w, t) (NAV:368-388). Exposures use sessions ≤ d only (PX:80-91).
- Borrow tiers at MARK t come from decision t−1: `mark_session` runs before `classify_borrow` (NAV:854 vs 868).
- Signs are correct: cash, P&L, short financing, write-off (NAV:483, 500, 546, 622).
- Financing accrues on pre-mark dollars × calendar days / day count (NAV:563).
- NAV compounding and the return identity are checked at 1e-9: observed 2.8e-16 and 4.9e-14.
- Excess-return accounting (cash 0%, no rebate) is consistent with a swap book.
- Numerics: the OLS residual (equilibrated Cholesky, pivot floor, one refinement step) is sound. The amplification guard
  (cap 5.0, TR hpp:33) is far above the observed 1.49.
- Rank → z-score weights is **not** a lever. With a Gaussian combined signal, alpha per unit gross and turnover per unit
  gross both rise about 11% under z-weights (1.13 → 1.25 × E|Δz|), so the ratio is unchanged.

## 3. Cost-model realism (S2 components)

| component | code | value | literature / comparison | verdict |
|---|---|---|---|---|
| half-spread | NAV:1557, 394 | 5 bps constant, all names | Universe is ADV top-3000 with ADV > $5M and price > $5 (ROLE), so about 2/3 of names by count are small/mid caps. 2020-22 half-spreads run ~1-2 bps (S&P 500) to multiples of 5 bps (R2000 tail); Novy-Marx & Velikov (2016, RFS) and Chen & Velikov (2023, JFQA) find effective spreads material for small-cap anomaly legs. Equal-ish weights put ~2/3 of traded dollars in that tail. | Lenient for continuous trading; harsh if fills are MOC (no spread paid). Net: neutral to lenient. |
| commission | NAV:1557 | 1 bps | Institutional $0.001-0.003/share | Realistic |
| impact law | RC:84-94; NAV:1557-1558 | 0.6·σ_d·(q/ADV)^0.5 | Square-root law with Y of order 0.5-1 (Tóth et al. 2011 PRX; Bouchaud, Bonart, Donier & Gould 2018). Larger than the Almgren, Thum, Hauptmann & Li (2005) temporary term at 1% ADV. Frazzini, Israel & Moskowitz (2018) report realised institutional costs below older academic estimates. JKMP λ = .2/ADV (NAV hpp:115) gives 10 bps at 1% ADV but ~1 bp at 0.1%, where sqrt gives ~3.8 bps. | Mid-to-conservative per trade |
| impact memory | RC (per call, stateless); NAV:609 | none: each day's impact fully reverts | θ .05 makes each position change a ~20-session same-sign metaorder. Impact relaxes only partly between days (propagator models; Bershova & Rakhlin 2013). | **Lenient**: understates impact for persistent flow, possibly by a multiple, bounded by √n |
| σ | NAV:376-387 | 63-day sample SD of adjusted daily returns, guarded | standard | OK |
| ADV | NAV:368-386 | mean raw $ volume over [t−63, t), absent = 0 | Absent = 0 is conservative. Mean rather than median is lenient after volume spikes (2021 meme names, Russell reconstitution). | Mixed, minor |
| participation cap | NAV:1558; RC:104-106 | 1% ADV per session | Binds on 1% of fills but leaves .004-.007 NAV/day unfilled; mostly exits (F2) | Conservative for capacity |
| stale handling | NAV:477-490, 1559 | carried at stale mark 5 sessions, then written off at the last mark with 0 haircut | Lenient (S3, K = 1 adverse, covers it and is negative in 13/13 cells) | Known (T33a) |
| financing | NAV:1537-1548 | long 40; short 20 + tier fee | GC all-in 50 bps is slightly above the typical ~25-40. Warm 120 bps for a single flag (e.g. mcap < $1bn alone) is harsh. The long spread is right for a swap book but unnecessary for a 1x cash-PB book. | Conservative (F3) |

Verdict: S2 is a middle-of-the-literature model. Do not pursue "the cost model is too harsh" as a v6 route. Pursue spending
less (F1, F2) and earning more gross alpha per dollar traded.

## 4. Levers, ranked by expected net-SR gain per unit effort (all are construction-only, TRAIN-only, +1 trial each unless noted)

| rank | lever | direction / magnitude (net SR, $1bn S2) | cost (files / LOC) | pure TRAIN test? | notes |
|---|---|---|---|---|---|
| 1 | Drift-preserving (delta) orders (F1) | ↑ +0.03 to +0.05 | NAV only; ~30-40 LOC + recipe key + 2 fixtures (θ=1 byte stability; delta semantics) | yes | Makes lower θ effective (the drift floor disappears); re-run the θ ∈ {.03, .05} pair after. Re-derive L on post-ramp rows. |
| 2 | Nonmember exit at θ_exit (F2) | ↑ +0.02 to +0.05 | TR:209-222 ~10-15 LOC + CLI/recipe + fixture | yes | Pre-register θ_exit ∈ {θ, 2θ}; report S3; guard against holding names with no price (the stale/write-off path already covers them). |
| 3 | Dust re-tune after 1 (`--dust-multiple`, max .5, TR:99) | ↑ 0 to +0.02 | zero code | yes | Today's dust benefit is mostly drift suppression, so tune only after lever 1. |
| 4 | Locate-in-aim (F4) | ≈0 SR; gate safety | NAV + TR ~30 LOC | yes | Prerequisite for any lever that adds short-interest tilt or L. |
| 5 | Per-name rate capped at θ (`--rate per-name-v1 --rate-max .05 --rate-min .01`, rra/λ so the median ≈ .05) | ± 0.03 | zero code | yes | Isolates "slow only the illiquid names". per-name-v1 lost .195 because liquid names ran up to 3× faster: impact 8.36 vs 6.95 bps/$, gross SR .99 vs 1.18. |
| 6 | Industry neutralisation (FF12 or FF49 dummies by within-group demeaning, Frisch-Waugh, in `neutralize_target`; `grp_ff12`/`grp_ff49` are in fields-v6) | ↑ 0 to +0.15 (vol ↓; loses the `ind_mom_12_1` bet) | PX + NAV field plumbing (reuse `load_fields`, NAV:66, 1494-1534) + TR `form_desired` input; ~150 LOC + tests | yes (existing TRAIN fields) | Biggest construction upside: idiosyncratic variance is only ~4% of book variance (2,900 names, Σw² ≈ 4.6e-4), so industry/style bets drive vol. Asness, Frazzini & Pedersen (2014, FAJ) show gains from removing industry bets. |
| 7 | Exposure smoothing (vol126/252, ladv252) (F7) | ↑ 0 to +0.02 | ~20 LOC (new neutralisation id) | yes | Cheap to fold into 6. |
| 8 | Beta vs `mkt_ret` or member-EW (F6) | ± small | ~30 LOC | yes | Fold into 6. |
| 9 | Liquidity floor / position cap as % ADV | sign unknown | TR ~30 LOC | yes | Per-name evidence says alpha sits in the less liquid names. Decide only after the v6-explore cost/alpha-by-bucket study. |
| 10 | Book-level vol targeting | ± 0.05; overfit risk (2020 regime) | NAV ~40 LOC | yes | Adds turnover and fights the gross gate. Cederburg, O'Doherty, Wang & Yan (2020, JFE) find mixed out-of-sample results for Moreira-Muir (2017). Low priority. |
| 11 | Full Garleanu-Pedersen / JKMP optimiser with a factor risk model | largest theoretical upside, unquantified | 500+ LOC + a new risk model | yes | Out of scope for one sprint. The current rule is GP with Σ ∝ I and one θ; the GP-type variants already tested (aim gains, per-name θ) lost on TRAIN. |
| — | Not levers | — | — | — | Rank → z weights (same alpha/turnover ratio); cadence > 1 (sqrt impact penalises lumpier trades); daily turnover budget (τ p95/mean is 1.39, little convexity to harvest); cash-PB financing (a scenario, not construction; F3). |

Arithmetic: 1+2+3 ≈ +0.05 to +0.10 (deployable .71 → ~.77-.81). Adding 6, if it works, gives ~.85-.95. Reaching 1.0 needs net
mu +1.3%/yr at 4.4% vol, which cannot come from cost cuts alone (trading + financing total is 2.06%/yr). The signal and
combination side must contribute.

## 5. Five most fragile points

| # | where | failure scenario |
|---|---|---|
| C1 | NAV:702-711, 757-758 (locate block after neutralisation) against the R6' \|mean net\| ≤ .02 limit | L1.279 mean net is +.0148 (max .031). A v6 book with more short-interest tilt, higher L, or F2's slower exits of blocked names crosses .02 → mechanics FAIL even at net SR ≥ 1. Fix F4 first. |
| C2 | R6' gross ∈ [.90, 1.05] with one-shot L = 1/mean_gross(L=1), all rows (nav_summ.py:173) | Steady-state gross is 1.026; 102 post-ramp rows are above 1.05. F1 (dispersion drift raises gross) or F2 (nonmembers held longer) with a stale L → mean above 1.05 → FAIL; each recalibration is +1 trial and moves DSR N. |
| C3 | Runtime vs the 180 s bound (v5_train.sh:17): PX:107-146 (~2.1e9 log/run) + NAV:606-608 → 368-388 (~1.2e9 log/run across 5 books) | 28-45 s today, scaling with decisions × instruments × (window + books × 63). Industry neutralisation + 2 more scenario books + a warm-up+TRAIN+VAL panel (~1,650 dates) could reach ~110-180 s → killed. v5_train.sh:153 does not exit non-zero on a failed nav run (T34c m3), so the loss surfaces only at nav_summ. Fix F8/F9 before adding books or fields. |
| C4 | Memory: `--max-bytes 1073741824` (v5_train.sh:152) vs a fixed reserve at max geometry (NAV:1413-1421: 20,000 names × 4,096 dates × books + 262,144 events × books ≈ 0.19 GiB) plus a dates × 5,627-instrument panel (≈52 MB per f64 field; 6 fields ≈ 312 MB; RSS 338 MiB) | Adding grp_ff12 + grp_ff49 + mkt_ret (+156 MB), 8 books and a 1,650-date panel → ~0.9-1.1 GiB → a clean "workspace budget" refusal. Raising --max-bytes toward the 1,536 MiB RSS cap leaves no headroom for JSON publication (16 MB slack, NAV:68) → runner kill. Charge the reserve at actual geometry. |
| C5 | Participation cap convexity at the ADV boundary (NAV:1558; RC:104-106; F2) | L 1 → 1.279 (+28% scale): capped fills 18.9 → 26.1/day (+38%), unfilled +70%, impact 6.95 → 7.64 bps/$. 2020 cost/$ is 15.7 against 12.3 in 2021. Any lever that raises gross or trades the boundary names, or a higher-vol regime, makes cost/$ and SR fall faster than linearly; the "$1bn" claim sits near the cap knee. Related: S3 is negative in 13/13 cells, and F2 raises exposure to exiting, delisting-prone names. |

## 6. What I could not verify

- **Executed forced-exit dollars.** The CSV carries only the planned forced stock (re-counted while capped). The F1
  drift-extra is an accounting residual (executed − planned + unfilled + blocked), not a per-fill measurement; no per-name
  fills are written.
- **Per-name / per-liquidity-bucket cost and alpha.** Needs v6-explore's bucket study; decides lever 9.
- **Industry/style share of book variance and the realised cap-weighted beta.** Needs holdings, which are not in the
  outputs; decides the size of levers 6 and 8.
- **True spreads and auction depth for the universe.** No quote data was read.
- **The effect of mean vs median ADV.** Per-name ADV is not output.
- **Runtime split between exposures and execution.** Inferred from operation counts, not profiled.
- **Build and test state.** No builds were run.
- **Signal clocks and PIT of the combined signal.** Left to the signal reviewer.
