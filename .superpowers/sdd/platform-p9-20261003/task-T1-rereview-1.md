# Lane T1 re-review 1 (fix round 1)

## Verdict
APPROVE. F1 is ADDRESSED, the fix diff introduces no new blocker or major, and every finding is addressed.

## Reviewed SHA
`0a61b706e0334dc4421467c7d74c7752a426eb51` on `feat/p9-t1-20261003` in pool-19. The previous review was at
`3fbc94e0`. The fix diff `3fbc94e0..0a61b706` has three commits:
- `dd5c4f5e`: the main fix, plus minors 2 (mirror half), 3 and 6;
- `10493041`: the versioned-guard subset rule;
- `0a61b706`: the report.

Pool-19 was at HEAD 0a61b706 and clean (`git status --short` empty) before and after every command. I built nothing,
ran no real exe and edited nothing.

## F1 (major): planted recovery not asserted. ADDRESSED
| Requirement | Evidence |
|---|---|
| Module constant cited to T1-SE | `scripts/tests/test_cycle_e2e.py:80` sets `PLANTED_BAND_SE = 2.0` (comment: "PM ruling T1-SE"). |
| Live test asserts \|z\| <= 2 for planted_a and planted_b | `:472-476`: `planted_block` (`:166-173`), then `planted_problems` (`:176-179`, filters on `within_band`), then `assert not outside`. This runs before the null-golden check at `:478-480`, so a run with unrecorded goldens still checks the band. |
| `record()` refuses on a band miss | `:514-520`: admission facts, the IC-reference tie and `planted_problems` are collected into `refusals`, and `SystemExit("nothing recorded ...")` is raised before `digests`/`GOLDENS.write_bytes` (`:521`, `:533`). |
| Offline seed-7 check | `:342-357` `test_reference_planted_members_sit_inside_the_band_at_seed_7`: the reference gives planted_a z -0.82 and planted_b z -1.14 (each within 0.005), both inside the band. It also proves that a moved z -2.5 is refused. These match the z table I reproduced independently in the previous review. |
| noise_c stays out of the band | The band iterates over `PLANTED` only (`:169`, `:352`). `:355` asserts noise_c z +2.23 and `not within_band`. |
| `planted_report` stores the band | `scripts/tests/fixtures/tiny_world_ic.py:204-208` returns `band_se` and `within_band = abs(z) <= band_se`. `within_one_se` is gone, and all three callers were updated (`git grep`: `test_cycle_e2e.py:172,337,350`). |
| Docstring and report cite T1-SE | The docstring is at `test_cycle_e2e.py:21-26,36-39`. The report's "Fix round 1" section (report :355-380) says Deviation 1 is resolved by T1-SE. |

## New breakage from the fix diff
None (0 blocker, 0 major). Checked:

- **`--repin` (`test_cycle_e2e.py:504-507,522-525,548-549`) does not allow a silent expected-hash edit.**
  - Without the flag, a non-null `builds[<type>]` is refused before any run. The test at `:360-371` asserts the
    goldens bytes are unchanged after the refusal.
  - With the flag, every refusal still applies (admission, IC tie, band), and `old -> new` is printed per digest key
    on stderr.
  - The result is a dirty `tiny_world_goldens.json` that root must commit, so the change is explicit and visible.
- **`research-build.ps1 -CanaryRecord` (unchanged by the fix; `:126-127`, `:185-186`) refuses loudly.**
  - It still passes `--record` without `--repin`, so on an already-recorded build type the Python `SystemExit` gives
    exit 1.
  - The message lands in `mega-<Tag>-canary.log`, `Canary.ExitCode` becomes non-zero, and the receipt still writes.
  - A first record on a null entry behaves as before. This is the refusal finding 3 asked for. The report
    (:391-392) states that a ruled re-pin runs `--record --repin` directly.
- **The guard subset rules pass on the current tree and match the base.** I parsed the allowlists from git objects
  with `ast` (scratchpad script, no repo writes):
  - Versioned: `FROZEN_ROWS` (30) equals `019239f6`'s ALLOWLIST keys exactly. HEAD's 11 rows equal `dee23d2a`'s and
    are a subset.
  - Mirror: `FROZEN_PAIRS` (39 pairs, 16 rows) equals `019239f6`'s pairs exactly, HEAD's pairs equal the base's, and
    there are no duplicates.
  - The `RULE_SYMBOLS` set is unchanged from the base, so the scan result on any tree (root, wave-1 heads) is
    identical to what the previous review verified.
  - Detection is pair-based (`allowed()`, `test_no_new_python_mirror`), so retiring one module's `deflated_sharpe`
    does not affect the other's.
- **Merge note holds.** The files of `dd5c4f5e`, `10493041` and `dee23d2a` are disjoint. `dee23d2a`'s only non-delete
  change is `test_no_versioned_scripts.py`, and `dd5c4f5e` does not touch it. So `dd5c4f5e` applies onto
  `019239f6`, and `10493041` follows `dee23d2a`.
- **eval_tie host identity (minor 6).**
  - `expected.json` gains only a `host` block (+6/-0). The input SHA pins are unchanged, and nothing outside the
    fixture pins `expected.json` (`git grep eval_tie` outside the fixture dir: no hits).
  - On this host, exact mode still applies and the regeneration test runs, rather than skipping.

## Evidence
1. The lane's covering command in pool-19:
   `PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   scripts/tests/test_cycle_e2e.py scripts/tests/test_no_versioned_scripts.py scripts/tests/test_no_python_mirror.py
   atx-engine/tests/fixtures/eval_tie/test_eval_tie_fixture.py` gave exit_code=0:
   ```
   ..............s.........
   G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes
   .........                                        [100%]
   32 passed, 1 skipped in 2.97s
   ```
2. The same command without eval_tie, run `-v -rs` with `PYTHONHASHSEED=1`, gave exit_code=0:
   `25 passed, 1 skipped in 2.55s`.
   - The new tests passed: `test_reference_planted_members_sit_inside_the_band_at_seed_7`,
     `test_record_refuses_to_overwrite_a_recorded_entry_without_repin` and
     `test_admission_problems_name_every_broken_fact`, plus both `..._only_shrinks_...` guards.
   - The skip is the live canary: `test_cycle_e2e.py:439: set ATX_EQUITY_BIN`.

## Out of scope (not findings)
- Report :245 (original root-steps text) still says "A ruled re-pin is `-CanaryRecord` plus a ruling line", and
  report :238 says "Expect `12 passed`". Both are superseded by the Fix round 1 section (:391-392). Following the
  stale line gives a loud refusal, not a wrong pin.
- The `research-build.ps1:26-27` help text does not mention that `-CanaryRecord` now refuses on an already-recorded
  build type.
- `--repin` takes no ruling id argument, so the ruling's existence rests on root's commit discipline (the §0.6 process).
