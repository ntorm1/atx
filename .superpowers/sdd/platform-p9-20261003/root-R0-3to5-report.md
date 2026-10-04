# Root R0-3..R0-5 report (P9 Phase 0; 2026-10-03)

Root `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `84f06f6b`. Commits: `d23efa5a` (R0-3 result, R0-4
checks, P8 and R0-5 plans, written before the runs), `fd962cb9` (P8 and R0-5 results). Log section:
`integration-log.md` "P9 Phase 0, root steps R0-3, R0-4, R0-5". No build, no push; E1's files and `y-s.json` untouched.

| step | verdict | evidence |
|---|---|---|
| R0-3 X-5 identity, v8-16d | PASS | runs existed (source `d7c1c520`, exes = v8-16d receipt: ic `985019d9`, targets `72ff6d2d`); argv = X-5's but `--output` (w also the empty cache dir). NAV 27/27 byte-identical (S2 `529062d6` = ledgered); w 10/12 (all 6 `train_combined.*`, daily IC, orientations, recipe, planned targets), the other 2 differ only in timing, cache and cold-cache I/O counter paths; fit `admission.csv` identical, both JSONs identical after the PM7-30 substitutions (script `8d05a9bb`, admission, registry `6a1ef89d`) |
| R0-4 fields v15 | PASS | manifest `26fee5ce...3b09` = `y-s.json` pin; v15a 78 reused / 5 computed, v15 83 / 1; reused entries sha-equal and hardlinked (78/78, 83/83); 84/84 payloads re-hash; seal 2024-01-01, role lo3 to 2023-12-29; caps held (294.5 s / 710 MiB, 209.8 s / 799 MiB) |
| R0-4 P8 | PASS | new run `p9-r04-x5-ref-v15-run` (56.4 s / 586 MiB): S2 daily `529062d6...3d61` = X-5's ledgered SHA; 23/27 identical, the 4 others differ only in the fields-manifest pin and the recipe hash that follows it |
| R0-5 IC memory | PASS | scratch Y-S library (73 = 58 + 15, built in memory by `generate_library.py`, SHA `bbcfbf9d...`; not of record) `--plan-only`, workers 4, `--no-composition`: **required_bytes 2,047,374,058 = 1,952.5 MiB <= 2,560**: cap unchanged, no ruling. The cell's w pass (73 members, 12 themes): 2,786.0 MiB <= 3,072 [arith; the composition formula matches X-5's w plan-only to the byte] |

Notes for the PM (no ruling needed):
- The NAV pass under v8-16d took 145.7 s of its 180 s cap in the identity run (45-59 s before; my P8 NAV run took 56.4 s
  on a quieter host). That points to host load, but watch NAV time in R0-6..R0-11.
- The admission of a `--no-composition` u pass does not depend on the member count (cells x slots, field capacity,
  labels, workers). The old ~3,200 MiB linear estimate was wrong in kind, not only by ~35%.
- Roster cap is still 80 (the 96 edit was refused in PM session 8). Y-S holds 73, so the cap is not binding.
- Untracked `docs/plans/2026-10-02-x5-equity-curve.png` (not root's) sits outside the code pathspec. Receipts list it
  under `dirty_outside_pathspec`. Left alone.
- Blindness: comparisons by SHA-256 and JSON paths only. No NAV stdout was opened and nothing dated 2024 or later was
  opened. While looking for the ref precedent I passed X-7's already-ledgered public lines in the log.

Scratch scripts (session scratchpad): `r03_identity.py`, `r03_cache_counters.py`, `r03_argv.py`, `r04_fields.py`,
`r04_p8.py`, `r05_lib.py` (rebuilds the scratch library from committed inputs).
