# P9 code review — NAV / target replay / cost / leverage stage (sub-reviewer, pool-2 @ d7c1c520, read-only)

Supplement to `2026-10-02-p9-code-review.md` (the stage reviewers had not reported when the main review was written).
Paths repo-relative. nav = atx-impl/src/strategy_nav_replay.cpp; v7 = atx-impl/src/strategy_nav_v7.cpp;
tr = atx-impl/src/strategy_target_replay.cpp.

## 1. Pipeline map

| stage | code | tests (atx-impl-strategy-target-tests) |
|---|---|---|
| load: combined signal (5 payloads) + role prices/volume, SHA-pinned | tr:875-934, 935-982; admission/budget nav:2090-2134 | strategy_target_replay_test.cpp |
| desired target: tied rank -> (hold band, inv-vol, norm-score, two-speed) -> locate zeroing -> price-risk neutralization -> adv-hold cap | tr:524-580, 272, 482-507; once per decision for all books nav:792-813, 875-891 | target + nav tests |
| borrow tiers at d | nav:816-865 | StrategyNavReplay.BorrowTier* |
| target rule aim-partial-v5: next = c + theta(L*desired - c), dust band | nav:924-936 -> v7:434-477 (vol/risk scaler sets L_t) -> v7:479-573 -> tr:345-399; theta = --trade-fraction (tr:380) or per-name-v1 (nav:957-964, 2166-2176) | VolTarget.*, RiskTarget.* |
| orders (target or delta basis, decision-NAV dollars) | nav:983-1023, 969-973 | NavV6.* |
| execute at close d+1, 1% ADV cap | nav:721-780; ADV/sigma window [t-63,t) nav:480-501 | nav_replay_test |
| cost | nav:438-457: FlatBpsV1 or SqrtImpactCost = (5 + 1) bps + 0.6*sigma*(F/ADV)^0.5 (atx-engine/src/book/replay_cost.cpp:84-111; params nav:2150-2164); v2 KO/FIM laws reserved (strategy_cost_v2.hpp:89-99) | CostV2* |
| financing at MARK: flat-300 short, or swap long 40 / short 20 + tier fee 30/100/500 bps ACT/360 | nav:649-678, 2137-2148 | FlatLegacyFinancing* |
| MARK / NAV identity | nav:683-716, 1029-1049 | |
| summary -> publish recipe.json, daily_<S>.csv, events_<S>.csv, summary.json | nav:2442-2546, 2698-2747 | |
| v7 extras: v7_extras.json, capacity_curve.csv, vol_target.csv | v7:347-431 | |
| capacity curve: second dispatch v7:1121-1133; 5 books in ONE lockstep pass, m in {.5,1,2,4,8} (v7:622; strategy_cost_v2.hpp:109) | | CostV2Capacity |

nav (3,653 lines) layout: 1-106 constants; 107-290 state structs; 291-437 validation; 438-553 cost/liquidity; 555-716 MARK;
717-780 EXECUTE; 781-1023 DECIDE; 1024-1100 close/holdings; 1102-1343 lockstep loop; 1345-1429 summary accumulators;
1430-1580 prose declarations; 1581-1934 writers; 1935-2134 pinned loaders; 2137-2208 scenarios/workspace; 2210-2420
grid + APIs; 2442-2546 summarize_nav; 2557-2943 timers/publish/holdings; 2962-3308 run_nav_replay/grid; 3310-3497 CLI;
3499-3652 decide-path seams. God-file: simulation core ~1,050 lines (291-1343); ~2,300 lines IO/receipts/CLI. Behaviour
depends on a thread-local hook in another file (v7:103).

## 2. Flags and configuration

- 40 core flags (nav:3317-3458) + 27 v7 flags (v7:121-135, 842-911: --cost-v2, --capacity-curve, 4 v6, 3 risk-target,
  --vol-target, 17 spo) = 67 flags in two parsers with separate duplicate checks.
- JSON grid may set only 8 construction keys (strategy_nav_replay.hpp:440-442).
- The "spec" is an argv template: research_cycle.py:1167,1172-1175 substitutes `nav.leverage` and passes `nav.rule` /
  `nav.flags` verbatim; no Python allowlist for NAV flags (contrast MARGINAL_SPEC_FLAGS research_cycle.py:206-211).

| knob | today | change type |
|---|---|---|
| leverage L | `--aim-leverage`, refused outside [1,2] (tr:129-134) | flag; > 2 = code change |
| vol target | `--vol-target vol-target-v1`; floor 1, cadence 21, 252 compiled (vol_target.hpp:42-43) | new law/param = code |
| cost model | not a flag (nav:3385); S1/S2/S3 hard-coded (nav:2150-2164); `--cost-v2` adds 2 laws | code |
| horizon / theta / cadence / exit | `--trade-fraction`, `--cadence`, `--exit-rate`, `--two-speed` | flags (grid keys) |
| initial NAV $1bn, liquidity window 63, min pairs 20 | compiled (strategy_nav_replay.hpp:143-144) | code |
| capacity multiples | compiled (strategy_cost_v2.hpp:109) | code |

Soft spots: `--holdings-format` accepted without `--emit-holdings` (nav:3412-3417); `--book-workers`, `--liquidity-cache`,
`--stage-timers` not in the recipe (by design, bit-identical output); X-10 at L = 2.0 cap means a PM6-6 correction to
L' > 2 is refused (tr:131).

## 3. Cost of adding a NAV rule or cost model

- Y-1 (47d6afd9): 12 files, +861/-35. R-8 (f5a8eefe): 9 files, +993. Cost v2 (00aeeb63): 12 files, +3,390.
- A new cost law touches strategy_cost_v2.hpp/.cpp, the hook nav:439, scenario list v7:626-629, parsing v7:604-607 /
  842-845, recipe label, tests. Book cap: main pass + --cost-v2 = 7 books of 8 per lockstep (nav:52, 2275-2276).
- A drawdown control needs the NAV path; plan hook passes only nav_post (strategy_nav_v7.hpp:199-206, 13 params); the
  scaler sees only current weights (strategy_risk_target.cpp:77-79) -> 10+ files, and loses grid/book workers (§5).

## 4. Correctness

- Timing: decision at d uses signal row d (tr:531-544) and close d; fill close d+1; first return row d+2 (nav:1294).
  Signal-row causality is the IC runner's contract, not verified here. Look-ahead test:
  StrategyNavReplay.FutureSignalAndPricesDoNotChangePast (nav_replay_test:635).
- Liquidity windows: fill side [t-63,t) (nav:480-501); decision side ends at d (nav:794-810); per-name rate [d-w,d) --
  causal but inconsistent.
- Vol target: sigma_hat from post-fill weights at d and risk row d (v7:461-462; strategy_risk_target.cpp:125-126);
  sigma_ref expanding mean incl. current (vol_target.hpp:52,118). GAP: no truncation / look-ahead test on the scaler path.
- Capacity curve: NAV-scale identity exact (strategy_cost_v2.hpp:102-108, tested). EXCEPTION: adv-hold cap reads the
  initial NAV at every multiple (nav:800; help 3350-3352; pinned by strategy_live_test.cpp:2234) -> capacity overstated
  whenever `--adv-hold-q` is on.
- Hash pins: summary.json binds recipe, combined, role, weights, semantics, combined manifest (tr:890-894), field SHAs
  (nav:2012-2014, 2055-2059), daily/events SHA per book (nav:2669-2671); risk-model SHA in v7 block (v7:253-254).
  MISSING: executable identity (git SHA, build type -- the risk verb records it, strategy_risk_verb.cpp:888); argv;
  v7_extras.json not bound by summary.json; capacity/summary.json not bound by the extras files map (v7:349-430).
- Completeness hole: summary.json published (v7:1107) BEFORE the capacity pass (1121-1133) and extras (1134);
  research_cycle marks done on summary.json (research_cycle.py:1176); capacity reader returns None if missing
  (wave_readers.py:79-81) -> a crashed capacity pass reads as "done" with null capacity.
- Debug vs Release 1-ULP: + - * / sqrt correctly rounded; no /arch:AVX2 (CMakePresets.json:48-55) so no FMA; sequential
  name-order accumulation in both builds. Only non-exact call: `std::pow(participation, delta)` replay_cost.cpp:92 (+ cbrt/
  pow strategy_cost_v2.cpp:45,138,196,231); ucrtbased vs ucrtbase may differ in the last bit. Fix: sqrt when delta == 0.5
  (the only registered value); probe test comparing cost_fraction bits across builds; one re-pin ruling.
- Memory: whole panel in RAM (36 B/cell tr:903 + fields 16-24 B/cell nav:2041 + label marks nav:2109) admitted against
  `--max-bytes` (default 512 MiB) before loading (nav:2103-2107); geometry cap 4096 x 20000 (nav:51). Risk rows re-read
  from disk per estimate by every book (strategy_risk_target.cpp:126; strategy_spo.cpp:734-760).
- Accounting: return identity 1e-9 every MARK (nav:90, 710-714); cash + holdings = NAV every session (nav:1044-1047).
- Determinism: lane-owned writes (nav:1299-1303); v7 hook thread-local (v7:103) -> pool refused under it (nav:2299-2304).

## 5. Performance

- Capacity curve = ONE extra lockstep pass of 5 books, not 5 replays. A gross-matched cell = 4 full passes in 2 processes.
- Re-done every pass: argv re-dispatch (v7:1098-1103); role manifest parsed twice (nav:2070, 2027); SHA verification of
  5 combined + 6 role payloads (tr:912-917, 956-975) + field payloads (nav:2047-2054); session ring (nav:1275-1278);
  desired target + neutralization every decision (nav:875-891); borrow tiers all names every decision (nav:839-865).
  Shared: only the RiskStore (v7:1090-1097).
- Multiple leverages in one pass exist (replay_nav_grid, `--aim-leverage` grid key nav:2412-2416, 3032-3033) but are
  refused under any v7 extension (nav:3220-3221); `--book-workers` refused under v7 (nav:2299-2304).
- `--calibrate-gross` would save one process + load + construction + 4-6 books + 5 capacity books if pass 1 replays S2
  only with cached desired targets. Measure with `--stage-timers` (nav:2558-2595) first.
- Replay not linear in L: dust band absolute (tr:352-354, 378); 1% ADV cap (replay_cost.cpp:104-106); impact ~ F^1.5;
  weights re-based on drifted NAV (nav:989); adv-hold Q*ADV/(L*NAV) (tr:490); vol-target L_t. G(L) ~ kL with k weakly
  L-dependent: one PM6-6 step (scripts/wave_rules.py:110-119 -- a Python rule, against PM8-12) is adequate but the matched
  replay is still required. NAV scale is the only exactly rescalable dimension.
- Release build blocked by the 1-ULP issue; IC exe measured 59-82% less CPU on Release.

## 6. Top findings (impact x effort)

| # | where | problem | consequence | fix | effort |
|---|---|---|---|---|---|
| 1 | v7:103; nav:2299-2304, 3220-3221 | leverage scaler (vol/risk target) and cost-v2 behind a thread-local global hook with shared mutable state | Y-1, X-10 sweeps and gross matching cannot use the grid or book workers; each variant = separate process, full passes | make the leverage rule a per-Book member of NavReplayConfig (engine interface, state per book); add to grid keys | M |
| 2 | v7:1121-1133; nav:52 | capacity curve re-dispatches argv, re-runs load + construction | ~1 replay wasted per run, 2 per gross-matched cell | run capacity books in the main lockstep: cap 16, identify by capacity_multiple(id) (v7:503, 535) | M |
| 3 | replay_cost.cpp:92 | std::pow the only non-exact op on the cost path | NAV pinned to Debug; Release unused | sqrt when delta == 0.5; probe test; re-pin ruling | S |
| 4 | v7:1107,1134; research_cycle.py:1176; wave_readers.py:79-81 | summary.json written before capacity pass and extras | crashed capacity pass reads as done | write v7_extras.json first or bind from summary.json; check in nav_step | S |
| 5 | wave_rules.py:110-119; nav:3130-3207 | gross calibration = 2 processes, 4 passes, rule in Python | ~3 extra passes per gross-matched cell | `--calibrate-gross` in C++: S2-only pass 1 with cached desired targets, pass 2 at L' | M |
| 6 | nav:3310-3497, v7:813-1080, nav:2150-2164 | 67 flags in two parsers; costs and constants compiled; 3,653-line file | a new rule = 12 files / +861 lines; new cost law = code change capped at 8 books | typed NavSpec JSON + registries for cost laws and leverage rules; split nav at the §1 seams | L |

Also (S each): adv-hold capacity overstatement (nav:800); missing exe identity in receipts.

Prior reviews: P-9 fixed in core, stands for v7-flagged cells; P-13 stands and worse (nav 2,732 -> 3,653 lines);
P-4 stands for NAV; P-12 fixed (strategy test exes registered with ctest, atx-impl/tests/CMakeLists.txt:146-154).
