# P9 RESUME 6: addendum to the standing contract (PM, 2026-10-04)

Read `resume4-dispatch.md` first. This file adds to it and wins where the two differ. Then grep your lane ID in the
ledger `progress.md`; the blocks "RESUME 5" and "RESUME 6" hold the newest rulings.

## Owner rules restated for this session

- If the permission system refuses a command: do NOT retry it and do NOT route around it with another command that
  has the same effect. Put the refused command in your final message; the PM reports it to the owner.
- Never run `atx-impl/tools/holdout_gate.py` (it reads 2024+). Open nothing dated 2024-01-01 or later.
- Configure only as `powershell scripts\atx-build.ps1 configure -Preset <preset>`. Build only through
  `scripts/research-build.ps1`, target-scoped, one build on the host at a time.
- Never build, switch or commit in `C:/atx`. Never touch `atx-db/`. No push. Never `git worktree prune`, never
  `git worktree remove`, never stale-lease recovery. Never touch `main` or `origin/main`.
- Never edit an expected hash, golden or pinned value. The trial ledger `C:/atx-wt/pool-2/build-equity/trials.jsonl`
  stays 133 lines, sha256 prefix `27e40f9f`, until a registered P9 cell appends to it.
- Another live session works in `C:/atx-wt/pool-26` (statarb) and may build or test: its processes are not yours.

## New rulings every agent needs

- **ENV-1 (process-start failures).** A test run in which a child process exits 3221225794 (0xC0000142,
  "DLL init failed": Windows could not start the process under memory pressure) is void as evidence: neither a pass
  nor a red. Wait for the host to quieten (bounded, about 30 minutes in all), re-run that suite once, report both
  runs. A failure that repeats without that exit code, or the code repeating on a quiet host, is a red to attribute.
- **Known-red set of root at `59893e9f`:** `scripts/tests/test_research_spec.py::test_e27b_reads_what_the_fitter_and_nav_summ_parse`
  and `::test_the_whole_file_passes_with_a_generated_spec_present` (both cleared by D2R's merge). Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` is a named skip, not a red.
- **Blind agents** (lanes, reviewers, doc agents): read root's history only as `git log --format=%h` or one-line
  subjects of your own lane; never commit bodies of root commits (they can carry measured numbers).
- **No questions to the PM mid-run.** A slip is fixed in place and logged. A real stop condition (contract, last
  bullet of "Hard rules") means: write a handoff file, report BLOCKED.
- **Bounded real-data runs** (root only): `--seconds` <= 600, `--max-rss-mib 2048` (RUN-1, RUN-2).
- **Commit trailer**, exactly: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
