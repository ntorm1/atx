# Task L8 report: library v7.1 (v7.0 + wave 2) and spec v71 on fields-v9, no runs
**Commits:** pool-10, branch feat/platform-v7-l8-libv71-20260929 from base 8792ec44: `5513bea9` (work) + this report. No C++ built; no IC / NAV / fit run; no validation / 2023+ statistic and no v7.0 per-candidate result read; nothing written to pool-2 (pool-2 files were only hashed or read for metadata). An overlay of junctions into pool-2 was refused by the permission check, so the pins and the dry-run plan used an in-process read-only path map (scratchpad l8_dry.py) instead.
**Library v7.1** (`atx-impl/strategies/generate_fund_ic_v71.py`): fund_industry_ic_v71.json `787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259`, recipe `7f8a264312d47487d2a9f7e0e92ecd76defb22ce2822ec319805f9ad0dffb271`, generator sha256 (LF) `e6343b12...`. Parent v7.0 re-derived and pinned (e7bae75c / 60b82300).
- 48 = the 44 v7.0 entries (byte-identical prefix, including non-admitted members) + ins_opp `rank(ins_opportunistic_net)` (ownership_flow, B-), inst_best_ideas `rank(decay_linear(inst_best_ideas, 21))` (ownership_flow, C+), ftd_fail `rank((-1 * ftd_shares_ratio21))` (short_interest, C+), ea_overdue `rank((-1 * max(sign((-1 * ea_days_to_expected)), 0)))` (earnings_momentum, C+).
- DSLs are asserted equal to the backticked text of v7-prereg.md "Library v7.1" and the P4 table at e3880d83. The draft blocks at f43e54d9 differ only by the two registered respellings, which are recorded in `literature.respellings_v71`.
- Families 9 -> 10 (ownership_flow declared in v7.1). Only the short_interest and earnings_momentum descriptions change. Four field declarations are appended (referenced extras 36 -> 40).
- Static figures match P4 section 2 exactly: extras 1/1/1/1, slots 2/3/3/4, nodes 2/4/4/8, prior bars 0/20/0/0, bytes 27/39/31/53. House budget, no exception, no registry row.
- recipe `trials.dsr_n` = 35, and its rule text says: ledger lines at run time + 1.
- Pending for root (Q6): the JEF 2016 FTD authors and the "t n/v" values. The ftd_fail citation sits in the library, so completing it means editing the generator, rerunning it, running `lock --relock`, and gives a new library sha.
**Checker:** check_fund_ic_v6.py needed no rule change (only a v7.1 usage line was added). Run on the real pool-2 fields-v9 manifest (63 rows, 8fd00e9f) it gives `check: ok`, 44-entry prefix identical, added 4, roster 48 <= 56.
**research_cycle.py** (generic, documented in the module doc):
1. `fields.manifest_sha256`: an as-built fields dir. It is never rebuilt or suffixed. A wrong sha stops with exit 3; a missing dir is a hard stop; `list` must equal the manifest rows.
2. `ref` phase: the nav section's construction (rule, L and flags) on a pinned input combined signal and this cycle's fields.
3. `compare`: identity steps run after a named phase ("<phase>-compare"), with modes file / csv-rows / json-rows. `{input:K}` / `{out:P}` name the operands, and `keys` takes the member ids from a library. A miss is a hard stop (exit 4), no trial.
   - A probe of the real v6.1 -> v7.0 u-pass layouts (counts only) found that train_daily_ic.csv carries a `__combined__` series, which moves with any library change. The compare is therefore keyed on the parent library's ids.
   - Probe result: 39 members identical in orientations.json and 84,864 rows in train_daily_ic.csv.
4. `summ.cells_from_ledger`: the grid is every trials.jsonl cell in ledger order, minus this cycle's cell. `summ.dsr_n` is the one key: the plan stops with exit 3 and prints "set summ.dsr_n to N" when the two disagree.
**Spec `scripts/specs/v71.json`** (inputs locked from the files; spec sha c4a2ff8b...):
- fields = lo1-fields-v9, pinned 8fd00e9f. The check expects 41 fields-v7 payloads byte-identical, the 22 W5a/W5b fields added, and the short-volume pin.
- ref -> `mega-nav-v70u-ref-fields-v9`, using the v7.0 combined 7c2961c6. The test asserts its argv equals the v7.0 cell receipt's, with only --fields, --fields-sha256 and --output changed.
- ref-compare checks the S2 daily CSV (`daily_modeled-1bn-stale5-v1+swap-fin-v1.csv`, 9c23f072) bit for bit (W2-e / iv).
- u runs on `mega-candidate-cache-v71` with v70's flags (incl. --cache-legacy-fields v7). u-compare checks the 44 parent rows of the orientations (232ad416) and train_daily_ic (9453c1da) (iii).
- Then fit (ew-theme-v1 / v4-prior-v1), card, gate p1-v71 (the 4 rows, require any), w, and nav -> `mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` (aim-partial-v5, L 1.247, --fields v9), monitor.
- summ: reference = the v7.0 cell, grid from the ledger, --dsr-n 35, --effective-n dirs --psr --pbo, ledger construction.
- (iv) runs before u. (iii) can only be checked on the u outputs, so it runs right after u, before fit.
**Tests:**
- test_generate_fund_ic_v71.py: 13 passed. They cover determinism, pins, prefix identity, the prereg / P4 / draft text, the P4 budgets, numpy transcriptions of the 4 members on the synthetic panel, causality, fitter labels plus ew-theme 9 -> 10, and the checker on the fields-v9 rows and on the real manifest.
- test_research_cycle.py: 66 passed, 3 skipped (live root). The new tests cover the v71 spec diff vs v70, the hash-only plan, ledger dsr_n, ref + compare run/resume/stops, compare modes and keys, as-built fields, and validation.
- Suite `scripts/tests atx-impl/tools atx-impl/strategies`: 374 passed, 3 skipped.
**Dry-run plan against the real pool-2 files:** all 11 pins "locked, verified", fields done, every phase pending. The only placeholders are this cycle's 5 own outputs.
**NOTE:** the pool-2 ledger now has 35 lines, because U-lo3 was ledgered at db1fc790. So the committed plan stops with "set summ.dsr_n to 36". That is root's one-key edit (step 2 below). With 36 in memory, the plan resolves with 35 grid cells + this cell.
**Root commands** (pool-2 root, PY="C:/Program Files/Python312/python.exe"):
1. `git merge feat/platform-v7-l8-libv71-20260929`
2. `sed -i 's/"dsr_n": 35,/"dsr_n": 36,/' scripts/specs/v71.json && git commit -qm "spec v71: dsr_n 36 (U-lo3 ledgered)" scripts/specs/v71.json` (only if `plan` asks for it)
3. `cp -al build-equity/mega-candidate-cache-v70 build-equity/mega-candidate-cache-v71` (hard-link seed; optional check: the IC exe with the u line's library/train/fields pins, `--candidate-cache build-equity/mega-candidate-cache-v71 --cache-report` -> 44 hits, 4 misses)
4. `"$PY" scripts/research_cycle.py plan specs/v71.json`, then `"$PY" scripts/research_cycle.py run specs/v71.json` (clean tree)
5. JSON scoring rerun (adds 0 ledger lines): `eval "$("$PY" scripts/research_cycle.py plan specs/v71.json --lines-only | tail -1) --json build-equity/mega-nav-v71-summ-n36.json --pbo-json build-equity/mega-nav-v71-pbo-n36.json --ledger-n build-equity/trials.jsonl"`
**Expected u-pass memory:**
- Admission = v7.0's 1,553,061,946 B + 4 x 512 B = 1,553,063,994 B (1,481 MiB), under the 1,536 MiB cap. Max slots stay 8 (qmj_safety) and resident capacity stays 6.
- The manifest has 63 resident-capable rows, but only the 40 referenced extras are loadable, at most 6 resident at once. The 4 misses load one field each.
- Measured: v7.0 u peak 1,162 MiB (qmj at 8 slots). The largest v7.1 evaluation is 4 slots, so expect about 0.95-1.16 GiB. The w and NAV passes should be about v7.0's 505 / 346 MiB.
**Untested:**
- Native parse and VM of the 4 DSLs.
- Real cache hits.
- Whether the NAV on fields-v9 reproduces the S2 CSV (the ref phase's job).
- u-pass RSS.
- nav_summ over the real 36 dirs (argv only).
- The ftd_fail stale window's effect on IC days (P4: 740 of 756).
