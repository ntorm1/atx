# Root brief: wave-1 merges, builds, identity, P9-B0 (after R0-14)

Root = `C:/atx-wt/pool-2`, on `feat/platform-p9-20261003` (cut at R0-14). Plan §3.4 is the recipe; §4.1 / §4.2 /
§4.3 phase 1 are the gates. The ledger (`progress.md`, same dir) is authoritative; grep "Merge note" and "Task <ID>:".

## Lane heads (all review APPROVE; merge by SHA, `git merge --no-ff <sha>`, in this slot order)

| slot | lane | pool | head | report (in the lane's pool, same rel path) | notes |
|---|---|---|---|---|---|
| 1 | E1 tasks 1+ | 17 | fdd2bda3 | task-E1-report.md "Tasks 1+", "Fix round 1", "Fix round 2" | task 0 already merged (1b9b37e8); confirm every wave complete first (E1-STALE) |
| 2 | T1 | 19 | 0a61b706 | task-T1-report.md | see T1 below |
| 3 | A1 | 12 | f8edca96 | task-A1-report.md (+ Fix rounds 1-3) | before A2 |
| 4 | A2 | 13 | 2b6f8e6f | task-A2-report.md | 3-row flip after merge |
| 5 | S1 | 18 | 34ef92dd | task-S1-report.md (+ Fix round 1) | |
| 6 | B1 | 14 | 01f20405 | task-B1-report.md | |
| 7 | D1 | 20 | 757ba582 | task-D1-report.md | |
| 8 | C1 | 15 | 10c35df3 | task-C1-report.md (+ Fix round 1) | NAV re-pin, two-stage identity |

## Per-lane merge notes

- **T1:** first run `generate_from_spec.py --spec specs/library-v71.json --check`; it must print v71 `787c802e`,
  recipe.v2 `69e95298`, 19 artefacts, exit 0. If it does, merge the whole head 0a61b706 (includes class-C deletion
  dee23d2a + allowlist 10493041). If not, merge up to dd5c4f5e only (cherry-pick order: tasks 1-4 + dd5c4f5e; T1-HIST:
  historical specs keep naming the deleted script). Canary goldens are recorded after C1 merges, on equity-dev then
  equity-rel (`research-build.ps1 -Canary`; `--record` requires the re-pin flag).
- **A1 then A2:** A2's 2 gtests and `test_real_executable_identity` need A1's `expected/`. After A2 merges, flip
  `si_shares`, `si_dtc`, `vol_126` to `kind: engine` via A1's `regenerate()` round-trip (no hand edit of the generated
  JSON); `check` must pass. Root v15 `--registry` identity: 82 reused + nt_first_126, deal_pending recomputed, 84
  payloads byte-identical (A1 report "How root verifies"). A1 tools + fixtures suites in separate processes.
- **S1:** `ic_screen.{hpp,cpp}` cross-lane edit and un-bumped `ic_result_sources_sha256` e7a40331 -> 2d758bff accepted
  (S1-PIN). Release marginal needs a Release u pass first. Marginal with many candidates: `--workers <= 8`. Gtests
  `MarginalIc.*` in both Debug and Release trees; flag-absent Debug X-5 marginal byte-identity.
- **B1:** no adaptation to D1 needed (review-d1). `load_pinned_f64` is defined in S1's `strategy_ic_signal_cache.cpp`:
  build B1 targets after S1 (and again after D1). Admission comparator: decisions/order byte-equal, floats 1e-12 (P12).
- **D1 / S1 / B1:** all append the tail of `atx-impl/CMakeLists.txt` (and tests lists, P2): keep every block.
- **C1:** identity per its report §4: Debug (equity-dev) stage 1 = build at dd925b7f vs old X-5 NAV (only the
  sqrt-named files / JSON paths may differ); stage 2 = build at a275088b (code head) vs stage 1 (all CSVs, both
  recipes, capacity/summary.json, capacity_curve.csv, v7_transfer_coefficient.csv byte-identical; summary.json differs
  only at v7.extras, v7.files, producer; v7_extras.json only at files["capacity/summary.json"]); then Release vs that
  Debug output minus summary.producer. Exe identity in summary.producer is ruled (C1-PROD). The re-pin list is the
  DEC-20 substitution list: write it into the log before the run.

## Per merge (plan §3.4 step 2)

One target-scoped build per C++ lane under tag `p9-1<letter>` via `scripts/research-build.ps1` (never raw
cmake/ninja), exactly the targets the lane report names; then the named anchored gtest filters; then pytest on the
touched Python suites. 0 warnings under `/W4 /WX`. A slip (typo, missing include) is fixed in place and logged; a design
error stops the merge and returns to the PM. Never edit an expected hash.

## After the last merge (§3.4 steps 3-4, §4.2, §4.3 phase 1)

- Suites: `ctest -L atx_research`, `-L atx_equity_strategy` (count logged, G-P4), the IC / target / impl test exes,
  engine alpha / factory / book groups, `atx-engine-research-fields-tests`; pytest `scripts/tests` under
  `PYTHONHASHSEED` 0 and 1, `atx-engine/tools`, `atx-impl/tools`, `atx-impl/strategies`: 0 failed.
- Golden `0x889874a3b9b29c55` at 1 and 4 workers; tiny-world canary goldens Debug + Release (G-P8).
- Release IC adoption (G-P3, IC half): S1's u / w identity Debug vs Release per its report.
- P9-B0 (DEC-20): re-run the current parent's u / fit / w / NAV under the new build at 0 trials; write the expected
  substitution list (C1 NAV paths, A1 manifest keys) in the log before the run; compare by SHA and JSON path only (no
  statistic read); pin the re-based reference. A difference outside the list is a stop.
- `research_cycle.py scoreboard` reproduces the lineage; ledger chain verifies; `scoreboard --timings` appended.
- Tick every G-P row wave 1 makes checkable in the integration log.

Report to `root-wave1-merge-report.md` in this dir (per merge: SHA, build tag, targets, warnings, gtests, pytest;
identity tables; P9-B0 list and result; gates ticked). Stop and report on any design error or identity miss.
