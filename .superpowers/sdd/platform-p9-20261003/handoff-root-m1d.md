# Handoff: root-m1d (stopped by the owner, 2026-10-03)

Agent root-m1d, pool-2, branch `feat/platform-p9-20261003`. Dispatched from `ad406718` (wave-1 code head `b52a5de7`).
Stopped on the owner's request after block 1's gtest half. Head at stop: `ad406718` + this handoff commit (docs only;
no code commit, no merge, no expected hash touched). Scratch logs: session scratchpad `m1d/`.

## State of each block

| block | state | result |
|---|---|---|
| 1 suites | **partial**: build + every gtest suite done; pytest NOT done | see below |
| 2 golden + canary | **partial**: golden done (as part of block 1); canary goldens NOT recorded | golden holds |
| 3 Release IC adoption | not started | - |
| 4 P9-B0 | not started; **no substitution list written or committed** | - |
| 5 scoreboard / ledger / timings | not started | trial ledger still 133 lines, sha256 `27e40f9f6314585e` (checked at start) |
| 6 G-P ticks | not started (nothing ticked by M1d) | - |

### Block 1 detail

- **Debug tree reconfigured** first (`atx-build.ps1 configure -Preset equity-dev`, 37 s, exit 0): `engine_git_sha`
  now `ad406718...-dirty` (was `1239a5ff...-dirty`). Release tree `build-equity-rel` untouched (configured at
  `61ac4423`, code = `b52a5de7`).
- **Build p9-1k** (equity-dev, source `ad406718`, DirtyEntries 2 = PM's `progress.md` lines + owner png, 4 jobs, free
  4,229 MiB): exit 0, 193.3 s, 36 TUs, 14 links, **0 warnings / 0 errors** in the UTF-16 log. Targets: ic, targets,
  ic-tests, target-tests, impl-tests, strategy-mine-tests, strategy-tests, engine alpha / factory / book tests,
  research-fields-tests, research-admission-tests, research-fields, research-admission. Exe SHA-256 (prefix): ic
  `393183c0`, targets `407c34ad`, ic-tests `13d7b053`, target-tests `d08a739a`, impl-tests `dd58e3ad`, mine-tests
  `336e1b67`, strategy-tests `e1b27211`, alpha-tests `723508b1`, factory-tests `d2c1284d`, book-tests `1436bf72`,
  fields-tests `1b364bf8`, admission-tests `c2b35ce7` (unchanged), fields exe `8db24467` (new configure-time git sha),
  admission exe `416966bc` (unchanged). Receipt `build-equity/mega-p9-1k-receipt.json`.
- **CTest (Debug, `-Jobs 2`):** `-L atx_research` **104/106**, the 2 failures = exactly M1a-RED's gtests
  `ResearchFieldsWriter.QuantilesPartitionLikeNumpy` and `ResearchFieldsVolumeMean.SumOrderIsNumpys`, assertion text
  identical to M1a's record (writer_test.cpp:139 x5 `same_bits(x, 0.0)`; volume_mean_test.cpp:120/133/135 got 5e15,
  1.25e15, 1.25e15 for 1/3, 0, 1/9); 65.6 s. `-L atx_equity_strategy` **577/577 passed** (strategy-tests 46, ic-tests
  193, target-tests 338), 219.8 s. G-P4 counts: atx_research 106 (fields 46 + admission 16 + mine 44),
  atx_equity_strategy 577.
- **Engine groups (whole binaries, Debug):** alpha 771/771 (107.8 s), factory 392/392 (161.2 s), book 184/184
  (16.6 s).
- **Not run:** `atx-impl-tests` whole binary (built in p9-1k, ready); every pytest suite.
- **pytest: killed mid-run, nothing usable.** The chain (scripts/tests seed 0, seed 1, engine/tools, impl/tools,
  impl/strategies) was at ~60 % of `scripts/tests` seed 0 when the stop came; I killed it (my processes only: the sh
  chain and its pytest + one pytest child). Its partial log shows one `F` at ~57 % -- name not printed (-q, killed
  before the summary); most likely the known red `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry`
  but **unverified**. Re-run all five pytest runs from scratch (explicit paths, PY-HYG).

### Block 2 detail

- **Golden `0x889874a3b9b29c55`: holds at 1 and 4 workers** on the p9-1k exes: mine-tests
  `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest` and `...ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount`
  (workers 1 and 4) passed in the atx_research ctest; factory-tests `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
  `NsgaSearch.ScalarRaw_DigestInvariantAcrossWorkers` (1, 2, 4, 8) and `NsgaSearch.DeflateSelection_DefaultIsOff_ByteIdentical`
  passed in the factory binary. Nothing further needed.
- **Canary goldens: not recorded.** `scripts/tests/fixtures/tiny_world_goldens.json` still has `builds.Debug = null`,
  `builds.Release = null`. Note for the next agent: `test_cycle_e2e.py --record` needs `--repin` only when the entry is
  already recorded (`record()` lines 504-507); both are null, so the first record needs no `--repin` and is a first
  record, not an edit. Plan: `research-build.ps1 -Tag p9-1l -Targets "atx-equity-strategy-ic,atx-equity-strategy-targets"
  -CanaryRecord` (equity-dev; a no-op build on the p9-1k tree), commit the goldens file, then `-Canary` on the next tag;
  then the same on equity-rel (that tag builds the Release IC exe first: expect a large compile).

## Mid-flight at the stop

- pytest chain: killed (above); outputs unusable.
- Nothing else running: no atx exe, ninja, clang or ctest process of mine is alive (checked).

## Reusable

- Debug `build-equity/bin` = p9-1k (all suites' exes current at `b52a5de7` code, configure `ad406718`). No rebuild is
  needed for blocks 1-2 Debug or the Debug side of P9-B0.
- Release `build-equity-rel/bin`: only C1's three targets (p9-1j); the IC exe / ic-tests / alpha-tests are not built
  there yet.
- Logs: `m1d/ctest-research.log`, `m1d/ctest-equity.log`, `m1d/gt-engine-{alpha,factory,book}.log`,
  `m1d/gt-fields-reds.log`, `m1d/build-p9-1k.out`.

## Next free build tag: **p9-1l**

## Binding items written before a run

None. No P9-B0 substitution list was written or committed (block 4 not reached).

## Notes for whoever runs block 3 / 4 (not binding; my reading, unverified by any run)

- **Current parent for P9-B0:** Y-F0 = `scripts/specs/v8/lib-v8ysb-gm.json` (last accepted unlevered cell; plan §5.2
  "parent of every cell = the last accepted"); brief-COV step 4 also needs **Y-1's NAV re-based at P9-B0**
  (`y-vol-target-y-1.json`, NAV-only on X-10 on Y-F0). References (all SHAs = the spec pins): u
  `mega-v8-b0b-train-u-v8ysb-1` (receipt `...-run1`, warm shared cache, exe `985019d9`), fit
  `mega-weights-v8x-theme-erc-v8ysb` (`121f7046` / `0c480773`), w `mega-v8xw-train-theme-erc-v8ysb-2` (receipt `-run2`,
  combined json `e13fbc4d`), NAV Y-F0 `mega-nav-v8x-theme-erc-L1.1828-v8ysb` (summary `2d30b7f6`), X-10
  `mega-nav-v8x-theme-erc-L2.0-v8ysb` (`1bc1b6ca`), Y-1 `mega-nav-v8y-vol-target-L2.0` (summary `876d107b`, 28 files incl.
  `vol_target.csv`). Re-run each from its receipt argv with only `--output` changed (scratch `rerun.py` /
  `m1c/navrun.py` pattern); u / w on the spec's shared cache are warm (~0.5 / ~1.4 GiB peak).
- **Y-1 is a vol-target book; the C1 lists were measured on X-5 (no leverage rule).** From the code: capacity books keep
  no leverage record (`strategy_nav_replay.cpp:1042-1043`), the capacity summary gets no `/vol_target/books`
  (`strategy_nav_v7.cpp:591`), records merge in (session, book) order (`merged`, v7:498-522). So the only extra paths I
  expect sqrt can move are the S2-family per-book leverage records: `vol_target.csv` rows of books [1]-[4],
  `summary.json` / `v7_extras.json` `/vol_target/books/<L1..L4>`, `v7_extras.json` `/files/vol_target.csv`. These are
  NOT on the PM-ruled C1 list verbatim; the next agent should either get them ruled before the run or treat a hit as a
  stop.
- Fit precedent (PM7-30 / D1): `admission.csv` identical; `admission.json` only `/inputs/script_sha256`;
  `composition_weights.json` only `/provenance/{admission_sha256, script_sha256, std/registry_sha256}`. u / w: payload
  files identical; `summary.json` / `train_candidates.jsonl` only `*seconds` leaves (both warm on the same cache).
- Release adoption (S1 report fix-round-1 "How root verifies" 4): Release `marginal` on a DIR with only Debug entries
  must refuse NotFound; after a Release u pass fills `DIR/dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/`, it must succeed.
  A Release cold u pass computes every candidate: Debug cold w peaked 2.4 GiB; host free memory was 2.1-4.2 GiB.
- Host: free memory 2.1-4.2 GiB during my work; ten lane pytest runs and another session's jobs alongside.

## Open problems

- M1a-RED (3) and M1c-RED (2) known reds stand; block 1 saw only the two M1a-RED gtests in the suites it finished.
- pytest gate (scripts/tests x2 seeds, engine/tools, impl/tools, impl/strategies) and `atx-impl-tests` whole binary
  still owed before block 1 can pass.
