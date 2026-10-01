# Review MINE: the mining verb (lane H3) at d22b735a

Reader MINE, read-only. Every file was read with `git show d22b735a:<path>`, and every diff with
`git diff fd2ff7a8 d22b735a`, in `C:/atx-wt/pool-2`. Ruling E-33a was read at `a7673e0a` (docs only, after d22b735a).
Nothing was built, no C++ test was run and no real data was opened. Nothing dated 2024-01-01 or later was opened.
One Python probe ran on a scratch copy of `backtest_integrity.py` (d22b735a) in the session scratchpad, with bytecode
writes off and no repository file touched (MINE-1).

Scope:
- H3 merge e2bb716b (lane 339c07b1: engine part 2 2beb83c2, verb part 3 12fc27a9) and integration fix ccb66a87.
- The signal-fitness hook, mask and catalogue in `search_driver`; `research_ic_fitness`; `ResearchRole`.
- `strategy_mine*`; the `ledger-campaign` verb and its test.

Binding texts:
- progress.md E-32, E-33 and E-33a (a7673e0a);
- v8-prereg.md rule 10;
- task-H-brief.md H-3 (mined-v1 and acceptance);
- the method in review-w1-part2-brief.md; review-w1-T.md T-1 as the standard for tests.

Severity: **I** important, **M** medium, **m** minor. There is no I finding. No defect produces a wrong number in v8
today, because no campaign runs (OD-7). Every M finding below must be closed before an OD-7 campaign.

## Answers to the brief's questions

| question | answer | findings |
|---|---|---|
| Can the hook change a result when off (bit identity across worker counts)? | No, by reading. The default path differs only by the catalogue rebuild (same build), `set_parsimony` (the same values), output-only `fidelity_rejected_hashes`, and an empty refusal. Golden 0x889874a3b9b29c55 is pinned at 1 and 4 workers (`factory_signal_fitness_test.cpp:245-265`). That pin is ScalarRaw only, with no parsimony and no racing. The other paths are covered by the existing NsgaSearch / FactoryFidelity pins (387/387 at v8-7). | clean (MINE-11 and MINE-12 are latent, legacy path) |
| Can a mined signal be promoted on a statistic other than E-32's? | As coded, the confirm is the discover sign x the confirm marginal HAC t (`strategy_mine_promote.cpp:117-121`). But: the confirm t has no date floor (MINE-2); with no `--pool` the "marginal" t is the raw IC t and rho checks nothing (MINE-7); and the Bonferroni value is applied to a t that is too large on overlapping labels (MINE-6). | MINE-2, MINE-6, MINE-7 |
| Can the discover and confirm windows overlap or reach past the seal? | No. The calendar order is enforced (`strategy_mine.cpp:149-153`). Labels mature at the window end (`ic_screen.cpp:519-526`), so discover returns end at row `de - 1` and confirm entries start at row `cb + 1 > de - 1`. The role reader refuses `score_end_ns` past the seal and any sealed session before any payload (`strategy_data.cpp:109, 139-141`). Tests cover only two of these cases (MINE-18). | clean |
| Is the registry's chain head bound to the campaign's inputs? | Partly, and only indirectly. Each record's config hash is a 64-bit FNV over a recipe SHA, which binds role, fields, payloads, pool, the discover window and min_names / min_dates. The confirm window is not bound, and neither is the VM / library. The ledger line carries neither the recipe nor the rule, and the head is a u64. | MINE-1, MINE-3, MINE-13 |
| Can a campaign be re-run to add trials without a new budget line? | Yes. A fresh `--registry` resets N and the hurdle. A re-run on the same head adds 0 records and makes a new confirm read with no ledger trace. No budget is declared in advance. Today no campaign line can be appended at all. | MINE-1, MINE-3, MINE-4, MINE-5 |
| Can the fixture tests fail on a wrong promotion rule (T-1 standard)? | No. The rho step also catches the copy. The planted signals are book-orthogonal and all have sign +1, and the hurdle is checked only as > 3. | MINE-9 |
| Is the 8 VM slots per cell memory estimate derived or guessed? | Guessed: the figure is borrowed. 8 is W0-f's figure for library v7.1 (qmj_safety). Mined programs have no depth or slot cap, and per-trial retention is outside the estimate. | MINE-10 |

## Findings

### MINE-1 (M) `ledger-campaign` refuses every real campaign: the verb writes a 16-hex chain head and `campaign_line` requires 64

- Where:
  - `atx-impl/src/strategy_mine.cpp:417` (`head_hex = hex16(head.head)`; `TrialChainHead::head` is `u64`, `trial_registry.hpp:250-254`), `:463`, `:496`;
  - `atx-impl/tools/backtest_integrity.py:763-764` (`len(registry_head) == 64` or ValueError);
  - `scripts/research_ledger.py:226` (passes `campaign.json` `registry.head`);
  - tests `scripts/tests/test_research_ledger.py:135-172` (head `"ab" * 32`) and `atx-impl/tests/strategy_mine_test.cpp:433-446` (expected line rebuilt from the verb's own head; `campaign_line` never called on it).
- What is wrong:
  - `campaign_record` hands the verb's 16-hex head to `campaign_line`, which raises.
  - `campaign_main` then exits 2 and appends nothing. So the E-33 wiring the integration log reports ("ledger_line.json is now exactly campaign_line(...)") cannot ledger any output the verb can produce.
  - The C++ test pins a line that `campaign_line` would refuse.
  - The Python test feeds a 64-hex head that no verb writes.
  - Neither test can fail.
  - Separately, the "chain head" is a 64-bit chained digest, not the SHA-256 the ledger field names. So `trial_id` dedup also rests on 64 bits.
- Blocks: E-33 (the campaign's registry count in the cycle ledger) and rule 10's audit trail for every OD-7 campaign. With no line, `campaign_registry_count` stays 0 and Appendix A prints no campaign.
- Fix:
  - Decide the head's ledger form: a SHA-256 over the exported `(records, head)` pair, or a 16-hex `registry_head` key that `campaign_line` accepts.
  - Add one test that runs `ledger-campaign` on a directory the C++ verb wrote (the fixture already makes three).
- Verified. Probe: `campaign_line("fixture", path, "0123456789abcdef", 81, ...)` raises ValueError "...names its registry's chain head (a SHA-256 hex digest)". The same call with `"ab" * 32` passes (trial_id ca4e08b6c45bf39a). C++ not run.

### MINE-2 (M) The confirm read has no date floor: a 25-session confirm window confirms on a "HAC t" from three overlapping label rows

- Where:
  - `strategy_mine.cpp:171` (the only floor: 22 + 3 rows);
  - `strategy_mine_promote.cpp:67-85`, `:117-121` (reads `confirms[j].marginal_t`; never `ic_defined` or `marginal_dates`);
  - `research_ic_fitness.cpp:166-171`;
  - `marginal_rank_ic.cpp:210-228` (`hac_t` defined from 2 dates, lag clamped to n - 1);
  - `ic_screen.cpp:139-187` (`validate` never compares the window with `min_dates`).
- What is wrong:
  - `--min-dates` (default 128) gates only the raw IC estimate. Discover inherits the gate through the `IcUndefined` screen; confirm does not.
  - With `--confirm-begin` / `--confirm-end` 25 sessions apart, the marginal series has 3 values whose labels share 19 of 21 days.
  - Their Bartlett variance is near 0, so |t| is large whenever the marginal IC of those three weeks has the discover sign.
  - mined-v1's "one confirm read at HAC t 2.0" then reduces to the sign of one three-week window. Nothing registers the confirm window length, and the CLI accepts any length.
- Blocks: the mined-v1 / E-32 confirm under OD-7.
- Fix:
  - Refuse a confirm window with fewer than `min_dates` mature h 21 rows.
  - Treat a confirm read with `!ic_defined` or `marginal_dates < min_dates` as unconfirmed (NaN t).
  - Fix the confirm length in the campaign budget (MINE-4).
- Verified by reading. Not run.

### MINE-3 (M) A campaign can be re-confirmed without trace: the confirm window binds nothing, and a re-run adds no record and no ledger line

- Where:
  - `strategy_mine.cpp:369-383` (recipe: the discover window only), `:391-396`;
  - `strategy_mine_trials.cpp:180-181` (config hash = recipe SHA, canonical hash, DSL);
  - `backtest_integrity.py` `campaign_line` (trial_id = (kind, head));
  - `strategy_mine_test.cpp:475-488` (the re-run pinned as intended).
- What is wrong:
  - Take a re-run with `--registry-head`: the same role, fields, pool, discover window and seed, but another confirm window (or `--max-promotions`).
  - Every trial dedups (`inserted` false) and the head is unchanged. A new confirm read is made, and a new `mined_members.json` is written.
  - Its ledger line has the first campaign's trial_id. So `research_ledger` skips it as "already present", and under E-33a (count 0) `campaign_line` refuses it.
  - The second read of 2023 leaves no ledger trace either way. The registry cannot say which confirm window produced the members.
  - mined-v1 allows "one confirm read".
- Blocks: mined-v1's single confirm read; the count of looks at the confirm year.
- Fix:
  - Make the confirm read a recorded event: one registry record per (recipe, confirm window, candidate), or a refusal when a campaign with the same recipe and head already exists.
  - Put the confirm window in the campaign line's identity.
- Verified by reading.

### MINE-4 (M) Pre-registration rule 10 ("campaign budget fixed in advance") has no input and no check

- Where:
  - `strategy_mine.hpp:37-57` (`MineConfig`: no budget);
  - `strategy_mine.cpp:93-97` (population up to 4,096, generations up to 256), `:395`, `:402` (hurdle from the realised `n_raw`, after recording);
  - `strategy_mine_trials.cpp:134-150` (a registry path that does not exist starts at N 0 with no anchor).
- What is wrong:
  - N is whatever the search reached, and nothing declares a budget before the search or refuses a campaign that exceeds one.
  - A new `--registry` path starts N afresh, so a re-run with another seed or field list gets a fresh, lower hurdle, and earlier campaigns' trials do not count against it.
  - Together with MINE-1, no campaign line reaches the ledger at all, so such re-runs are invisible today.
  - Note: mined-v1's "effective trial count" is read as `n_raw`. That is conservative, and it is not a defect.
- Blocks: pre-registration rule 10; the OD-7 grant.
- Fix:
  - Add a required `--budget N`, written to `campaign.json` and to the ledger line, with the confirm window.
  - Refuse when the distinct trials exceed the budget.
  - Compute the hurdle at max(budget, n_raw).
  - Refuse a fresh registry for a campaign id already ledgered.
- Verified by reading.

### MINE-5 (M) Ruling E-33a is not implemented: `registry.count` is the cumulative `n_raw`, and no test can tell the difference

- Where:
  - `strategy_mine.cpp:496`;
  - `research_ledger.py:226` (`reg["n_raw"]`);
  - `strategy_mine_test.cpp:399-406`, `:444`, and `test_research_ledger.py:142`: every fixture uses a fresh registry, where `new_records == n_raw`;
  - `strategy_mine.cpp:391-413` (records and head are written before `promote`).
- What is wrong:
  - E-33a sets count = the records this campaign added, with the cumulative size carried as `registry.total`. The code writes `count = n_raw` and no `total`.
  - So on a shared registry, `campaign_registry_count` counts earlier campaigns again.
  - Second case: if `promote` fails after recording, the records are appended and the head is exported, but no `campaign.json` exists. A re-run then adds 0. Under E-33a its count is 0, which `campaign_line` refuses, so those trials can never be ledgered.
- Blocks: E-33a; the campaign registry count in Appendix A.
- Fix:
  - Write `count = new_records` and `total = n_raw` in the verb and in `campaign_record`.
  - Add a two-campaign shared-registry test.
  - Rule on what a 0-record campaign writes.
- Verified. The code predates the ruling (a7673e0a); the test gap stands either way.

### MINE-6 (M) The Bonferroni hurdle is applied to a Bartlett lag-21 t that is too large on overlapping h 21 labels

- Where:
  - `research_ic_fitness.hpp:45` (`kResearchIcHacLag = 21`);
  - `research_ic_fitness.cpp:166-169`;
  - `marginal_rank_ic.cpp:210-228` (undefined dates compacted);
  - `eval/hac.hpp` `kernel_weight` (Bartlett 1 - j / (L + 1));
  - `strategy_mine_rule.cpp:14-17`.
  - Same statistic as review-w1-B B-7 (m, report-only there). Here it gates.
- What is wrong:
  - For a persistent signal (the `ts_mean(f, 252)` templates, slow fundamentals), the daily marginal IC on 21-session overlapping labels has an ACF of about (21 - j) / 21 at lags 1..20.
  - The true long-run variance is about 21 g0. Bartlett(21) estimates about 14.3 g0, so t is about 1.21x too large.
  - At N = 100 the hurdle 3.48 corresponds to z of about 2.87: one-sided p .0021 against the nominal .00025, about 8x. At N = 1,000, 4.06 corresponds to z of about 3.35.
  - Persistence beyond 21 sessions, and compacting undefined dates (which the runner's `estimate` explicitly avoids), make it worse.
  - The confirm gate is inflated the same way: t 2.0 corresponds to z of about 1.65.
  - The engine already has `TStatRule::HorizonAwareV3` (uniform kernel, lag >= h - 1) for exactly this case.
- Blocks: mined-v1's family-wise level under OD-7.
- Fix (a ruling): the mined-v1 t uses the uniform kernel at lag >= 20 on the calendar series (or the runner's lag 2h); otherwise, re-derive the hurdle for the K6 statistic by simulation.
- Unverified:
  - This is arithmetic under an idealised triangular ACF; it was not simulated.
  - The fixture's drivers are i.i.d. over days, so its series carry no such ACF and cannot show it.

### MINE-7 (M) Without `--pool`, mined-v1 runs on the raw IC and checks rho against no member

- Where:
  - `strategy_mine.cpp:92` (pool optional), `:319-327`;
  - `strategy_mine_pool.cpp:77`, `:107`;
  - `marginal_rank_ic.hpp` (zero regressors: a plain rank IC with an intercept-only residual);
  - `strategy_mine_promote.cpp:41-63`.
- What is wrong:
  - E-32 exists because "a raw-IC confirm would pass a spanned signal".
  - With an empty pool, f2 and the confirm t are the raw rank IC t, and the rho step has no member rows. So a mined copy of a book member is admitted.
  - The verb accepts this silently; `campaign.json` shows only `regressors 0, members 0`.
- Blocks: E-32; mined-v1 "abs(rho) at most .70 to every member".
- Fix: under mined-v1, require `--pool` with at least one regressor and one member.
- Verified by reading.

### MINE-8 (M) The shared research-role loader does not refuse a delisting-returns role (B-3; settles review-w1-fixes F-7)

- Where:
  - `strategy_research_role.cpp:105-162`, which has no call to `refuse_delisting_returns_signal_role`;
  - compare `strategy_ic_admission.cpp:216`, `strategy_marginal_ic.cpp:412` and `strategy_target_replay.cpp:825`, which do call it.
- What is wrong:
  - F-7 left open whether H3's loader calls the B-3 refusal. It does not (grep at d22b735a).
  - The miner takes signals and IC labels from the same role.
  - So a `--delisting-returns` role feeds the hindsight-classified terminal close (classified after T) into every close-based mined expression at T, and into cross-sectional ranks.
- Blocks: the hidden-data discipline of any OD-7 campaign on a delisting-returns role (B0c's label role is one); every later user of `ResearchRole` (the IC-runner follow-up).
- Fix: call the refusal in `ResearchRole::load` (or in `pinned_manifest`), with a fixture role carrying `returns_applied`.
- Verified by grep.

### MINE-9 (M) The fixture acceptance cannot fail on a wrong promotion rule (T-1 standard)

- Where:
  - `strategy_mine_test.cpp:232-249` (pool: regressor `book` = rank(m1), member `m1`), `:384-451`, `:455-489`;
  - `factory_signal_fitness_test.cpp:534-587`.
- What is wrong:
  - **The copy is caught twice.** copy = m1 + .02 noise, so rank(copy) has |rho| of about 1 against pool member m1 (> .70).
    - A shortlist on f1 (no marginal term) rejects it at the rho step.
    - The brief's "copy rejected by the marginal term" is not pinned. `:427` checks only that its f2 is below the hurdle, which is a property of the read, not of the rule.
  - **E-32 is not pinned.** p1..p3 are independent of the book, so their confirm raw IC t and marginal t agree (replica: 4.9 to 8.5), and no copy variant reaches an f2 shortlist.
    - So a confirm on the raw IC t admits the same set.
  - **Sign freezing is not pinned.** Every planted sign is +1 and nothing negative reaches the shortlist, so |t| in place of sign x t passes.
  - **The hurdle's N is not pinned.** `:407` checks `hurdle > 3.0`, never `hurdle == mined_hurdle(n_raw)`.
    - Planted f2 is 11 to 14 and noise f2 is near 0, so any hurdle in roughly [2.5, 11] is invisible, for example N = evaluated or N = new_records.
  - **"5 seeds" is weaker than it reads.** Stage 1 does not depend on the seed (one generation, `explore` false, no mutation). So the noise gate is the same 55 templates five times, plus small stage-2 runs.
  - **The engine test cannot see a wrong f2.** `PlantedSignalScoresAndIsRecorded` has no regressor (f2 = the raw IC t); `CandidateSpannedByTheRegressorsIsScreened` is fully spanned (screened).
    - No candidate is partly spanned and none has a negative IC. So f2 = |marginal t| or f2 = raw t passes both tests.
- Blocks: the H-3 acceptance as a guard of mined-v1 and E-32 under OD-7.
- Fix:
  - Keep the book regressor but drop m1 from `members`, so that only the marginal term can stop the copy.
  - Add a field equal to p4 on discover rows and to m1 on confirm rows. It is shortlisted and passes rho; a marginal confirm rejects it and a raw-IC confirm admits it. This pins E-32.
  - Add a field whose return loading is negative, to pin sign freezing.
  - Assert `hurdle == mined_hurdle(n_raw)` exactly.
  - In the engine test, add a half-spanned candidate whose f2 is checked against a direct `marginal_rank_ic_day` + `summarize_rank_ic` computation.
- Verified by reading and arithmetic. Not run.

### MINE-10 (M) The memory admission is borrowed, not derived: 8 VM slots is the library v7.1 maximum, and two growing terms are outside the estimate

- Where:
  - `strategy_mine.cpp:258-278` (`:272` 8 slots; `:268` a flat 64 MiB for "metadata, genomes, search");
  - progress.md W0-f (8 = v7.1 max compiled slots, qmj_safety);
  - `crossover.hpp:55-58` (`CrossoverCfg` has `max_lookback` only, no depth or node cap);
  - `vm.hpp:1155-1163` (the SlotPool grows to the largest program evaluated);
  - `research_ic_fitness.cpp:275-288` with `strategy_mine_detail.hpp:46-50` (every full read keeps `label_rows` f64, held to the end in `StageRun`);
  - `research_ic_fitness.cpp:237-256` (the functor's own strided panels, beside the driver's per-generation rung panels).
- What is wrong:
  - Nothing bounds the slots of a mined program. Stage 2 runs subtree crossover with no depth cap (parsimony is an objective, not a bound), and no compiled-slot check runs before `evaluate`.
  - One slot on the 4-year role (about 1,405 x 6,100 cells) is about 69 MB per engine. A 12-slot program at 4 workers adds about 1.1 GB to the full engines alone.
  - Trial retention is about 5.9 KB per evaluated trial on a 3-year discover window:
    - about 60 MB at the 10,000-trial scale that mined-v1 quotes;
    - about 6 GB at the configuration bounds (4,096 x 256);
    - against the flat 64 MiB.
  - The strided panels are held twice.
  - So the estimate is borrowed, not derived. One measurement on one campaign does not bound another configuration.
- Blocks: OD-7 admission on the 15.7 GB host (E-6).
- Fix:
  - Refuse (file as failed) a program whose `num_slots` exceeds the admitted count.
  - Add trials x `label_rows` x 8 and the functor's panels to the estimate, or keep only (canon, f2, sign) per trial and recompute the daily IC for the shortlist.
- Verified by reading. The sizes are arithmetic, not measured.

### MINE-11 (m) The mask and the op catalogue are outside the checkpoint identity on the legacy path

- Where: `search_driver.cpp:72-116` (the checkpoint refusal applies only with a functor, `:105`), `:238`, `:650` (`serialize_cache` binds only the IC and CPCV identity).
- What is wrong: a mask-only or `op_catalog` run with a sink or resume can resume a cache scored under another mask or catalogue. No caller does this today.
- Fix: refuse a sink / resume with a non-empty mask or a non-default `op_catalog`, or bind both into the cache identity.
- Verified by reading.

### MINE-12 (m) A rung mask failure fails open without a flag

- Where: `search_driver.cpp:2285-2288`.
- What is wrong: an `apply_mask` error returns no rejection, sets no `res` flag, and the full pass runs. It is unreachable today because `run` validates the mask, but it is a silent fall-back.
- Fix: set `signal_path_invalid`.
- Verified.

### MINE-13 (m) Trial identity omits the VM, and the ledger line binds neither the recipe nor the rule

- Where:
  - `strategy_mine.cpp:369-383`: no `vm_identity()` / `dsl_vm_semantics_version` (`strategy_ic_signal_cache.cpp:30, 89`). `research_ic_fitness.cpp` and `strategy_mine*.cpp` are in no source pin, and the `ic` / `marginal` recipe entries are prose;
  - ccb66a87 dropped `rule` from the line;
  - `research_ledger.py:219-233` never opens the registry or `registry_head.txt`.
- What is wrong:
  - After a semantics change, a reopened registry dedups trials that now score differently and keeps the old records.
  - The windows, role and pool in `campaign.json` can change after the fact without changing the line, which binds only id, path, head, n_raw and window id.
- Fix:
  - Add `vm_identity()` and a source pin to the recipe.
  - Put `recipe_sha256` and `rule` in the line.
  - In `ledger-campaign`, check `registry_head.txt` and the registry tail against the head.
- Verified by reading.

### MINE-14 (m) The shortlist cap is applied before the rho step

- Where: `strategy_mine_rule.cpp:19-29`; `strategy_mine_promote.cpp:97`.
- What is wrong:
  - The cap (default 16) fills with variants of the strongest field before decorrelation: rank(p), five delta(p, w) at about .71 of its t, and ts_mean(p, 5).
  - Distinct signals ranked lower can be dropped. This only loses promotions, and BY's m shrinks.
- Fix: run the greedy rho step over the whole above-hurdle list, then cap.
- Verified by reading.

### MINE-15 (m) An undefined rho pair does not block

- Where: `strategy_mine_rule.cpp:47`.
- What is wrong:
  - Take a candidate with no date of at least `min_names` joint names with a member (for example, a member defined on a sub-universe). It is never checked, although the rule says "to every member".
  - This is the report's flagged interpretation.
- Fix: count an undefined pair as failing, or require joint dates >= `min_dates`.
- Verified.

### MINE-16 (m) A VM failure at a racing rung is filed as racing-rejected

- Where: `search_driver.cpp:164-181` (a compile or evaluate error becomes NaN); `strategy_mine_trials.cpp:47-49` (filed as racing-rejected, fidelity 1).
- What is wrong: the fixture's `failed == 0` (`strategy_mine_test.cpp:403`) cannot see VM errors on rungs. N still counts these trials.
- Fix: give rung failures their own status.
- Verified.

### MINE-17 (m) Canonically equal expressions in different surface forms are two trials

- Where: `strategy_mine_trials.cpp:180-181` (the config hash includes the DSL text beside the canonical hash).
- What is wrong:
  - Across seeds or campaigns on one registry, the surface form seen first decides the identity. This over-counts N (conservatively).
  - "A re-run adds 0" holds only when the surface form is identical.
- Fix: hash (recipe, canonical hash) only and keep the DSL as metadata.
- Verified.

### MINE-18 (m) The window and seal refusals are tested in one case each

- Where: `strategy_mine_test.cpp:493-516` (a confirm end past TRAIN; a sealed role refused by the manifest `score_end` check, `strategy_research_role.cpp:54`).
- What is wrong:
  - Untested: overlapping windows; confirm before discover; discover before 2020-01-01; rows outside the score window and the 25-row floor (`strategy_mine.cpp:171`); the role reader's session-level seal check.
  - The code refuses all of them by reading (`strategy_mine.cpp:149-153`; `strategy_data.cpp:109, 139-141`).
  - Also, `check_config` accepts `--min-dates` 2..7, which the IC `validate` refuses later (`ic_screen.cpp:156`). That refusal is loud.
- Fix: one table-driven refusal test.
- Verified.

## Summary

| id | sev | file:line | defect |
|---|---|---|---|
| MINE-1 | M | `strategy_mine.cpp:417`, `backtest_integrity.py:763` | 16-hex head; `ledger-campaign` refuses every real campaign; both tests fabricate around it |
| MINE-2 | M | `strategy_mine_promote.cpp:117-121`, `strategy_mine.cpp:171` | confirm t has no date floor (3 label rows suffice) |
| MINE-3 | M | `strategy_mine.cpp:369-383` | confirm window unbound; a re-run re-confirms with no record and no ledger line |
| MINE-4 | M | `strategy_mine.hpp:37-57`, `strategy_mine.cpp:402` | no budget fixed in advance (rule 10); a fresh registry resets N |
| MINE-5 | M | `strategy_mine.cpp:496`, `research_ledger.py:226` | E-33a: count = cumulative n_raw; tests cannot tell |
| MINE-6 | M | `research_ic_fitness.cpp:166-169` | Bonferroni hurdle on Bartlett-21 t, about 1.2x inflated on overlapping labels |
| MINE-7 | M | `strategy_mine.cpp:92` | no pool: the marginal t becomes the raw IC t; rho checks no member |
| MINE-8 | M | `strategy_research_role.cpp:105-162` | delisting-returns role not refused (B-3, F-7) |
| MINE-9 | M | `strategy_mine_test.cpp:232-249, 384-451` | fixture cannot fail on a wrong rule (copy, E-32, sign, N) |
| MINE-10 | M | `strategy_mine.cpp:258-278` | 8 slots borrowed; slots and trial retention unbounded |
| MINE-11 | m | `search_driver.cpp:105, 650` | mask and catalogue outside the checkpoint identity (legacy path) |
| MINE-12 | m | `search_driver.cpp:2287` | rung mask failure fails open, no flag |
| MINE-13 | m | `strategy_mine.cpp:369-383` | no VM identity in the recipe; line lacks recipe and rule; registry unchecked |
| MINE-14 | m | `strategy_mine_rule.cpp:19-29` | cap before the rho step |
| MINE-15 | m | `strategy_mine_rule.cpp:47` | undefined rho pair passes |
| MINE-16 | m | `search_driver.cpp:164-181` | rung VM error filed as racing-rejected |
| MINE-17 | m | `strategy_mine_trials.cpp:180-181` | DSL text in the trial identity |
| MINE-18 | m | `strategy_mine_test.cpp:493-516` | one refusal case each for windows and seal |

Totals: I 0, M 10, m 8.

## Coverage

| file (at d22b735a) | lines read | result |
|---|---|---|
| `atx-engine/include/atx/engine/factory/signal_fitness.hpp` | 1-78 (all) | clean |
| `.../factory/search_driver.hpp` | diff (+31), `catalog_` grep | clean |
| `.../factory/search_state.hpp` | diff | clean |
| `atx-engine/src/factory/search_driver.cpp` | diff (all hunks); 1-480, 840-900, 940-1480, 1830-1900, 2280-2340 | MINE-11, MINE-12, MINE-16; default-off identity clean |
| `.../factory/op_catalog.hpp`, `op_catalog.cpp` | diff; cpp 1-110 (all) | clean |
| `.../factory/research_ic_fitness.hpp`, `.cpp` | 1-196, 1-324 (all) | MINE-2, MINE-6, MINE-10 |
| `atx-engine/include/atx/engine/combine/marginal_rank_ic.hpp`, `src/combine/marginal_rank_ic.cpp` | 1-110; 200-300 | context (MINE-2, MINE-6) |
| `atx-engine/include/atx/engine/eval/hac.hpp` | 60-240 | context (MINE-6) |
| `atx-engine/src/factory/ic_screen.cpp` | 139-200, 283-540 | clean (labels mature at window end) |
| `.../factory/crossover.hpp`, `mutation.hpp`, `alpha/vm.hpp` | 50-95; 100-125; 1145-1175 | context (MINE-10) |
| `.../eval/trial_registry.hpp` | grep: 24-111, 206-354 | context (MINE-1) |
| `atx-engine/src/data/strategy_data.cpp` | 95-175 | clean (seal before payload) |
| `atx-engine/tests/factory/factory_signal_fitness_test.cpp` | 1-589 (all) | golden pins clean; MINE-9 (f2 tests) |
| `atx-impl/src/strategy_mine.hpp`, `.cpp` | 1-76, 1-618 (all) | MINE-1..5, 7, 10, 13, 18 |
| `atx-impl/src/strategy_mine_detail.hpp` | 1-121 (all) | clean |
| `atx-impl/src/strategy_mine_trials.cpp` | 1-236 (all) | MINE-16, MINE-17 |
| `atx-impl/src/strategy_mine_promote.cpp` | 1-175 (all) | MINE-2, MINE-7, MINE-14 |
| `atx-impl/src/strategy_mine_rule.hpp`, `.cpp` | 1-77, 1-72 (all) | MINE-14, MINE-15; arithmetic clean (hurdle, BY) |
| `atx-impl/src/strategy_mine_pool.hpp`, `.cpp` | 1-63, 1-120 (all) | MINE-7; role binding clean |
| `atx-impl/src/strategy_research_role.hpp`, `.cpp` | 1-94, 1-164 (all) | MINE-8 |
| `atx-impl/tools/equity_strategy_mine.cpp` | all | clean |
| `atx-impl/tests/strategy_mine_test.cpp` | 1-517 (all) | MINE-1, 5, 9, 18 |
| `atx-impl/CMakeLists.txt`, `atx-impl/tests/CMakeLists.txt`, `atx-engine/CMakeLists.txt` | diff | clean |
| `scripts/research_ledger.py`, `scripts/research_cycle.py` | diff | MINE-1, MINE-5, MINE-13 |
| `scripts/tests/test_research_ledger.py` | diff (+57) | MINE-1, MINE-5 |
| `atx-impl/tools/backtest_integrity.py` | 728-800, grep for campaign | MINE-1 (probe) |
| `atx-impl/src/strategy_ic_signal_cache.cpp` | grep 25-134 | context (MINE-13) |
| sprint docs | task-H-brief; task-H-3-report; integration-log "integration 5 part B"; progress.md (E-32, E-33; E-33a at a7673e0a); v8-prereg.md 1-30; review-w1-T (T-1); review-w1-B (B-3..B-8); review-w1-fixes (F-7) | - |

Not read:
- `data/role_panel.{hpp,cpp}` and `factory/fidelity.hpp` (part 1, merged at integration 4);
- the implementations of `race()`, `subtree_crossover`, `TrialRegistry` and `literature_ops()` (headers only);
- `.agents/cpp/agent.md`.
