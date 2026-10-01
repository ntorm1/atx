# Task MINE-FIX report: review-mine.md findings

Lane MINE-FIX, worktree `C:/atx-wt/pool-8`, branch `feat/platform-v8-minefix-20260930` from `864b7836`. The lane
stopped at owner instruction after MINE-2 (`d7f98f1b`) and was resumed to finish MINE-7, MINE-8, MINE-6, MINE-9,
MINE-10 and the cheap minors. Nothing was built in C++ (lane rules); every C++ change is written to compile under
`/W4 /WX` and re-read for it, but is not compiled. Python ran on synthetic data only. No real data, nothing dated
2024-01-01 or later opened, no subagents, no pushes.

## Done (one commit per finding, brief order)

| finding | commit | what |
|---|---|---|
| MINE-1 | `73a0da26` | Ledger chain head = SHA-256 of the registry log's first `bytes` bytes (the verb closes the registry, digests it, reopens it against its own head so another writer's append is refused). New `strategy_mine_ledger.{hpp,cpp}`: the C++ twin of `campaign_line`; the verb refuses to write a line it would refuse (`mine_ledger_line`, `mine_ledger_line_problem`). `ledger-campaign` checks the registry prefix hash and `registry_head.txt`. The Python test uses the verb's real output format; the C++ fixture calls `mine_ledger_line_problem` on the verb's line and checks the head against the registry bytes. |
| MINE-5 | `7ea78ade` | E-33a: `registry.count` = records this campaign added, `registry.total` = cumulative (verb, C++ check, `campaign_line`). A campaign adding 0 records is refused. Tests: two campaigns on one shared registry (C++ and Python). |
| MINE-4 | `5858228f` | E-32a: `--budget N` mandatory, >= the configuration's trial capacity (`mine_trial_capacity`), refused before any payload. Hurdle = `mined_hurdle(budget)`. `campaign.json` and the ledger line carry N; `campaign_line` refuses budget < count. |
| MINE-3 | `45a0dc9c` | The trial recipe binds the confirm window, role, fields manifest, library (VM and IC source pins) and pool. A campaign any of whose trials the registry holds under its recipe is refused before the confirm read. Ledger line carries rule, recipe_sha256, confirm {begin, end}; a second line on the same trial_id, recipe or name is refused. |
| MINE-2 | `631a81a0` | `kMinedMinConfirmRows = 200`; a confirm read counts only on its full window (`mined_confirm_defined`), else t = NaN. `ResearchIcRead.ic_dates` added (engine). |
| MINE-7 | `8051803e` | E-32a: `check_config` refuses a missing `--pool` / `--pool-sha256`; `check_pool` refuses a pool manifest without >= 1 regressor and >= 1 member, both before any payload. Test `StrategyMineCampaign.RefusesACampaignWithoutTheBook` (no pool, no member, no regressor; nothing written). |
| MINE-8 | `318e5a2c` | `pinned_manifest` (shared by `ResearchRole::geometry` and `::load`) reads the pinned text and calls engine `refuse_delisting_returns_signal_role` before parsing: a `universe.delisting.returns_applied` true role is refused from metadata, before any payload. Test `StrategyMineCampaign.RefusesADelistingReturnsRole` (geometry, load and run_mine refuse; the same role with false is read). |
| MINE-6 | `6b0aeb2f` | E-32a: `kMinedOverlapFactor = 1.55` and `kMinedMinDiscoverRows = 504` in `strategy_mine_rule.hpp`. Every hurdle reads t / F: the shortlist is f2 / F >= z(budget); the confirm is t / F >= 2 with p = Phi(-t / F) (`mined_overlap_corrected`, `MinedConfirm::t_corrected`). A discover window under 504 label rows is refused before any search. F and both floors are in the recipe; `campaign.json` hurdle and promotions carry F and the corrected t's. Tests: rule arithmetic on the corrected scale, constants and estimator pin (`StrategyMineRule.OverlapFactorIsRegisteredAndItsEstimatorIsTheVerbs`), 495-row discover refusal; pytest `atx-impl/tools/test_mine_overlap_factor.py`. Derivation below. |
| MINE-9 | `12f3dfd8` | Fixture redesign (below). `StrategyMineCampaign.RulePinsOnTheTemplates` pins E-32 (swap), sign freezing (neg, flip), N (hurdle and the exact shortlist) and the copy; the five-seed test pins stage-1 seed invariance and that the copy is never shortlisted; engine `ResearchIcFitnessTest.PartlySpannedAndNegativeCandidatesScoreTheDirectMarginalT`. |
| MINE-10 | `5cddc990` | Engine `SearchConfig::max_program_slots` (0 = off; signal path only) refuses, before the race and the full pass, a candidate whose compiled program needs more VM slots; it stays an unscored trial listed in `SearchResult::slot_refused_hashes`. The verb sets `kMineMaxProgramSlots = 8` (refused trials filed failed, reason `slot-bound`) and `mine_working_bytes` is derived term by term (below). Tests: engine `SignalFitnessPath.ProgramSlotBoundRefusesLargerProgramsBeforeEvaluation`, verb `StrategyMine.WorkingBytesAreTheDerivedSum`. |
| MINE-11 (m) | `791b18e0` | `signal_path_refusal` refuses a progress sink or resume with a non-empty mask or a non-default op catalogue, on any fitness path. Test `SignalFitnessPath.MaskAndCatalogueRefuseAProgressSink`. |
| MINE-12 (m) | `62f1b818` | A racing rung whose mask cannot be applied sets `signal_path_invalid`; `evaluate_generation` returns nothing from that generation. Unreachable today (run validates the mask): no test. |
| MINE-17 (m) | `d81303ab` | `trial_config` = FNV of (recipe tag, recipe SHA, canonical hash); the DSL text left the identity (stays in trials.csv). |
| MINE-18 (m) | `81be48a2` | `--min-dates` under 8 (the IC recipe's floor) refused in `check_config`. `StrategyMineCampaign.RefusesBadWindowsAndBoundsBeforeAnyPayload`: overlap, order, before TRAIN, past TRAIN, impossible date, min-dates. |

Covered by earlier commits: MINE-13 (recipe with VM/IC source pins, rule and recipe_sha256 in the line, registry and
`registry_head.txt` checked by `ledger-campaign`: MINE-1, MINE-3) and MINE-18's confirm-length case (MINE-2).

### MINE-6: the overlap factor derivation

`atx-impl/tools/mine_overlap_factor.py` (seed 20260930; reproduced here: exit 0, about 14 s). Null: 30 names, i.i.d.
N(0, 1) returns; the label of row t is the sum of returns t+2 .. t+22 (delay 1, h 21); the signal is i.i.d. per name
and constant over the window (fully persistent: the worst case, its daily ICs share label overlap at every lag < 21);
the daily statistic is the zero-regressor marginal kernel (Pearson of the centred signal rank with the label ranks);
t = `summarize_rank_ic(daily, 21)` (Bartlett lag 21, small-sample n / (n - 1), undefined days dropped). The pytest and
the C++ test pin the script's `summarize_t` and the engine's `summarize_rank_ic` to the same values on a fixed series
(2.096947998340487; 11 defined days with the lag clamped: -0.3360738764789554).

- (a) confirm gate, 200 label rows, 20,000 draws: the (1 - 2 Phi(-2)) quantile of |t| over 2 = **1.5422** (sd ratio
  1.5061; shape 1.48 / 1.70 / 1.86 at 90 / 99 / 99.8%).
- (b) discover tail, 504 label rows, 10,000 draws: the 99.8% quantile of |t| over z(.999) = **1.3931** (sd 1.3129).
- F = ceil(max(a, b), 2 decimals) = **1.55**. (a) binds.

One-off deeper check (scratch script, synthetic, not committed; null_t of the same script, other seeds):

| rows | draws | ratio at 99.8% | 99.95% | 99.99% | per-trial rate of t / 1.55 >= z at N = 25 / 100 / 1,000 (nominal .002 / .0005 / .00005) |
|---|---|---|---|---|---|
| 504 | 200,000 | 1.421 | 1.461 | 1.513 | .00093 / .00025 / .000045 (9 events) |
| 1,074 | 60,000 | 1.312 | 1.345 | 1.323 | .00038 / .000033 / 0 |

So on the 504-row floor the hurdle is conservative up to about N = 1,000 and the ratio keeps rising into the tail;
for budgets in the thousands on discover windows near 504 rows it is at or past nominal (concern 1).

### MINE-9: the fixture

Sessions are calendar days 2018-12-14 .. 2023-12-31 (1,844), 16 names. Drivers p1..p4, m1, m2, n1, neg, flip, e i.i.d.
N(0, 1); r(t) = .01 x(t-2) + .005 e(t), x = p1 + p2 + p3 + m1 - neg + (p4 - flip before row 1,295 = 2022-07-01; flip
from it). Fields p1, p2, p3, copy = m1 + .02 noise, n1, swap (p4 before the confirm begin, m1 from it), neg, flip.
Pool: regressor book = rank(m1), member m2 (independent: only the marginal term can stop the copy). Discover
[2020-01-01, 2022-07-01) (890 label rows), confirm [2022-07-01, 2024-01-01) (527). The stage-2 campaigns mine
{p1, p2, p3, copy, n1} (no combination can cancel); `RulePinsOnTheTemplates` mines all eight with stage 2 off, no racing,
budget 1,000, cap 64, so every template is read and the shortlist is exactly recomputable. A numpy replica (same
generator, Bartlett t's; the IC runner's conservative t approximated) reads on t / 1.55: discover f2 rank(p1) 5.1,
p2 6.4, p3 6.0, swap 4.6, neg 5.6 and flip 6.8 (sign -1), copy -1.2 (its f1 7.3), n1 0.3, hurdle z(1000) 4.06, 27 rows
shortlisted, every rho under .70 (delta templates about .66 to their rank); confirm p1 4.3, p2 5.0, p3 4.3, neg 6.1,
flip -3.6, swap undefined (527 / 527 rows spanned by the book) with raw IC t / F 4.8. On the stride-2 rung of the
five-seed configuration rank(copy) ranks 4th, rank(p1) 9th, rank(p3) 14th of 55; the 28th |t| is under 2. Engine
replica: f + g sign +1, raw t 17.7, marginal t 7.8; -2 f + g sign -1, marginal t +8.1.

### MINE-10: the memory formula and worked numbers

`mine_working_bytes` (strategy_mine.hpp documents each term; C = dates x names, H = dates x ceil(names / 2), F = 3 +
extras, S = 8, W workers, R rungs, T = `mine_trial_capacity`, P = records of a reopened registry):

    64 MiB + research_role_bytes + (regressors + members) C 8 + 2 x 49 C + R H (8F + 54) + R H (8F + 1)
    + ((W + 1) C + W R H)(8S + 1) + W (C + R H) 8 + (W (1 + R) + 1)(128 names + 80 dates)
    + T (8 dates + 16 KiB) + 512 KiB + 128 (P + T) + shortlist C 8 + (members + shortlist) names 8

| case | bytes |
|---|---|
| 4-year role 1,405 x 6,100, 16 fields, 3 regressors, 32 members, W 4, R 1, shortlist 16, T 272 (default search) | 11,651,171,382 (10.85 GiB; engines 3.72 GiB, pool 2.29 GiB, role 1.32 GiB, trial reads 7 MiB) |
| same, T 9,968 (a 10,000-trial search) | 11,920,254,774 (11.10 GiB; trial reads 263 MiB) |
| configuration bounds: 4,096 x 20,000, 64 fields, 11 / 64, W 64, R 2, shortlist 256, T 1,049,280 | 1,184,945,709,312 (1.1 TB): refused against the 64 GiB `--max-memory-mib` ceiling |

## Deviations from the design notes (and why)

- MINE-6: implemented as t / F >= z (the ruling's words) rather than f2 >= z x F; `mined_hurdle(N)` stays the plan's
  Bonferroni value so its existing pins hold. Equivalent up to rounding at the boundary.
- MINE-9: the pins run on a separate stage-1-only campaign over all eight fields; the five-seed acceptance keeps the
  original five fields. With swap / neg / flip in stage 2, a combination such as rank(p1 - flip) shortlists above
  rank(p1), can block it at rho about .7 and then fails the confirm, so the five-seed promotion assertions would be
  replica-unverifiable and fragile.
- MINE-10: the slot bound is an evaluation-time refusal (a compile pre-pass in `evaluate_generation`) instead of a
  generation-time bound in `make_child` / `init_population`: it covers seeds, children, immigrants and grammar fill
  uniformly, needs no refused combinations, keeps rung engines bounded too, and files the trial (the review's "file as
  failed"). No node bound: VM memory scales with slots only (scratch is O(dates) / O(names)). The per-trial term uses
  the capacity (<= budget), the true bound on full-pass reads. Deriving the terms also corrected the IC cache term
  (48 + 1 B per cell, it was 25) and added both strided-panel copies, the promotion engine and the registry index.

## Cross-lane edits

- `atx-impl/CMakeLists.txt` (one source line, `src/strategy_mine_ledger.cpp`; MINE-1).
- `atx-impl/tools/test_trial_ledger_rules.py` (FIX-C's E-33 test, new `campaign_line` keywords; FIX-C contracts kept).
- `atx-engine/include/atx/engine/factory/search_driver.hpp`, `atx-engine/src/factory/search_driver.cpp` beyond the hook
  itself: the race's fresh-list rebuild became the shared `drop_fresh` lambda (same operations), and MINE-11 refuses a
  mask or catalogue with a checkpoint on the legacy path too.

## How root verifies (when built)

- Build: `atx-impl-strategy-mine-tests` and `atx-engine-factory-tests` (search_driver.hpp changed: a wide rebuild).
  `check` first: `atx-impl/src/strategy_mine.cpp`, `strategy_mine_ledger.cpp`, `strategy_mine_trials.cpp`,
  `strategy_mine_promote.cpp`, `strategy_mine_rule.cpp`, `strategy_research_role.cpp`,
  `atx-engine/src/factory/search_driver.cpp`, `atx-engine/src/factory/research_ic_fitness.cpp`,
  `atx-impl/tests/strategy_mine_test.cpp`, `atx-engine/tests/factory/factory_signal_fitness_test.cpp`.
- gtest: `atx-impl-strategy-mine-tests --gtest_filter=StrategyMine*:SignalFitness*:ResearchIc*:OpCatalogCfgTest.*`;
  then the whole `atx-engine-factory-tests` (387 at v8-7) for the race refactor, in particular `NsgaSearch.*` and
  `FactoryFidelity*`.
- Golden: `0x889874a3b9b29c55` must hold at 1 and 4 workers (`SignalFitnessDefaults.*`,
  `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`). By reading the default path is unchanged: the slot block is skipped at
  0, `drop_fresh` performs the race's former operations, the MINE-11 check is false with the defaults, and the MINE-12
  flag is never set on a valid run.
- pytest (passed here, 16): `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
  scripts/tests/test_research_ledger.py atx-impl/tools/test_trial_ledger_rules.py
  atx-impl/tools/test_mine_overlap_factor.py`.
- Not compiled; the C++ fixture expectations rest on numpy replicas (approximate: the IC runner's conservative t,
  stage 2 and racing are not replicated). The exact-equality pins (shortlist recomputation, direct marginal t, working
  bytes, estimator) do not depend on a replica.

## Not done

- MINE-14 (m, cap before the rho step): needs `promote` to stream candidates in f2 order (the cap bounds the signals
  held in memory, which the admission counts); not cheap.
- MINE-15 (m, an undefined rho pair does not block): needs a ruling (fail the pair, or require joint dates >=
  min_dates).
- MINE-16 (m, a rung VM failure filed as racing-rejected): needs a failure channel through `race()`; not cheap. N still
  counts those trials.

## Open risks and concerns for the project manager

1. MINE-6 tail: F = 1.55 is derived at the confirm gate (95.4%) and the 99.8% discover tail. The Bonferroni hurdle of
   any N > 25 lies deeper; on the 504-row floor the ratio rises (1.46 at 99.95%, 1.51 at 99.99%) and the per-trial
   rate reaches nominal near N = 1,000 (9 of 200,000 null draws). For budgets in the thousands on discover windows near
   504 rows the hurdle may be anti-conservative. Options (ruling): raise `kMinedMinDiscoverRows` toward ~750 (about the
   most TRAIN leaves beside a 200-row confirm), cap N on short windows, or derive F at the budget's own level. The
   confirm's BY p = Phi(-t / F) also sits in the tail where the 200-row ratio is 1.70 (99%), so BY at short confirm
   windows is approximate.
2. A real 4-year campaign at the default search needs about 10.9 GiB: `--max-memory-mib` (default 2,048) must be
   raised, against the 15.7 GB host (E-6).
3. Real stage-2 searches may file `slot-bound` failures (counted in N, conservative); the fixture asserts none.
