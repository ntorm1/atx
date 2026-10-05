# Handoff: root after M2h (E2 merge; finished by a fresh root after the owner stop), 2026-10-04

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Ruling M2H-1: nothing re-merged, no history rewritten.
Rows: `integration-log.md` "M2h"; evidence `m2h-logs/` (the stopped agent's `e2-*` / `root1-*`; mine `root1b-*` /
`m2h2-*`); my pre-run list `m2h-logs/m2h2-prerun.md` (written before the runs, amended before attempt 2).

- **Code head: `59893e9f`** = `2fbef84e` (housekeeping) + `430c29e8` (merge of E2 `dd857198`) + `1dd0ac46` (E2-18
  slip) + `59893e9f` (SQL2-CLS: `atx.research-wave/v2` in the wave-manifest class). No code commit in this session.
- **Docs head:** the commit that adds this file (on `59893e9f`).
- **Status: DONE_WITH_CONCERNS.** Every check below ran and holds; the concerns are the documented substitutions of
  the real-exe tiny-world check (section "Substitutions") and the open questions. Nothing of mine is running; nothing
  half-merged.
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials; the
  tiny-world waves used their own empty `trials.jsonl` inside each world root and stopped at verify).
- **Next free build tag: `p9-2m`** (no build in M2h by either agent: no `mega-p9-2m` receipt exists).

## Results (S = the stopped agent's log, taken as evidence because `root1-head.txt` = `59893e9f`, 0 tracked changes; M = run by me)

| item | result | src |
|---|---|---|
| start state | head `59893e9f`; tracked changes only `progress.md`, `next-goal-prompt.md`; ledger 133 / `27e40f9f` | M |
| ROOT-1 `scripts/tests` seed 0 | **510 pass / 4 skip / 2 fail**, exactly the two e27b known reds; 0 exit 3221225794; 630 s (`root1b-run1-*`) | M |
| ROOT-1 `scripts/tests` seed 1 | 510 / 4 / 2 fail (the same two) (`root1-scripts-s1.log`) | S |
| void run (ENV-1) | the stopped agent's seed 0: 339 / 4 / 173 fail, 152 `git init -q` + 9 HARD-STOP at exit 3221225794: void, replaced by my run above (which showed no such exit, so no second re-run was needed) | S |
| former E2 red | `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` PASSED by name (seed 0) | M |
| stage chain | 9/9 under seeds 0 and 1 (`root1-chain-s*.log`) | S |
| `test_research_store_identity.py` | 16 pass under seeds 0 and 1 | M |
| SQL2-CLS | classes guard 1 fail before (the v2 literal) / 5 pass after; store suite 43 pass / 135 sub, seeds 0 and 1; fixture chain `d32f7655...4e69`, identity 12 checked 0 mismatches, rel = abs; scratch catalog classifies both v2 kind manifests as wave-manifest | S |
| live-table check (re-review 4 item 1) | RESULT PASS on both trees: live IC listing = committed `composition_rules_list.json`; `unmapped_ic_rules == []`; MR-4 ids derive `--theme-hedge` / `--mom-volman`; fixed-v1 range [1, 2]; fixtures reach spec / preflight on the live tables | S |
| (a) MR-4 + C2 S13 | the lane's merge-of-root `d2cea6a6` vs git's auto-merge tree `0f746a16`: 5 added / 2 removed in `wave_kinds.py` (MR-4) and `wave_stage_cell.py` (S13); lane head `dd857198` = HEAD on both files; lane vs HEAD over `scripts`, `atx-engine/tools`, `atx-impl/tools` differs only by E2-18 (string unchanged) and root's AL-DATA spec amendments `30b9c9d2`; C2 R4 pin present (`wave_stage_record.py:315`) (`m2h2-mr4-s13.txt`) | M |
| (b) engine match on real exes | wave `eng0` (kind `rule`, `pm6-6-engine`): G_parent 0.986692308059862; the run recipe `-cal` NAV ran `--aim-leverage 1.12 --calibrate-gross 0.986692308059862 --calibrate-tolerance 0.005`; the exe's `summary.json` calibration is complete: `leverage_requested` 1.12 = the cell's L and `target` = G_parent (the `:374` check), `gross_requested` 1.0419426343712663, one correction to **L' 1.0606**, `gross` 0.9877841486160911, `matched` true; mechanics reader G = exe G bit for bit; cell of record `-gm`: plain recipe, `nav.leverage` 1.0606, no `--calibrate-*`, planned NAV argv `--aim-leverage 1.0606`; verify PASS (mechanics in band) and `07-verify.json` pins `match.record_spec_digest` = the match receipt's `ae9b138c...` | M |
| (b) leverage check, negative | wave `pin` (fixed-v1 `aim_leverage` 1.5, `pm6-6-engine`): stops at match, **exit 3**: "the calibration ran at L 1.5 toward 0.986692308059862, not the cell's L 1.12" | M |
| (c) O1 / E2-16 | template with its own whole `ref` block (no leverage, no flags), same rule wave: **`pm6-6`: `ref.leverage` = 1.0606** (L'; `gm_doc` pins nothing, the ref follows `nav.leverage`); on real exes the `-gm` matched run then refuses at the ref phase, **exit 3** (the ref NAV `tiny-ref-m2h` was made at 1.12, argv sha differs). **`pm6-6-engine`: `ref.leverage` = 1.12** (the cell's own L: `-cal` and `-gm` pin it; the ref NAV ran with `--aim-leverage 1.12`; the `-gm` plan shows ref done at 1.12, NAV at 1.0606). So the engine route already keeps the reference's own leverage; only the Python route differs (E2-16's branch: C3b adds the test) | M |
| (d) W1-I1 margins | 65 committed specs with an IC pass and their library on disk (7 skipped: the 5 kinds templates on the tiny-world nominal parent, `tiny.json`, `v7u-lo3` whose library is not on disk): worst `v70-lo3` 249 (margin 1); 60 more at 248 (margin 2, id `seasonality_same_month`, incl. AL-COMB's `p9/templates/mom-volman.json`, `theme-cov-hedge.json`), 4 with larger margins; none refused at pool-2; canary `%TEMP%` root shape 242 (margin 8) / 248 audit-exact (margin 2); my tiny-world roots margin >= 11 (`m2h2-w1i1-margin.log`) | M |
| Release live canary | `ATX_EQUITY_BIN=build-equity-rel/bin` `ATX_EQUITY_BUILD_TYPE=Release` `test_cycle_e2e.py`: 15 pass (goldens unchanged) | M |
| re-review 4 items not run | C3's real trade-rate rows and which `--rule` wins (C3 not merged); O3 is covered by `test_match_flags_inherited_from_the_parent_are_refused` (passed in the suite) and ruled by PRE-27; re-review 2 items 3, 7, 8, 9 are sequencing notes (no wave runs in the root tree; B2 redo, PM7-35 and W1-I2 (b) wait for their steps) | - |

## Substitutions in the real-exe tiny-world check (pre-run list amendment; each logged)

1. **Repository-shaped world.** In git mode the bounded runner runs in ITS worktree (`run_bounded_research.py`
   `--root` defaults to the script's `parents[1]`), so a plain `git init` world root fails the parent's u pass (attempt
   1: IC exe "metadata file missing"). Each world root carries a `git archive` of HEAD's `scripts`, `atx-engine/tools`,
   `atx-impl/tools`, `atx-impl/strategies`, committed there; every script ran from that byte copy.
2. **Parent `summ` block and L.** The committed `scripts/specs/tiny.json` has no `summ` (preflight compares the
   parent's `summ.ledger` with the wave's) and at L 1.247 its gross 1.0972511252536685 fails verify's mechanics band
   [0.9, 1.05] (attempt 1). The world's own copy got `summ.ledger trials.jsonl` and `nav.leverage` 1.12.
3. **The gross change.** No IC rule moves the tiny world's weights (`within-theme-v1` is a no-op with one admitted
   member per theme; all others need a standardised composition, refused by the member cap) and the live NAV table's
   rules set leverage or need a risk store. The named rule stays `within-theme-v1`; the gross moves through a FREE NAV
   constant `--trade-fraction .10` (a trading-rate change, as P9-C's trade-rate row would make).
4. **Seal tokens.** Every wave's verify tripped on two tokens `2026-10-04`, each the mechanics reader's own
   `"started_utc"` stamp in its console; after a per-hit check (printed in the wave logs) the wave resumed with
   `--seal-allow 2026-10-04=SEAL-ALLOW` (rule (b) of the standing SEAL-ALLOW ruling, applied by analogy: its named
   scope is R0-6..R0-11).

## Trees (unchanged since M2g; no build)

- `build-equity-rel` (Release): IC `atx-equity-strategy-ic` p9-2j `fccaa07e` (source `c2416d62`), NAV
  `atx-equity-strategy-targets` p9-2l `53ca5dd4` (source `3fd0bf98`), risk p9-2l `a31a0f96`, eval p9-2j `a57a2090`.
  `git diff <source>..HEAD` over C++ / CMake files: empty for each. These are the exes the tiny-world runs used.
- `build-equity` (Debug): as after M2g (risk p9-2k `94b5a5e7`, IC p9-2i `6c2e2e2e`, eval p9-2i `bd8b5e08`). Its NAV exe
  `atx-equity-strategy-targets` p9-2d `90cb34a7` was built at `a25453be`, before later C++ changes (`c2416d62` and
  others); not used here.

## Known-red set (unchanged)

- pytest `scripts/tests/test_research_spec.py::test_e27b_reads_what_the_fitter_and_nav_summ_parse` and
  `::test_the_whole_file_passes_with_a_generated_spec_present` (cleared by D2R's merge).
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`: a named skip, not a red.
- E2's former red `test_fields_are_the_rule_applied_to_the_registry` is green and leaves the set.

## Kept outputs / deleted

- Kept: nothing new. The five tiny-world roots `build-equity/p9-m2h-tw{,-eng0,-eng,-py,-pin}` (about 128 MB,
  regenerable by `m2h-logs/m2h2-engine-o1.py base` / `wave <label>`; every value is in `m2h2-report.log`) and my pytest
  basetemps / probe scratch are deleted by name after the docs commit. M2g's kept outputs are untouched.

## Uncommitted state at the end

- None of mine. `progress.md` committed as it stood (LEDGER-1). Four untracked `docs/plans/*.png`: not mine, left.
- The stopped agent's scratch (`.../6ea15be9-.../scratchpad/m2h`: pytest basetemps `bt-s0`, `bt-s1`, `bt-c0`,
  `bt-c1`, catalogs, `root1.sh`): not mine, left.

## Open questions for the PM (one line each)

1. Seal scan: the mechanics readers print `started_utc` into consoles, so every v2 wave's verify stops on the run's date; extend SEAL-ALLOW (b) to P9 waves, or have the reader keep the stamp out of the console?
2. The kinds fixtures say "tiny-world wave layout", but the committed `tiny.json` has no `summ` block and fails verify's gross band at L 1.247: a real-exe kinds canary needs both changed (D3 / P10 note)?
3. E2-16: the engine route already keeps `ref.leverage` at the cell's own L and only Python `pm6-6` follows L' (and then hard-stops), so C3b only adds the test: confirm.
4. P9-C's real NAV-rule shape and C3's `--rule <id>` vs `nav_step`'s `--rule nav.rule` question stay unprobed until C3 merges: re-check at C3's merge?
5. The Debug NAV exe (p9-2d, built at `a25453be`) predates later C++ changes: rebuild it (`p9-2m`) before any Debug real-exe check?
6. The stopped agent's scratch dir under its own session id: delete it, or keep it?
