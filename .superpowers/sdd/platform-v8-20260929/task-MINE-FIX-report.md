# Task MINE-FIX report: review-mine.md findings (STOPPED at owner instruction)

Lane MINE-FIX, worktree `C:/atx-wt/pool-8`, branch `feat/platform-v8-minefix-20260930` from `864b7836`.
Nothing was built in C++ (lane rules); every C++ change is written to compile under `/W4 /WX` but is not compiled.
Python ran on synthetic data only. No real data, nothing dated 2024-01-01 or later opened, no subagents, no pushes.

## Done (one commit per finding, brief order)

| finding | commit | what |
|---|---|---|
| MINE-1 | `73a0da26` | Ledger chain head = SHA-256 of the registry log's first `bytes` bytes (the verb closes the registry, digests it, reopens it against its own head so another writer's append is refused). New `strategy_mine_ledger.{hpp,cpp}`: the C++ twin of `campaign_line`; the verb refuses to write a line it would refuse (`mine_ledger_line`, `mine_ledger_line_problem`). `ledger-campaign` checks the registry prefix hash and `registry_head.txt`. The Python test now uses the verb's real output format (real registry bytes); the C++ fixture calls `mine_ledger_line_problem` on the verb's line and checks the head against the registry bytes. |
| MINE-5 | `7ea78ade` | E-33a: `registry.count` = records this campaign added, `registry.total` = cumulative (verb, C++ check, `campaign_line`). A campaign adding 0 records is refused (backstop; MINE-3 refuses it first). Tests: two campaigns on one shared registry (C++ and Python); counts sum to the registry once. |
| MINE-4 | `5858228f` | E-32a: `--budget N` mandatory, must be >= the configuration's trial capacity (`mine_trial_capacity`: templates + stage-2 population x generations), refused before any payload. Hurdle = `mined_hurdle(budget)`. `campaign.json` (budget, hurdle.budget, search.capacity) and the ledger line carry N; `campaign_line` refuses budget < count. Tests: hurdle == `mined_hurdle(budget)` exactly on a fresh and a shared registry; missing / below-capacity budget refused. |
| MINE-3 | `45a0dc9c` | The trial recipe binds the confirm window, role, fields manifest, library (VM and IC source pins via `ic_cache_vm_identity` / `ic_result_cache_identity`) and pool; the per-campaign field-payload subset left the recipe (the fields manifest pins every payload). A campaign any of whose trials the registry holds under its recipe is refused before the confirm read and before any output. Ledger line carries rule, recipe_sha256, confirm {begin, end}; trial_id = sha256(["mining-campaign", recipe, head])[:16]; `check_line` refuses (not skips) a second campaign line on the same trial_id, recipe or campaign name; `ledger-campaign` checks campaign.json's recipe hashes to recipe_sha256. |
| MINE-2 | `631a81a0` | `kMinedMinConfirmRows = 200` registered with mined-v1. Confirm window with fewer mature label rows refused before the search; a confirm read counts only when the h21 IC is defined on >= 200 rows and the marginal IC on every row (`mined_confirm_defined`), else t = NaN (unconfirmed). `ResearchIcRead.ic_dates` added (engine). Promotions report confirm_defined / rows / date counts. |

Cross-lane edits: `atx-impl/CMakeLists.txt` (one source line, `src/strategy_mine_ledger.cpp`);
`atx-impl/tools/test_trial_ledger_rules.py` (FIX-C's E-33 test, updated to the new `campaign_line` keywords; FIX-C
contracts kept: kind mining-campaign, count 0, adds 0 to every N, registry count printed beside N, no defect of a
campaign line).

## How root verifies (when built)

- Build: `atx-impl-strategy-mine-tests` (and `atx-engine-factory-tests` for the `ResearchIcRead.ic_dates` header change).
  `check` first: `atx-impl/src/strategy_mine.cpp`, `strategy_mine_ledger.cpp`, `strategy_mine_trials.cpp`,
  `strategy_mine_promote.cpp`, `strategy_mine_rule.cpp`, `atx-engine/src/factory/research_ic_fitness.cpp`.
- gtest: `atx-impl-strategy-mine-tests --gtest_filter=StrategyMine*:SignalFitness*:ResearchIc*:OpCatalogCfgTest.*`
- pytest (passed here, 13): `scripts/tests/test_research_ledger.py atx-impl/tools/test_trial_ledger_rules.py`.
- Identity: the default-off engine path is untouched by these commits (only a new `ResearchIcRead` member);
  golden `0x889874a3b9b29c55` should hold (`SignalFitnessDefaults.*`).
- Not compiled: the fixture tests were edited (shared-registry test now changes only the confirm window, because a
  registry's `pnl_len` is the discover label rows and must match on reopen).

## STOPPED HERE (owner stop)

Remaining findings, in brief order:

1. MINE-7: `--pool` mandatory for mined-v1 (>= 1 regressor and >= 1 member), refused before any payload; test.
2. MINE-8: `ResearchRole` loader calls `refuse_delisting_returns_signal_role` (put it in `pinned_manifest`, so
   `geometry()` refuses before payload too); fixture role with `universe.delisting.returns_applied` true; test.
3. MINE-6: the overlap factor. Derivation is DONE but not wired: `atx-impl/tools/mine_overlap_factor.py` (committed
   with this report, nothing calls it yet), seed 20260930, 30 names, null = i.i.d. N(0,1) returns, label = sum of
   returns t+2..t+22, fully persistent signal (worst case), the verb's t = Bartlett lag 21 small-sample HAC of the
   intercept-only marginal IC. (a) confirm gate at 200 rows, the (1 - 2 Phi(-2)) quantile of |t| over 2 = 1.5422;
   (b) discover tail at 504 rows, the 99.8% quantile ratio = 1.3931; F = ceil(max) to 2 decimals = **1.55**
   (sd ratio 1.506 at 200 rows, 1.313 at 504; tail shape at 200 rows: 1.48 / 1.70 / 1.86 at 90 / 99 / 99.8%).
   Remaining: pin `kMinedOverlapFactor = 1.55` and `kMinedMinDiscoverRows = 504` in `strategy_mine_rule.hpp`;
   shortlist on f2 >= z(budget) x F and confirm on t / F >= 2 with p = Phi(-t / F); refuse a discover window under
   504 label rows; put F in the recipe and campaign.json; pytest `test_mine_overlap_factor.py` (runs the script,
   about 16 s, asserts 1.55 and the header constants); a C++ test pinning `summarize_rank_ic` against the script's
   `summarize_t` on a fixed series. Open risk: beyond the 99.8% level (Bonferroni at N >= ~250 on 504-row discover
   windows) the factor is checked by extrapolation only.
4. MINE-9: fixture redesign (verified with a numpy replica, not committed): discover [2020-01-01, 2022-07-01),
   confirm [2022-07-01, 2024-01-01); fields p1, p2, p3, copy, n1 plus swap (= p4 before the confirm begin, m1
   after), neg (loading -1 both windows), flip (-1 in discover, +1 in confirm); returns load p4 only before the
   confirm begin, noise .005; pool regressor book = rank(m1), member m2 (independent, so only the marginal term can
   stop copy). Replica (F 1.55): confirm z rank(p1) 6.34, rank(p2) 3.85, rank(p3) 6.03, rank(neg) 4.25 (sign -1),
   rank(flip) -3.06, rank(swap) marginal t undefined (raw t / F 5.0, a raw-IC confirm would admit it). Tests to add:
   E-32 pin (swap), sign pin (neg admitted with sign -1, flip rejected though |z| >= 2), hurdle == z(budget) x F,
   shortlist == every evaluated row with f2 >= hurdle, stage-1 rows identical across seeds; engine test with a
   half-spanned candidate checked against a direct `marginal_rank_ic_day` + `summarize_rank_ic` and a negative-IC one.
5. MINE-10: `SearchConfig::max_program_slots` / `max_program_nodes` (0 = off, default path identical), enforced in
   `make_child`, `init_population`'s seeded mutations and seeds (refused combinations: grammar fill, immigrants);
   the verb sets 8 slots / 64 nodes; `mine_working_bytes` derived from the slot bound, budget x (label_rows x 8 +
   16 KiB per-trial genome / log allowance), both strided-panel copies; formula and worked number at the bounds.

Minors MINE-11..18 not started (MINE-13's recipe / rule / registry-head checks and MINE-18's confirm-length refusal
are covered by MINE-1, MINE-3 and MINE-2 above).
