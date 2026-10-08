# Handoff AL-CLOCK (lane AL-CLOCK, pool-8, branch `feat/p9-alclock-20261004`)

Resume R8 (`r8-dispatch.md` "AL-CLOCK"). Contract: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
`resume7-addendum.md`, `resume8-addendum.md`; rulings ALCLOCK-1 / -2 / -3, W3-9, W3-16, C3-5 / C3-13, PRE-7a, DEC-16.
Report: `task-AL-CLOCK-report.md` (same dir). The design text of the earlier handoffs is in git history
(`git show ebaf1e3f:.superpowers/sdd/platform-p9-20261003/handoff-AL-CLOCK.md`, and at `120f6a25`).

Continued in pool-30 on `feat/p9-alclock2-20261007` (TEETH-POOL); pool-8 frozen for the owner. The section "State at
the stop" below is pool-8's, as it stood at STOP 8 (copied by plain `cp`; only the HEAD placeholder filled).

## State at the stop: BLOCKED by a permission refusal (PERM-1)

- HEAD `1cbe5233` holds all lane code, tests, registration and template (committed); root `fd084e78` merged
  (`e40ed78c`, clean).
- The WORKING TREE IS NOT CLEAN: six source files carry the teeth set 1 mutations (uncommitted):
  `atx-impl/src/book/nav_cli.cpp`, `nav_decide.cpp`, `nav_load.cpp`, `nav_lockstep.cpp`, `nav_validate.cpp`,
  `atx-impl/src/strategy_nav_v7.cpp`. The exact diff is `alclock-logs/teeth-set1.diff` (108 lines; made by
  `alclock-logs/tools/teeth.py 1`). The teeth build ran and every mutation was caught (report "Teeth").
- The revert was REFUSED by the permission system, as one command:
  `cd C:/atx-wt/pool-8 && git checkout -- atx-impl/src/book/nav_validate.cpp atx-impl/src/book/nav_lockstep.cpp atx-impl/src/book/nav_load.cpp atx-impl/src/book/nav_decide.cpp atx-impl/src/book/nav_cli.cpp atx-impl/src/strategy_nav_v7.cpp && rtk proxy git status --short && "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/alclock-logs/tools/teeth.py 2 && rtk proxy git diff`
  Not retried; no same-effect substitute (git restore / stash / Edit-tool reversal) was used (PERM-1). Everything
  that needs the reverted tree stopped there.
- Build tree `build-equity` (equity-dev, configured, warm) is KEPT for the resume (a fresh agent saves ~25 minutes);
  `bin/atx-impl-strategy-target-tests.exe` is the TEETH exe of `p9-alclock-f` (mutated): never evidence. The canary
  exes in `bin/` (`atx-equity-strategy-targets`, `-ic`, `atx-research-eval`) are `p9-alclock-b`'s (source `c3f3f1f8`,
  clean; SHA-256 in `mega-p9-alclock-b-receipt.json`). `build-equity-rel` does not exist (Release not configured).

## Items

| item | status |
|---|---|
| merge root | done: `c3f3f1f8` (root `1b64ce45`), `e40ed78c` (root `fd084e78`), both clean |
| compile the WIP (16 C++ files) | done: clean under /W4 /WX at `p9-alclock-b` (no slip) |
| C++ test file + CMake tail | done: 13 tests; Debug `p9-alclock-e` (pre-merge tree): `InfoClock*` 13 / 13, named 24 / 24, whole target-tests 403 / 403 |
| `rule_flags` pytest | done: `scripts/tests/test_info_clock_rule.py` 5 / 5 |
| registration + template | done |
| teeth set 1 | done (`p9-alclock-f`): 7 tests fail as intended, 6 pass; NOT reverted (refused) |
| teeth set 2 (ALCLOCK-3 over-loosened) | NOT done (needs the reverted tree) |
| Debug canary | goldens match on `p9-alclock-b`'s exes; one first-run wall miss (16.0 s, limit 15 s; C3-14). R9: `p9-alclock-h` canary 15 / 15 |
| Debug build of the merged head, named + whole on it | NOT done (needs the reverted tree) |
| Release (configure, build, named, whole, canary) | NOT done (needs the reverted tree) |
| `scripts/tests` seeds 0 / 1, stage chain | stage chain 10 / 10 at seeds 0 and 1; `scripts/tests` seed 0 ran on the teeth-mutated tree with 81 ENV-1 hits (void). R9: deferred to the gate (TEST-1) |
| report | R8: committed with placeholders; R9: filled (pool-30) |

## Tools (lane-local, `alclock-logs/tools/`)

- `build.sh TAG PRESET TARGETS` (`RECACHE=1`, `CANARY=1`): waits for no other build on the host and the memory floor
  (90 minutes), then `scripts/research-build.ps1`; copies receipts / logs to `alclock-logs/`, one line in `builds.txt`.
- `gt.sh LABEL EXE LOG FILTER`, `pyt.sh LABEL SEED BASETEMP LOG -- args`: root's wrappers with lane paths.
- `teeth.py 1|2`: the teeth mutations (exact text replacements, CRLF-aware).

## Next step for a fresh agent (after the PM / owner restores the six files to HEAD in an accepted form)

1. Check `git status --short` is empty and `git diff HEAD` is empty (the six files equal HEAD's).
2. Teeth set 2: `teeth.py 2`, `bash alclock-logs/tools/build.sh p9-alclock-g equity-dev atx-impl-strategy-target-tests`,
   `gt.sh g-teeth <exe> <log> 'InfoClockRate.CapacityCurveAcceptsThisRateOnly'` (expect FAILED: per-name-v1 with the
   capacity curve and aim-partial-v6 with this rate both accepted), then restore the file in the accepted form. If
   that restore is not available, skip set 2 and say so (set 1's M10 already shows the refusal line has teeth).
3. Merge `feat/platform-p9-20261003` if root moved.
4. `CANARY=1 bash alclock-logs/tools/build.sh p9-alclock-h equity-dev "atx-impl-strategy-target-tests,atx-equity-strategy-targets,atx-equity-strategy-ic,atx-research-eval"`;
   `gt.sh` named filter `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*`
   and `'*'` on `build-equity/bin/atx-impl-strategy-target-tests.exe`.
5. `powershell scripts\atx-build.ps1 configure -Preset equity-rel` (the documented form, from pool-8); then
   `RECACHE=1 CANARY=1 bash alclock-logs/tools/build.sh p9-alclock-i equity-rel "<the four targets>"`; named + whole
   on `build-equity-rel/bin/atx-impl-strategy-target-tests.exe`.
6. If root moved after step 3: `scripts/tests` seeds 0 and 1 and `atx-engine/tools/test_stage_chain.py` again
   (separate invocations, `pyt.sh`).
7. Fill the report's `@@...@@` lines, commit report + handoff + `alclock-logs/` (`git add -f`), then delete
   `build-equity` and `build-equity-rel` object trees by name (keep `mega-*` receipts and logs, already copied to
   `alclock-logs/`).

## R9 (pool-30, `feat/p9-alclock2-20261007`; TEETH-POOL, TEST-1)

State: DONE_WITH_CONCERNS, working tree clean, handed back for the first review (BASE per REV-BASE:
`git merge-base <HEAD> feat/platform-p9-20261003`). No code changed in R9; lane code head `1cbe5233`.

| step | result |
|---|---|
| copy the pool-8 records, DIRTY-1 / TEETH-POOL commit | `3cc8a356` (handoff, report, `alclock-logs/` incl. `tools/`, `teeth-set1.diff`; tools repointed to pool-30) |
| configure `equity-dev` | done twice (before and after the merge, so the baked provenance is the merged head) |
| teeth set 2 | SKIPPED: `teeth.py` does not keep / restore the bytes itself (TEETH-RESTORE) |
| merge root `685e238b` | `adf1c6cb`, docs only, no conflict |
| Debug `p9-alclock-h` (`adf1c6cb`) | exit 0; canary 15 / 15 (built with `-Canary` and the four targets: started before TEST-1 arrived) |
| named filter on `p9-alclock-h` | 24 / 24 (`gt-h-named.log`) |
| `test_info_clock_rule.py` seeds 0 / 1 | 5 / 5, 5 / 5; stage chain 10 / 10 at both seeds (before TEST-1) |
| Debug whole, Release leg, whole `scripts/tests` | deferred to the gate (TEST-1) |
| object trees | pool-30 `build-equity` object tree deleted by name after the records commit (tracked `audits/`, `v8-interim3-pitch-render-run2/` kept) |

Next step for a fresh agent: none in the lane. Owed elsewhere: the gate runs the deferred legs (TEST-1); teeth set 2
only if the PM asks (needs a restore-capable `teeth.py` and one target-tests build). Pool-8 stays frozen for the owner.

## Fix round 1 (ALCLOCK-S), pool-30: STOPPED at STOP 9 (owner) before any edit

State: head `e9b3a323` (APPROVE head; code `1cbe5233`), working tree clean, NOTHING changed. No build was started
(tag `p9-alclock-k` unused; `p9-alclock-l` reserved for the S-3 teeth build); no test run; no mutation applied; no
object tree exists in `build-equity` (receipts / logs of `-h` and `-j` only). No permission refusal.

| item | status | design (worked out, not written) |
|---|---|---|
| S-3 (REQUIRED) | not done | new `TEST(InfoClockRate, CalibrateGrossKeepsTheRatesAndCountsOnce)` in `atx-impl/tests/strategy_nav_info_clock_test.cpp`, the pattern of `CalibrateGross.OneCorrectionMatchesTwoProcess` (`strategy_nav_calibrate_test.cpp:249`) on `World`: shape = the rule (`--rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage L --exit-rate .05 --rate info-clock-rate-v1`) + Y-F0's keys this fixture supports (`--order-basis delta --liquidity-cache --capacity-curve`) + `--info-clock-print`. Left out, stated in the comment: `--neutralize price-risk-v1` / `--locate-in-aim` (40 sessions < default min_return_pairs 126, `strategy_price_exposures.hpp:29`) and `--warm-start-sessions` (fixture role score_begin 0); the CalibrateGross x5 cases cover them. Runs: `first` at 1.2 -> g = numpy_mean of the primary daily `gross_leverage` (`summary.primary_scenario`); target = 1.1 g, tolerance = g / 1000; L' = `st::leverage_4_decimals(1.2 x target / g)`; `second` at four_decimals(L') with `--emit-holdings` csv; `calibrated` at 1.2 + `--calibrate-gross target --calibrate-tolerance tol` with `--emit-holdings` csv. Assert: every file but summary.json and the holdings dir byte-equal to `second`; summary minus `calibration` = second's; calibration corrected true, `construction_reused` TRUE (the tape-restore path, `nav_session.cpp:345-362`), leverage finite in [1, 2] and bit-equal L', gross_requested = g, gross = second's G, matched consistent; recipe `rate` = the id (main and `capacity/`); closed form on calibrated holdings (member rebalance rows with rule != held: rule = held + theta_i (L' desired - held), theta_i by the clock; fast and slow rows > 0); each main block's name_days / fast / missing = the members of the primary daily CSV's decision&rebalance rows (counted once: pass 1's books are dropped); capacity summary has no block. Optional: `--book-workers 4` calibrated (no holdings) = a 1-worker calibrated run (no holdings) byte for byte. Helpers in the anon namespace after `World`: `all_rows_gross(dir)`, `four_decimals(x)`, `text_of(x)` (precision 17), `rebalance_census(w, dir)`, `closed_form_rows(w, holdings, L)`; includes `<array> <charconv> <system_error>`, `"../src/strategy_nav_calibrate.hpp"`. Teeth (`p9-alclock-l`, TEETH-RESTORE tool): `nav_session.cpp` `if (clock_rule)` (before `nav_info_clock_rates`) -> `if (clock_rule && !(tape && tape->replay))`; expect ONLY the new test red |
| S-1 | not done | (a) help `strategy_nav_v7.cpp:1013` "fixed rate only" -> "fixed rate or --rate info-clock-rate-v1 (ALCLOCK-3: it reads no NAV)"; also the same stale phrase in `nav_cli.cpp:95` (`--book-workers`; the rule runs on a pool). (b) message `strategy_nav_v7.cpp:1344-1345`: string only, predicate untouched, e.g. "aim-partial-v6 needs the fixed rate, --capacity-curve the fixed rate or info-clock-rate-v1 (per-name-v1 reads the NAV)"; needles follow in the lane's own test (`:539`, `:580`); optionally `nav_lockstep.cpp:216` text (no test needles it; grep done). (c) capacity declaration: NOT pinned (checked: no golden / fixture / prereg / ledger quotes it; the tiny canary runs no `--capacity-curve`; only logs and the .cpp): correct it CONDITIONALLY, flag absent byte for byte: a second constant `capacity_info_clock_declaration` + selector (precedent `capacity_spo_v3_rule(adv_hold)`, `:236`); `State::info_clock` set in `configure()` (`:839`, `cfg.rate == NavRateRule::InfoClockV1`); `declarations(s, adv_hold, info_clock)` at `:581` / `:997` (state) and `:970` (the recipe's own `rate` key, set before `extend_recipe`, `nav_write.cpp:252`); `write_extras` `:465`. Test: the published texts under the rule name the rate; `FlagAbsentLoadsNoClock` already guards the flag-absent bytes |
| S-2 | not done | `scripts/specs/p9/rules/info-clock-rate-v1.json:51` `realised_leverage`: keep "C3-4" (pytest asserts it), add ALCLOCK-4, the bound verbatim "planned gross <= (theta_f / theta_s) x gross(aim) by contraction outside the dust band, 2.589x at cadence 1; P9-C's mean gross matched to its parent by pm6-6-engine", plus verbatim "realised gross leverage is read from the cell's own summary (max and mean over the cell) and printed beside the parent's" (summary `exposure.max_gross_leverage` / `mean_gross_leverage`); OD5-7 reads the realised figure, not the bound. New SHA re-quoted in the report's "For PRE" (old `51b8b754...`); add a pytest assert on the sentence |
| S-4 | not done | fixture `atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json` = this lane's row as `row_json` prints it (`nav_rules.cpp:105-125`: id, kind, x-flag, params_schema {type object, additionalProperties false, required [], properties {}}, incompatible, description = the `:75-82` text concatenated); `RegistryRowAndParser` asserts `*listed == fixture` (`ATX_IMPL_TESTS_DIR`, as `strategy_ic_rules_test.cpp:35`); the pytest's NAV_TABLE last row = the fixture (only this row: a whole-table fixture would break at COV-MV's row append) |
| S-5 | P10, record only | nothing to do in the lane |

Exact next step for a fresh agent: (1) `powershell scripts\atx-build.ps1 configure -Preset equity-dev`; (2) write S-1 / S-2 / S-4 text and the S-3 test as designed above, shaping with `powershell scripts\atx-build.ps1 -Preset equity-dev check <file>` per edited TU; (3) commit in slices (S-3 test; S-1 / S-2 / S-4 text); (4) ONE Debug build `p9-alclock-k` of `atx-impl-strategy-target-tests` through `scripts/research-build.ps1` (HOST-3 priority: fourth); `InfoClock*` named filter + the new test; `scripts/tests/test_info_clock_rule.py` (seeds 0 / 1, own basetemp); (5) teeth `p9-alclock-l` only if the host is quiet; (6) append "Fix round 1 (ALCLOCK-S)" to `task-AL-CLOCK-report.md`, a head line here, `git add -f`, commit; delete the object tree by name.
