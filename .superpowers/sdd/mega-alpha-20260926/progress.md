# Active task: recent-data DSL ensemble

## RESUMED 2026-09-27 (parent 3; owner goal prompt authorizes; SDD on Opus children)

- Docs commits 44666f09 (owner ruling revisions) + 9003f273 (T10 brief). Shared contracts:
  lane-contract.md, reviewer-contract.md, re-review-contract.md (children read these; no builds).
- Worktrees aligned to root 9003f273: pool-3 (T9) + pool-4 (T7) ff'd; pool-5 new branch
  feat/mega-alpha-nav-t4-20260927 (T2 fix -> T4 -> T10 lane). Ruling: parallel lanes in disjoint
  worktrees now, not after T2 (only root builds are serialized) — cost if wrong: rebase churn.
- Dispatched: t2-nav (T2 fix r1), t7-fields (T7), t9-weights (T9), t5-rereview (T5 fix r1 scoped
  re-review), t1-review (T1 redo), t6-review (T6).
- T6 VALIDATION fields: bounded run mega-fields-validation-v1-run exit 0, peak 452 MiB (tree 473.6MB),
  manifest a0905b0c63bf0574cf0601b0861d3a43c1a8a6a5895516c975e3dbdb29e80f0a, producer 892bd33f.
  Coverage (member, 2021-24): si 99.9%, iv 95.0%, earn 99.98%, shares_out 99.7%, mktcap/size_grp
  72.9%, mkt_ret 99.9%. Descriptive coverage only; no outcome data viewed. HAZARD: iv_atm_126d member
  max 1.19e16, IV max ~69 (garbage/units) -> v3 IV families must guard/rank (T8 brief).
- Declared BEFORE measurement: task-T8-brief.md (library v3 = fixed v2 + ~24-32 SI/DTC/dSI/IV level/
  IV slope/IV change/IV-RV/earnings-drift/size candidates, priors cited) and task-T11-brief.md
  (admission screen v3-admit-v1: orient on 2020-21 mean; tau_k<=0.70; HOLD-2022 oriented mean>0;
  greedy |rho|<=0.70 by FIT Sharpe; >=250 finite FIT days). Ruling: admission screen lives in the T9
  fitter (same f_k/tau_k data) — cost if wrong: one extra tool revision.
  Note: "FIT Sharpe > 0" is tautological under FIT-mean orientation; the binding test is the 2022 sign.
- T2 fix r1: pool-5 ca4fa80c -> root f6df5fe8 (fixture-only: v2 spent Jan budget day 0, name 1 never
  held -> forced turnover 0; fixture now exits held name 2). Build mega-t2-fix1 9.43s Jobs4 1 TU/1 link;
  target tests 25/25 0.59s. T2 review dispatched (t2-review, package review-T2.diff). T4 sent to t2-nav.
  Ruling: GMV turnover stats + ceiling flags use EXECUTED fills (planned forced turnover double-counts
  blocked absent exits) — cost if wrong: none (executed is the declared metric).
- T5 fix r1 scoped re-review: 7 addressed (I1-I4, M1, M2, M7), M3-M6 open non-blocking; new 0C/0I/6
  minor. Task T5: complete (2c92d658..f2d5fb97 -> root 6b4a21fe, library SHA b871743e). T5 minors
  (deferred): M3/M4 optional, M5 root, M6 info; N4 Amihud zero-volume blanking 252 vs 126; N5 docs; N6
  report lacks fix section. Ruling: N1/N2 (record corr notes) + N3 (abs guard in ivol_change denominators)
  fixed pre-measurement as "v2 fix round 2" first commit of T8 lane — cost if wrong: none, v2 unmeasured.
  Re-review also found: runner rejects any library field outside {close,raw_close,volume}
  (strategy_ic_runner.cpp:275-277) -> added to T7.
- T8 dispatched (t8-libv3) pool-7 branch feat/mega-alpha-library-v3-20260927 @6b4a21fe.
- T1 review (redo): spec FAIL 1C/1I/8m. C: cache key lacks engine/VM semantics identity (engine_git_sha
  recorded :557, never compared). I: pinned weights not tied to TRAIN (frozen_train :127-137).
  Ruling: T1 fix r1 routed to t7-fields lane (same file + same cache-key code as T7) — cost if wrong: T1
  fix waits for T7.
- T6 review: spec FAIL 0C/2I/6m. I1 IV no plausibility bound; I2 is_common not PIT, default-on.
  Ruling (declared before IV measurement): iv_atm_* outside [0.02, 5.0] -> NaN, counted; is_common
  opt-in + manifest point_in_time flag. Fresh implementer t6-fix pool-8 feat/mega-alpha-fields-fix1-20260927
  @71cbec8f. Fields must be regenerated (-fields-v2) for TRAIN and validation after.
- NAV TRAIN SMOKE (evaluator test, item 2): mega-nav-train-v6-c1 (run2; run1 failed: --role needs
  manifest.json path), exe @f6df5fe8, v6 blend 51740eff, baseline-v1 c1 f1, legacy flat-300 borrow.
  19.59s, peak 229 MiB, accounting max err 1.3e-13. Results (TRAIN, v6 = old equal-family blend, NOT
  neutralized, NOT MV):
    S1 linear: gross SR -0.27, net -0.38, HAC t -0.68, vol 19.2%, MDD 38.8%, trade cost 1.97%, borrow 4.49%
    S2 1bn (primary): gross -0.09, net -0.25, HAC t -0.47, vol 20.2%, MDD 37.4%, impact $21.3M
    S3 adverse: gross -0.29, net -0.45, HAC t -0.84
    executed tau ~4.0%/day mean, p95 ~6.1% (approx from daily CSV; T4 adds exact GMV column);
    held ~2979 names; participation p95 <= 0.18%; 415 write-offs; guard raw-minus-adj sensitivity $247M.
  DISCREPANCY: target-proxy said gross +0.36 and numpy prototype vol 10.4% for same blend; NAV vol ~2x.
  nav-recon investigator dispatched (writes studies/nav_recon.py; root runs it). Not a validation trial.
- T2 review: spec PASS, quality Approved, 0C/0I/8m. Task T2: complete (c4ba9c80/97e6b392/f6df5fe8, review
  clean). T2 minors (deferred): no K=5 write-off fixture (only K=1); guard branch untriggered by fixtures;
  budget-refusal fixture misses 36 B/cell volume budget; legacy 30%/mo flags unlabeled (T4 req 6);
  daily long/short dollars are post-trade (T4 adds pre-trade GMV). Others in task-T2-review.md.

## OWNER RULING 2026-09-27 (b) — financing: swap-fin-v1 replaces flat 300 bps (handoff 2 §2b)

PB portfolio swap: long pays bench+s_L, short receives bench-s_S-fee; dollar-neutral -> bench cancels;
replay cash-0%/no-rebate = excess-return accounting (structure OK). Flat 300bps wrong both ways: GC ~30bps
holds ~83% of US SI (S3 2022-23; D'Avolio 17bps/91%), specials avg 4.3% w/ fat tail and carry anomaly
short legs (Muravyev-Pearson-Pollet JF 2025); long spread missing; /365 vs ACT/360. Declared primary
swap-fin-v1: long 40, short 20 + engine tier fee (GC 30/warm 100/special 500 bps; flags mcap<$1bn,
px<$5, SI/shares_out>10%, age<365d; missing -> warm), ACT/360, no new/increased special shorts.
Stresses flat-300-v0 (legacy, bit-identical) and engine-tiers-v1 (27.5/300/2750). New task T10 (after
T4, same owner/files). TRAIN-study net .56 used flat 300 - not comparable. No NAV result seen yet.

## OWNER RULING 2026-09-27 — daily cadence; turnover target redefined (handoff 2 §2/§2a revised)

Shipped construction is daily (cadence 1). 30%/calendar-month turnover RETIRED (reporting only; not a
constraint/flag). Combined book: mean daily one-way turnover <= 20% GMV, p95 <= 30% GMV (sum|fills| /
pre-trade long+short $, deployment excluded). Individual alphas <= 70% GMV/day standalone; netting ratio
tau_book/sum w_k tau_k reported on TRAIN. Basis: WorldQuant 4,000-alpha study (median ~30%/day, range
5-149%), BRAIN 1-70% window, practitioner 30%/day ceiling; EMN mutual funds ~1-2%/day = where 30%/mo sat.
Cancelled: validation NAV on v3 blend / monthly-budget-v2 / cadence>1. Daily construction set: band
{0,.5,1,2}/N at fraction 1 (4 trials). T4 brief (+GMV daily stats, ceiling flags) and T9 brief (+tau_k,
sum w tau) revised. Deliverable = frozen daily mega-alpha, one validation NAV run.

## PAUSED 2026-09-26 at owner request — handoff 2

Authoritative: docs/plans/2026-09-26-mega-alpha-parent-handoff-2.md; goal prompt
docs/plans/2026-09-26-mega-alpha-next-parent-goal-2.md. All child agents stopped (T2 fix round, T5 fix
round, T7, T9, T1 review). T2 imported c4ba9c80/97e6b392: build 31.62s, 24/25 (fixture
ConstantPricesNoCostReproducesTargetReplayPlanned forced_turnover==0 open). T6 imported c615ce36; TRAIN
fields run 22.03s/604MB manifest 519fc9b2 (validation fields not run). T5 fix round 1 committed pool-7
f2d5fb97, unreviewed/unimported. T7 (pool-4) and T9 (pool-3) stopped with no commits. No owned process.


## RESUMED 2026-09-26 ~20:15 ET (new parent, owner goal prompt authorizes)

Owner mid-run note: do not let RAM limits slow progress; if blocked, improve
efficiency/incremental progress to cut resource use.

- Import: pool4 8527a839 -> 050c0efc (VM arena release), 23f1541b -> bfb6b859 (fixture).
- Pool3 archive: 58c21bb0 (46/46 files verified, index 352ca7b7) -> 470eb1b6.
- Build helper recent-strategy-targets-v1: source=configured bfb6b859, Jobs2 (free2198),
  36.62s, 6 CPPs/5 links, no PCH/deps. IC tests 35/35 5.141s; target tests 7/7 0.198s.
- TRAIN export v5: guard system-memory-limit at cand 11 (peak894.6MB, others'+agents' RAM).
  Ruling: guard min-free 768->512 MiB for real runs (RSS1536/180s kept) - owner note; cost if
  wrong: host paging, no correctness impact.
- TRAIN export v6 COMPLETE 94.70s wall, peak898.7MB, exe 03607890...; stages vm30.0 ic30.7
  comp15.1 load9.4 save4.4. Orientation array canon 4a3e8004 == v2; planned targets & daily IC
  CSV byte-identical to v2; candidate diffs timing-only. Blend manifest 51740eff...
- TRAIN replay (6bps/300bps): baseline turnover mean35.32%/max80.08%, gross.901; budget-v2
  mean30.66%/max36.69% (forced-exit breaches), gross.847. Only 253/754 mature days complete
  (missing names up to232/day). All-days observed-component gross SR .36 (base)/.33 (budget);
  complete-day subset SR ~1.2 is BIASED - not claimable.
- VALIDATION frozen replay: base mean34.44%/max70.60%, gross.906; budget mean30.24%/max31.31%,
  gross.853; 180/500 complete days; obs-component gross SR .34/.38, ~1.9%/yr, vol ~5%.
  Borrow scenario ~1.25%/yr dominates -> net ~0. Objective NOT met.
- Evidence commit 5c9cbaed: saved-blend-policy archive (60 files) + report + DAG row. Audit script
  d8e5fb27 PASS: 2268 exact baseline f64 values.
- TRAIN numpy prototype (diagnostic only; C++ remains the evaluator): neutralizing desired target vs
  trailing beta252/vol63/logADV63 lifts gross ratio .36->.55, vol 10.4%->7.5%, beta -.16->-.03, but
  turnover 35->40%/mo. Cadence/fraction/EWMA variants all ~.5; borrow 300->50bps ~+.16. BINDING
  CONSTRAINT = ALPHA QUALITY, not construction.
- Sprint v2 dispatched 2026-09-26 ~21:50 (opus, no TDD, root builds):
  T1 pool-4 feat/mega-alpha-runner-cache-20260926: candidate signal cache + pinned composition weights.
  T2 pool-5 feat/mega-alpha-nav-20260926: NAV replay per nav-backtest-design.md (S1 linear, S2 $1bn
    sqrt-impact PRIMARY, S3 adverse K=1; stale-carry K=5).
  T3 pool-3 feat/mega-alpha-exposures-20260926: price exposures + neutralize_target (new files).
  Data inventory (read-only Explore) running for fundamentals/short interest/13F/industry.
  Ruling: implementer tasks branch fresh from root 5c9cbaed in pools 3/4/5 (old branches kept) -
  trivial cherry-picks; cost if wrong: none.
  Ruling: 300bps borrow and K=5 kept as registered/designed; no cost grid.
- Inventory (read-only): FINRA SI as-of (C:/atx/data/finra_short_interest/asof, securityID-native,
  ~100% members, strict available_at<session) = fastest new source; TickerHistory3 ATM IV/HV +
  earnFlag native; spine_monthly me_line/security_type native via TBLTICKERHISTORY-<id>; CIK-mapped
  fundamentals/SIC only ~65% + export seal 2020 -> next tier. GICS empty. 13F/analyst absent.
  Ruling: role extra-fields path (Python producer + runner load of referenced fields only) rather than
  new price projection; cost if wrong: one more producer iteration.
  T5 pool-7 feat/mega-alpha-library-v2-20260926: library v2 = v1 48 + 48 price/volume (8 families).
  T6 pool-8 feat/mega-alpha-fields-20260926: prepare_research_fields.py (SI, IV/HV, earn, size).
  Planned: T4 wire neutralization into target/NAV; T7 runner --extra-fields; T8 library v3 (SI/IV).
- T3 e4a869b7 -> b9cf4023; CMake 429cbe43. Build mega-t3-a 21.89s Jobs4, 3 TUs/2 links, provenance
  429cbe43. atx-impl-strategy-target-tests 15/15 pass 0.145s (7 old + 8 new). Review dispatched.
- T5 2c92d658 -> c47ffdaa: library v2 96 (v1 48 byte-identical + 8 new families). SHA 0c7f3059...;
  recipe 93cd52ab... --check ok; native --plan-only: 96 compile, max slots 8, lookback 314, TRAIN
  admitted 1,234,268,377 B. Concern: vec_avg member-masked -> mkt-based templates NaN unless member
  whole window (review checking). Review dispatched. TRAIN run waits for T1 cache (180s cap).
- T3 review: APPROVE 0C/0I/8 minor. Task T3: complete (b9cf4023..429cbe43, review clean).
  T3 minors (deferred): M1 rescale amplification up to 1e9x -> T4 must cap; M2 msg; M3 clip untested;
  M4 refusal kind untested; M5 ADV present-missing-volume=0; M6 pivot floor; M7 compute exposures once
  per decision in T4; M8 noexcept.
- T1 09a18ec3/a7fd1c02 -> af8c38ee/445e828d. Build mega-t1-b 32.29s Jobs4 5 TUs/3 links; IC 42/42 9.68s.
  First real cold-cache run TIME-LIMIT 180.27s: Debug SHA-256 ~20MB/s (cache write 2.5s/candidate).
  Fix 6d85ac2a: scoped /O2 on atx-core sha256.cpp (build sha-o2 21.89s, 3 TUs/6 links, prov 6d85ac2a).
  Rerun mega-v1-train-cache-b: COMPLETE 71.16s peak 790MB, 37 cache-hit + 11 cold, planned targets /
  daily IC / blend bytes IDENTICAL to v6. load 9.4->1.43s, save 4.35->0.33s, write .24s, hit .35s.
- T5 review: spec PASS, 4 Important (I1 raw-volume splits; I2 vec_avg member-mask blanks mkt templates;
  I3 resmom ~ v1 mom; I4 illiquidity/ivol/max reload v1 tilts). Ruling: fix pre-measurement (fix round
  1 sent); I2 via producer field mkt_ret (T6 addendum) + runner --*-fields (T7) instead of VM opcode
  (avoids engine rebuild fan-out); cost if wrong: v2 waits on T6/T7.
- T2 addendum: accept pinned-candidate-weights blend semantics. T7 dispatched pool-4
  feat/mega-alpha-runner-fields-20260926 @6d85ac2a.
- TRAIN-only studies (scripts in studies/; diagnostic, not evaluator; validation untouched):
  compose_study: 48 cached v1 signals, per-candidate neutralized (beta252/vol63/logADV63) daily factor
  returns; signs+weights fit 2020-21, holdout 2022. Gross SR hold22: equal .72, inv-vol .84,
  pos-sharpe .82, MV shrink.5 .62, MV shrink.9 1.23 (7 methods compared = 7 composition trials on the
  2022 TRAIN holdout). Mean pairwise factor corr .18. Seasonality candidates unstable.
  construct_study (MV.9 blend, neutralized target, 6bps+300bps): hold22 gross/net SR, turnover/mo:
  daily full 1.19/.56/169%; c5 full 1.07/.49/131%; c5 f.25 .60/.13/56%; c1 f.10 .70/.19/77%;
  c5 f.25 band(1/N) .69/.31/28%; c21 full .58/.10/78%. => alpha decays fast vs 30%/mo budget;
  no-trade band >> partial adjustment; flat 300bps borrow costs ~.3 SR.
  Ruling: next construction = neutralize + no-trade band (T4), composition = MV-shrink(.9) weights fit
  on full TRAIN (pinned via T1), alpha priority = slower sources (SI, IV, then fundamentals); add an
  SI-tiered borrow scenario (engine borrow_tiers GC 27.5bps/warm 300/special) next to flat 300bps.
  Cost if wrong: some construction rework; no validation data spent.
- Next: NAV backtest with declared missing-price policy (design agent), audit script review
  (agent), then construction (risk neutralization) + alpha expansion toward SR>=1.


## PAUSED at owner request — next-parent handoff

Owner requested stopping and a detailed handoff/goal prompt. Goal status is
paused; all three agents acknowledged stop; no owned build/research job remains.
Authoritative handoff: docs/plans/2026-09-26-mega-alpha-parent-handoff.md.
Paste-ready prompt: docs/plans/2026-09-26-mega-alpha-next-parent-goal.md.
Last implementation HEAD6df7cc88, root clean before documentation. Memory fix
8527a839 and fixture23f1541b are committed in pool4, source-approved by pool5,
but unimported/unbuilt. Target replay is integrated but unbuilt. Pool3 has the
ignored unfinished compact v3/v4 evidence archive; exact paths/hash in handoff.
No new build or numerical run after the v4 memory stop. Resume only on owner
instruction; older future-tense checkpoints below are superseded by this stop.

## Current: frozen validation complete; saved-blend portfolio iteration

Root imported target replay through8c7dc6a0. Root CMake registration and fixed
baseline-versus-30%-monthly-budget recipe are being committed. Pool4 is fixing
runner VM slot growth: candidate11 grows5->7 slots while old5 stay allocated,
adding248MiB transient TRAIN RAM. Release old VM before growing, retain masks,
shared workers and exact arithmetic. Pool5 reviews; root alone builds/data.

At a9b8814c: focused build82.169s Jobs2; all34 native checks10.484s.
Validation-only v3 completes48+blend110.5s, peak660586496B; no TRAIN refit.
Combined rank IC5/21/63=.0180335655/.0356902802/.0596669293;
planned mean monthly turnover.3443560. No Sharpe or capacity qualification.
Saved validation manifest build-equity/recent-fast-ic-validation-v3/validation_combined.json
SHA7407d7e7b72548e5577fdf94e2f52d238f9ff751a8ebc5640987130c1390d8e1.
All55098 comparable old daily IC values exact; one old unterminated row excluded.
Pool3 archives compact v3 build/native/actual/audit receipts plus v4 RAM stop.

v4 TRAIN export stopped at41.578s system-memory-limit, peak1101864960B,
10complete/start11; no TRAIN artifact. All processes ended. Preserve evidence.
Do not blindly retry. Once lifetime fix is qualified, one bounded TRAIN export
must reproduce original v2 signs/metrics and save the blend. Then exactly two
saved-blend TRAIN target runs (baseline and fixed.30 monthly budget) and frozen
validation construction. Cost scenario6bps one-way/300bps borrow is hypothetical.
Do not expand corporate-action catalog; no2025+ evaluation or validation tuning.

## Latest: completed TRAIN, resume validation next

Root HEAD before this checkpoint `0230d799`. No active build/process. New
pending CMake edit adds existing factory/ic_screen_test.cpp to the focused
atx-impl-strategy-ic-tests target to qualify legacy arithmetic/bookkeeping.
Next ignored build helper `build-equity/build-recent-strategy-fast-ic-resume.ps1`.

At0f618a45, optimized build31.631s Jobs3 and all13 native checks1.618s pass.
Only3privateCPP commands gained O2/Ob2/finline and local PCHexceptions;
allotherflags/CRT/FP/ISA/PCH/deps unchanged. v2 fullTRAIN48+combined completes
131.666s; wholeprocesscap180.218s leaves13VALcomplete/14started, noVALblend.
Peak896258048bytes. Samefirst10 candidates84.772s->24.090s, all62376 valid
dailyIC values identical;1truncatedbaselineCSVrow excluded. Exactv2packet
fast-ic-optimized-20260926 and adjacent qualification report preserve all.

Resume artifact `build-equity/recent-fast-ic-v2/orientations.json` SHA
5106fdc13fc5347c9c2c670714c134a1978e6b7a0d2d2b5dd79bacb7dfb782d6;
adjacentrecipe needed. Same fullrole pins/library/minnames2000/memory1536,
workers4; add --orientations PATH --orientations-sha256 SHA, newoutputdir.
No TRAIN payload/refit. 2025+ reserved.

New source imported: parallelICbfabe3a4 +fixture5330a5fd, resume34d149a2
+fixturee72f32da, caller0230d799. G0sourceapproved42337ab3 pendingimport.
Pool4 separately implements opt-in --save-combined exactf64 blend+effective
membermask and boundmanifest; include before resumedVAL if ready, to avoid
re-evaluating48DSLs for later portfolio construction. Pool5 now DESIGNONLY
for fast savedblend portfolio/turnover work; no source/data/buildthere.

TRAINcombinedIC5/21/63=.03499/.04993/.05983, training-onlyfit, noSharpe.
Plannedmeanmonthlyturnover35.32%, meangross.9012,maxabsnet.0645; targetnotmet.
Do not resume corporate-action catalog expansion. Focus runtime, composition,
then fast portfolio construction. Alloldercheckpoints supersededasneeded.

## Latest runtime checkpoint

At clean source `20bf677b`, focused build passed in27.729s Jobs3 (eight CXX
actions, warm PCH/deps); all11 native checks pass1.344s. Metadata plan confirms
maxslots7, lookback314, TRAIN1,487,603,721bytes, validation1,030,839,264bytes.
First real fast attempt `build-equity/recent-fast-ic-v1` stopped deliberately
at105.609s:10 TRAIN candidates complete,11th started;6.091–11.509s each,
peak956231680bytes. No composition/orientations/validation complete. Fixed
library/weights unchanged. Guard is process-error plus explicit operator-stop
receipt. NO active process. Root imported final review/report ef65e6d8/2147d883.

Pool4 now implements explicit1..4 sharedDetPool CS/TS execution and per-stage
timers in private runner, preserving serial default/recipe/math; fixtures then
review. Pool5 investigates narrowly scoped optimized compilation of hot CPPs
without globalDebug/PCH/dependency rebuild. Root owns CMake/build/data. Next
build/run remains bounded; don't re-run known-slow serial baseline wholesale.
Exact32-file evidence and initial qualification report under
`.superpowers/sdd/strategy/fast-ic-qualification-20260926` and adjacent report.

## Fast IC integration checkpoint

Current root before this checkpoint: `9bb58f9e`. Event expansion is stopped.
New frozen 48-alpha library/helper source `c4c8dc59` -> `0139cb15`, fixtures
`67b28ba0` -> `2a26e674`; kernel `1dba7035` -> `3048951c`, fixtures
`ade1e99d` -> `9b65981a`; runner `08940e88` -> `1e67b5c5`, fixtures
`767d8894` -> `d0c9ade6`, identifier fix `86671916` -> `9bb58f9e`.
Independent kernel/composition review `d91514f4` -> `4c4f579c`; runner source
has no substantive review blocker, final fixture review pending with G0.

Full roles are ready from cached data at source `527add1c`:
TRAIN `recent-fast-train-2020-2022-v1`: 1,155 dates × 5,627 IDs, score 399..1155,
manifest `3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493`;
validation `recent-fast-validation-2023-2024-v1`: 903 × 5,048, score 401..903,
manifest `0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7`.
Preparation 12.266s/8.016s. No IC or portfolio result has been evaluated.

Next: commit focused CMake registration, run ignored helper
`build-equity/build-recent-strategy-fast-ic.ps1` (RAM-admitted 2–4 workers;
only atx-equity-strategy-ic and atx-impl-strategy-ic-tests). Run all 11 new
native cases, then metadata-only CLI to obtain actual slots/memory before
loading data. Real runs <=180s, sampled RSS/free floor, min-names 2000.
The fixed baseline includes all TRAIN-oriented candidates irrespective of
diagnostic screen rejection. Signs fit on TRAIN 21-session mean rank IC;
undefined/zero signs are neutral. 2025+ reserved. No book/event surfaces.
Pool5 is inspecting existing DetPool APIs for a narrow runtime follow-up only
if timings justify it; no edits or numerical runs there.

Stock batch 45/45 evidence is archived in
`.superpowers/sdd/strategy/stock-qualification-20260926`; no real stock run.
Older checkpoints below are superseded where inconsistent.

## LATEST OWNER STEERING: runtime, generation and composition before realism

Owner explicitly stopped overoptimizing realism/registering every action.
All event-expansion work is stopped. In-flightstockbatchclosedall45casespass
3.028s(guard3.25s/18MiB) atb036fa32. First195.553sJobs2 compilefailure was
fixturemixedconstpointerdeduction; test-onlyb036fa32 resumes12.462sJobs4,
oneCPP+twolinks. Configuredf1c40409. NOactualv4bookrun, threeactualattempts
stillzeroresults. BothJAG/WCGstockconfigsretainedcurrentSHA
2fc4cc34103cb7cf591b7a27e4b8da58f749d76193e36fe474014c66f3d76691.

Activeplan nowprioritizesnewfastresearchphase. Pool4implements NEWlightweight
factory/ic_research.hpp opaqueoptions/cache/scratch over sharedic_screen.cpp
helpers (legacyIcScreenConfig/header/APIsunchanged); research3horizons5/21/63,
strictendpointpresence. Newprivate strategy_ic_runner.hpp/CPP +standalonemain,
no surfaces/book. Pool5owns48DSL library8families*3templates*2slowvariants,
maxlookback<=320, plusnewprivate streamingcompositionhelper. Fixed1/48weights,
TRAIN21ICsigns/undefinedzero, fixeddenominators; combinedIC/coverage andplanned
targetturnover/exposurediagnostics only. No evidenceweightoptimizerfirst.
G0 revieweddesign andisIDLEawaitingfrozenpackets; reactivatewithfollowup_task.

Rootnext: preserveconcisequalification, preparefull2020-22TRAIN fromwarmup
2018-06 and2023-24validation fromwarmup2021-06 usingexistingacceptedcache
recent-projection-v1 (no rawrescan); caprealprocesses180s/RAM/freefloor. Freeze
exactnewlibrary beforeperformance, inspectkernel/runner/composition, register
focusedCMake, one warmbuild thenboundedactualfastresearch. Preliminarysignal
diagnostics arenotcostednetportfolioSR/capacity/actualturnoverclaims. Usergoal
still$1bn,Sharpe>=1,lowturnover/thousands;2025+reserved. No activebuild/process.

Currentrootbeforethischeckpointb036fa32. NoC:/atxwrites/push/warehousewrites.
OriginalDAGupdatedsource/importSHAs. Oldercheckpointsbelowaresuperseded.

## Current checkpoint: cash qualified; stock conversion in implementation

Root3780a55d records cash/monthly qualification and49 exact evidence artifacts.
Clean compiled sourceab049a50, configuredc09df61c. Firstbuild51.129s Jobs3
failed mask span/owned-vector API; one-linefixab049a50 resumes21.906s Jobs3.
All32 native cases pass1.738s (12new), zero failures/skips; guard1.875s.
Report .superpowers/sdd/strategy/2026-09-26-cash-claim-qualification.md.

Third actualrun recent-dev-rehearsal-v3 stops4.828s/219181056bytes on first
candidate+ orientation JAG4997008 short-151822.37061726692 at2020-01-10
22UTC; priorJan9price8.2299995422363281, no guard crossing, sourceabsent.
It passes priorMDCO/WAIRendpoints; laterthreeconfiguredcashevents unreached.
Three cumulative actual attempts, zero completions; no portfolio Sharpe.
Samecashconfig257c6d645292b9f9464e6401ed1f9adb5e41a66089fdc4f7e23db0724f6da510,
same24library/costs/roles/selection. No Q2score/2023-24/2025+ evaluation.

JAGfacts775a9cad->16a8adb2: issuerJan10 08:51ET confirmscompletion;
447/1000 ParsleyEnergy CLASS A PE, CUSIP701877102 per predecessor share.
Use followingminute13:52UTC conservativeavailability. Nofixedcashleg;
actualfractionalCIL/delivery/loantransfer unresolved. Priorstockinventory
a852df6c->ffda3043 identifies onlysyntheticexistingreplaycaller.

Pool4 implements new narrow factory stocktransitioncontext/privateCPP with
signed successor addition/bridge and optionalexplicitfixedcash, retirement/
queuedorder netting, exact observed successor raw/adjustedbasis, causalclock.
Preservecashroutehash/arithmetic whenstockempty. No oldsyntheticenumwidening.
Pool5 owns strict pinned --stock-transitions runner/config v3 with sameallroles/
signs and decisionmask, no fabricatedfractionalsettlement. Pool3 extends
audit_recent_price_gap.py --tickers exactsource selector to bind PEvendorID;
rootonlyrunsrawaudit, thenG0reviewsnewengine. No activebuild orrealrun.

Need stock primary/sourcefacts artifact, preregister pinnedv3 execution before
fourthattempt; rootownsCMake,focusedbuild,guardednative/realrun. UpdateDAG
withstockimports whenready. Emptywarehousetables and commonstockcoverage
remainunresolved; optionalexternaldatapathquestion stillpending. Original
goalactive/unlimited but ownerpivotgoverns; no goalcompletion or push.

## Prior integration checkpoint: five cash claims awaiting runtime (superseded)

This checkpoint supersedes the historical sections below. Root source before
this checkpoint is d52ed04b, pool2 only. No active build or numerical process.
Owner target is $1bn NAV, net annualized Sharpe>=1, calendar-month one-way
turnover<=30%, thousands of stocks, 2020+ data. No achieved result: two real
attempts failed, zero completed trials. Recent strict diagnostic evidence is
eb2559e8; source gap is genuine MDCO archive ending January3,2020.

Cash engine core c0be402e->eb67e4d5, clock1ef2524d->8ae8294c,
capital440d538f->963adbc0, fixtures25ff0a88->a2fe3280,
3d1ef20a->9d8a718f and47696031->f8d1062f. Independent source review
eeb6dc03->88f1e2c3 approves seven fixtures/source; native runtime pending.
Runner80ebf709->1ab4a490, decision-mask5e1fa849->be2ce0ad,
three fixturesa86b274d->010cc1a8, report1b62048e->d52ed04b.
Monthlyreport779add70->494ecc75 and fixture91329304->6a05ec3f pending.
Root CMake registration65e1e50a/2d06b704 already included.

Five event records MDCO/WAIR/BOLD/ARQL/THOR are committed atba58bab2;
cash config SHA256257c6d645292b9f9464e6401ed1f9adb5e41a66089fdc4f7e23db0724f6da510.
They are reconstructed publication evidence, not verified delivery/settlement.
No invented payment: signed nonspendable claims, reserve short payables,
continue last modeled short borrow. Active plan preregisters v2 retry with all
previous candidates, trial counts, costs and role/data pins unchanged.

Source forensic batch2a41477a ran38IDs in10.5s/121MiB; output
build-equity/recent-q1-gap-batch-v1.json SHA256
037234f9c3f427dd45160a8f19ab7ecbc40dd941699cc73f56c94f717f11ed56.
Masks-only queue is data QA, not terminal proof or future universe filtering.
Read-only warehouse event, terminal and universe-type tables are empty.
Optional user data-location question remains unanswered. Stock conversions,
temporary gaps, dated common-stock/type coverage remain unresolved.

Next: finish G0 adapter source review; freeze/checkpoint; build ONLY
atx-equity-strategy and atx-impl-strategy-tests via warm equity-dev wrapper,
RAM-admitted2-4 workers. CMake regeneration updates configured provenance.
Run bounded focused native, then one fresh claim-aware v3 rehearsal with
180s/1536MiB RSS/768MiB free, internal1024MiB/min_names2000. Preserve failures.
Root alone builds/runs; pool3 reviewsadapter, pool4 inventories existing stock
conversion support, pool5 source frozen available for fixes. No C:/atx writes,
warehouse writes, push, broad build, long performance workload or goal success.

## Latest checkpoint: native qualified; first real refusal preserved

At clean source497f567e all19 focused native cases pass (0.918s native,
1.062s wrapper). Initial18/19 DSL path failure corrected by test-onlyd8cc7b65.
Initial build88.873s Jobs3; fixture-only13.521s, warm PCH/dependencies retained.
Detailed source/import/evidence index:
.superpowers/sdd/strategy/2026-09-26-native-and-first-rehearsal.md.

First real run build-equity/recent-dev-rehearsal-v1 failed in6.234s/208MiB on
momentum_12_1_s21 positive orientation: missing/guarded held return. One real
trial attempted, zero completed, no Sharpe result. Q2/2023-24/2025+ not read.
Pool4 diagnostic sourcee2b97317/fixturee53d13a3 ready for review; preserve
strict behavior and identify the exact data issue. No future filtering.

Reuse accepted cache build-equity/recent-projection-v1. TRAIN role
recent-dev-smoke-v1 manifest900839a1ea8e21edc0f5edd5e9cd8f2bc7884a9295a86d5d6f2de19a79aed36b
is461x3950 with399warmup/62scored and2872..3000 eligible names/day.
Development check recent-dev-check-v1 manifest2129ed162ca3eb1e2bce3bd74f456b83ea11187c26729d06712677bbb6848e84
is524x4101 with461warmup/63scored. Cached prep6.391s/7.359s.
No source rescan needed. Frozen24 expressions,6families,1/24 contributions,
primary cadence5/fraction.25, diagnostic fraction1; TRAIN-only signs.
Budget52 completed evaluations per stage,104 across rehearsal/full experiment;
all failed/retried attempts additionally recorded. First admission min_names2000,
workspace1024MiB, external180s/RSS1536MiB/free768MiB; fixed$1bn cost scenario.
2025+ reserved. Current stock-type/source-vintage qualification remains open.

Pool3 masks audit7a69b22a->3f9e1749; dated stock-type inventory3614788d ready.
Pool5 source review5f03b4df->e86a7bd0 and fixture fix complete. Pool4 source
report30f25f26->53163fbd and strict diagnostics ready. Root alone builds/runs.
No active compilation or numerical process at this checkpoint.

The sections below are historical checkpoints, superseded where indicated.

Owner pivot and confirmed$1bn NAV supersede fullsprint/pre2020scope. Activeplan docs/plans/2026-09-26-mega-alpha-strategy.md. RootHEADbeforethischeckpointcf252c5b. Rootonlybuilds, pool2ownedtree, noC:/atxmutation/push/warehousewrites. No compiler/runtimeactive.

Sourceintegrated: executioncadence54b4ec99->34fcac6b, libraryeeb76477->cf252c5b, boundedrunner257ffd3b. Nativequalificationpending. Runtimeguardshortsuccess andintentionaltimeoutpass; JSONreceiptcopiedhere. Initial24candidates sixfamilies, primarypartial25every5sessions, fixed50TRAINtrialbudget, $1bn NAV, lowturnoverworkingtarget30%monthlysumabsfills/NAV. No recentperformancevalues read.

Pool3implements new prepare_recent_research.py andstrategy_data.hpp/privateCPP: fivecolumnstreamingParquet?privateDuckDBcache/sort?roledata withlagged63rawUSDADVtop3000,separatepresence/membermasks, boundedmemory/disk. Source3.6GBParquetfooter2012-03-26..2026-09-18,32mrows71fields. No stocktype/listingproof, onlyresearchliquiditycohort untilresolved. No sourcepayloadrun authorizedtoagent; rootreviewsandorchestrates. Warmup2018-06-01/2021-06-01; require63+320validwarmupsessions.

Pool4implements strategy_runner.hpp/.cpp andstandalone tools/equity_strategy.cpp: actualDelayedSurfaceV2/B1markedbook, train-onlysigns, fixedfamilyweights, modelcosts/borrow+$1bncapacity, explicitRAMadmission. Handlescadencefixtures. RootownsCMake/source-localJSON. CandidateVMmustuseD12asofcrosssectionmaskinsideallhistoricalranks.

Pool5libraryfrozen; addsnewtests/strategy_dsl_test.cppactual24VMcausality/readycheck andcompanioncombinationrecipe, thenreviewsboundedrunner257ffd3b. No morecoarsescreenresearch.

OldD4 andresidualcoarsescreensourcepacketsremainunimported/uncompiled; donotaccidentallycherry-pickagentdependencyalignmentmerges. D4latest8eed1323/d5c4f393/c9ba9a60/fixturescd886b07/d37cc7fd/reportc92ddc5b; screen0fa86097/adb38ab0/79a482a2/reviewe2e15b58/addendum8e2e20fa. Notcriticalforfirstensemble.


## Integrated recent data and focused build registration
Root8bb91e82 includes adapter5f3b6631->c4a9f015, fixturese5f9630f->afcef6b6, resourcefix70dd7213->a304139f; guardreview05d2ba51->3c07ceee. Producer4Pythonchecks pass at8bb91e82, native1.493s/wrapper2.734s/sampledtreeRSS117850112bytes, receipt build-equity/recent-data-python-qualification. No actualpricepayloadread. Agentreport5fb2c6f0 copiedexactly as report/metadata only (its unrelatedcoarsescreenreviewaddendum notimported).

Combinationrecipe67a2b70e/056ea973->1ca79330/b6d8c52f, finalSHA425df0171096357b33efdf1b80d18150317e095f1d0f5cfb271ea710cc643a13. Native24DSLfixture1b3b3936/a7b52a78->46a3e5eb/0709f70f. CMake8bb91e82 registers strategy_data.cpp withsource-localJSON andone focused atx-impl-strategy-tests target usingexistingimplPCH withDSL/data/executionfixtures. NO build started. Pool4runner/cadencefixturesstillpending.

Guardfix11afd431 retains observedchildidentities afterparentexit andperprocessRSSerrors/prelaunchfloor. Finaldescendanttimeguard stoppedall3observedownedprocesses in2.203s. Initialassertionexpectedexactly2processes; correctedassert>=2againstsamereceiptpassed. Scopeissampledoperationalguard, notOSsandbox; detachedunsampledchildrenunsupported. No activeprocessleft.

Pool5independentlyreviewsadapter beforeactualprojection; rootreviewofsource/fixes isclear. Pool3nowindependentlyreviewscadence54b4ec99 andavailableforproducerfixes. Pool4newstrategy_runner.hpp/CPP +tools/equity_strategy.cpp inprogress, usesD12maskedEngineandsharedexecutiondirectlywithoutmineheader. Rootnext: finishreview, firstboundedrealprojection(2018-06..2025exclusive), codebatchreview/compile andnativefixtures. Stillno achievedSharpe/turnover/commonstockqualification.
