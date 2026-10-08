# Handoff: root after M2 (RESUME 9: B2 post-identity redo and T2 merges), 2026-10-07

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m1.md`. Rows:
`integration-log.md` "M2 (RESUME 9)" (under the wave-2 heading); detail: `root-wave2-merge-report.md` "Step group M2
(RESUME 9)"; evidence `m2-logs/` (tools `m2-logs/tools/`).

- **Status: BLOCKED for two PM stops, both attributed to B2's merge `5dfb0ba1`. Every other item holds.** The merges
  stand (not reverted).
  1. PRE-15 public cross-check MISS: Y-F0's `tot_minus_m` n = 124 = N_tot 233 + 1 (Y-F0 not ledgered) - M 110; R0-12's
     logged DSR_hand is at N 123 (no + 1). The frozen oracle at N 123 reproduces R0-12's value to the printed 4 places,
     and the verb equals the oracle at 124 and at N_tot 234. The only difference is the + 1 convention, so this needs a
     definition ruling.
  2. Root-only `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced` FAILS on the merged head and
     passes on pre-merge `944581cc`. 0 numeric leaves differ beyond 1e-12. The verb's floats are within B2-R1, and one
     row carries B2-6's degenerate-pair keys. The test is byte-exact against a legacy Python golden: it needs a B2 fix
     round (B2-R1 (b) comparison plus the B2-6 disclosure) or a ruling. Root edited no test.
- **Code head: `fcb0200e`** = merge `5dfb0ba1` (B2 `46ffe3dc`, 0 conflicts) + merge `27e69387` (T2 `dfa4218d`, 0
  conflicts) + slip `6c201a1d` (C3B-6, `scripts/cycle/spec.py` text) + slip `fcb0200e` (B2-S S-1, three wraps).
- **Docs head:** the commit that adds this file. Earlier docs commit of this group: `944581cc` (step 0 housekeeping).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start, after every real-data
  run and at the end (0 trials).
- **Build tag used: `p9-2t`** (Debug). **Next free build tag: `p9-2u`.**
- **Known-red set:** suites EMPTY. One root-only red (above, item 2), which skips in every suite run without
  ATX_EQUITY_ROOT. Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip (not run).
- **SQL3-2:** `build-equity/research-store/catalog.sqlite` does NOT exist (the directory is absent); the store exe is
  present, so the hook is OFF by the catalog's absence.
- No lease touched. No permission refusal. No data dated 2024 or later opened; no statistic read (every comparison by
  diff: line numbers, paths, counts, booleans); `holdout_gate.py` not run.

## Exes (EXE-CUR)

New this group: Debug p9-2t, source `fcb0200e`, receipt `build-equity/mega-p9-2t-receipt.json` (copy
`m2-logs/mega-p9-2t-receipt.json`; SPDLOG-RELINK: this receipt is the evidence; all five re-hashed and equal):

| exe (Debug, `build-equity/bin`) | SHA-256 |
|---|---|
| atx-research-eval | `ad6c587861bcb7efe2091c4420304cbc735de2a8a7713d66221b9b3eb39c4bb9` |
| atx-engine-research-eval-tests | `1a81e3473b4c7b4691703531c47d73e810b2867c7e60eeef6440af48f56aa461` |
| atx-engine-research-ledger-tests | `8c5a6065f31f061fe6a06b474b0bd9987f45a640c5f59a9ade4b61b25fe322cd` (not relinked; no source change) |
| atx-impl-tests | `13bb6bde680e3e2c208c5860a52a19d3d3789ccd588a8d50eb501ed59bdb67fe` |
| atx-engine-research-fields-tests | `327fd4a4d7d25b73d1c4f271e92c914e2d189422fa2362e3b550ea8fd3d0de57` |

Unchanged and still current by source: only `atx-research-eval` and its tests link `atx-engine-research-eval`, and T2
and the slips are Python. Re-hashed this group and equal to `handoff-root-m1.md`: Debug NAV `637f7069...`, IC
`919569fb...`, store `d8a500d8...`, catalog tests `f1134aaa...`; Release NAV `adcf72d4...`, IC `1d24336f...`, store
`0d6083af...`, catalog tests `712907c9...`. The other p9-2p / p9-2q exes are as `handoff-root-m2j-fin.md` lists them.
**Stale by source after this merge (Release, deferred to the gate under TEST-1):** Release `atx-research-eval` p9-2q
`a57a2090...` and Release `atx-impl-tests` p9-2q.

## Results of this group

| item | result (evidence in `m2-logs/`) |
|---|---|
| 0 housekeeping | `944581cc` |
| B2 merge | `5dfb0ba1`, 0 conflicts; tree `7167e614` = `git merge-tree` recompute; code = lane's `46ffe3dc`; 35 code files |
| T2 merge | `27e69387`, 0 conflicts; tree `fcef6848` = recompute; 20 files = `bc618cb4`; A1-SHIM moot |
| C3B-6 | `6c201a1d`: `scripts/cycle/spec.py`, 3 hunks (+12 / -10), AST equal outside the 2 docstrings and 1 message |
| B2-S S-1 | `fcb0200e`: `wave_stage_record.py:303`, `test_wave_eval_verb.py:147, :155` (+5 / -3), tokens unchanged |
| SQL2-CLS | no new literal (B2: research-wave v1 / v2; T2: three registered); guard 5 passed |
| build | p9-2t equity-dev, 9 TUs, 7 links, 81 s, exit 0, 0 warning lines |
| gtests | eval 26 / 26; ledger 16 / 16; `atx-impl-tests` `*Ledger*:EvalVerb*` 29 / 29; fields `ResearchFields*` 92 / 92 |
| pytest B2 | `scripts/tests` 7 files 317p / 3 skip; `atx-impl/tools` 6 files + edges fixture 76p / 1 skip; legacy n37 root-only FAILED (pre-merge passed) |
| pytest T2 | 22 modules 248p; the other lanes' tools tests without the bind (7 files) 87p |
| identity | X-5 and Y-F0 **hold** (stdout equal except the one PRE-15 line; JSON 137 leaves bit-equal outside `tot_minus_m`); without `--dsr-total` byte-equal |
| PRE-15 cross-check | **MISS** (+ 1 convention; stop for the PM) |
| SQL3-2 | holds (no default catalog) |

## Kept and deleted outputs

- Kept: the Debug object tree `build-equity/` (C: 48 GB free; the next group reuses it) and `build-equity-rel/`.
  New run outputs of this group (small; identity evidence): `build-equity/p9-m2-b2-{x5,yf0}-run/`,
  `build-equity/p9-m2-b2-{x5,yf0}-nototal-run/` and `build-equity/p9-m2-b2-identity/` (X5 / YF0 JSON). The M2f
  references `build-equity/p9-m2f-b2-*` are untouched.
- Session scratchpad `root-m2/` (`ledger-copy/`, `pt/` basetemps, `pre-merge/` archive, my scripts): mine,
  regenerable; delete by literal name when no longer needed (never a wildcard in the shared scratchpad).

## What the next root group must know

- **The two PM stops above** decide whether B2 needs a fix round (the legacy n37 test; possibly the PRE-15 row). A B2
  fix merges as a second B2 merge at its own APPROVE SHA (MERGE-SHA).
- **Keep-both at the next merges** (B2 re-review 2 item 2):
  1. D3: `scripts/tests/test_wave_kinds.py:171` `F.pin_eval_verb(root)` in `world()` must survive D3's merge (re-add
     the one line if D3 rewrote `world()`).
  2. D3W: `eval_report.cpp` B2 hunks `:569-605`, `:722-737`, `:798-813` and D3W's `:603-628`, `:634-660`, `:683`,
     `:710-716` are adjacent but disjoint. B2-EVAL-KEEP: after D3W merges, run eval 26 and
     `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns`.
  3. C3b (APPROVE `b2517fe4`, review `76b9f256`, ledger): `wave_fixture.py` / `test_wave_kinds.py` auto-merge with B2's
     lines; then `test_wave_kinds.py` and `test_wave_eval_verb.py` must pass with the eval pin inherited through
     `match.cell_spec`. C3B-S: list the three evidence tools broken by design (`m2e-logs/tools/c2_gm_pair.py`,
     `m2h-logs/m2h2-engine-o1.py`, `m1-logs/tools/fakewave_f1.py`).
- **B2 SQL3 item 5** (three call lines in `cycle_resume.py`, `cycle_verdict.py`, `wave_stage_record.py`) is now
  unblocked: B2's redo is in root.
- **T2's post-merge gate** (whole `atx-engine/tools` under seeds 0 and 1) and the mixed `atx-engine/tools
  atx-impl/tools` run are the gate's (TEST-1). Since T2 retired the conftest bind, any later lane's tools test with
  2024-dated synthetic rows runs under the repository window and must be re-dated, never given the bind back.
- **Tools of this group:** `m2-logs/tools/b2_identity_{run.sh,cmp.py}` and `b2_identity_nototal.{sh,_cmp.py}` (blind
  X-5 / Y-F0 identity against the M2f Edbg outputs); `n37_diag.py` / `n37_diag_any.py <tools dir>` (blind legacy n37
  diff by path). Run pytest with `--tb=no --show-capture=no -rN` on any real-data test, so a failure prints no value.
- **Build wait:** one build on the host at a time; `m2-logs/build-wait.log` shows the waiter pattern (poll for no
  `ninja` / `cmake --build`, 60 s quiet, then `research-build.ps1`; bounded at 90 min).
- **Records:** the Bash heredoc fails on long texts with apostrophes (hook layer; hit again this group). Write long
  record files with the editor tool.
- Interpreter `C:/Program Files/Python312/python.exe` explicitly; create the `--basetemp` parent dir first.

## Uncommitted state at the end

- None of mine. Five untracked `docs/plans/*.png`: not mine, left.

## Open questions for the PM (one line each)

1. PRE-15: for a book not yet in the ledger, is N = N_tot - M counted with the book's own + 1 (verb: 124) or without it (R0-12: 123)?
2. Legacy n37 root-only test: B2 fix round (B2-R1 (b) comparison + the B2-6 disclosure), or retire it by a ruling?
3. `atx-engine-research-fields-tests` was named as T2's target, but T2 has no C++. I built it once as a no-change control (92 / 92); confirm no further T2 build is expected.
