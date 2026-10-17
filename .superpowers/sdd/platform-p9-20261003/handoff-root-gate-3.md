# Handoff: root GATE, chain agent 3 (GATE-1, RESUME 13), BLOCKED-OWNER at GATE-IDX, 2026-10-16

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Task file `r13-prompts/gate-prompt-3.md` (binding; order
GATE-3-ORDER, ledger :982). Report (the record of every step): `root-gate-report.md` "Status by step after chain agent
3", the round-2b-ii table under "Round-2b-ii facts", and "Chain agent 3 (RESUME 13 ...)" at the end. Evidence and tools:
`gate-logs/` (`gate-logs/tools/`). Row: `integration-log.md` "GATE (RESUME 13), chain agent 3". Previous links:
`handoff-root-gate-2.md`, `handoff-root-gate-1.md` (the exe table of record).

- **Status: BLOCKED-OWNER** (GATE-IDX). `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` still present at
  14:04Z (start), 14:21:49Z (the check after every cache-free item) and at the hand-back re-check (`root-gate-report.md`
  "GATE-IDX check (agent 3)"); never touched by this agent. Every cache-free item of GATE-3-ORDER is done and committed.
- **Code head: `06dc9e5a`** (unchanged; no code commit). **Docs head:** the records commit that adds this file
  (`git log -1 --format=%h -- <this file>`); below it this agent's `778d34b8` (the substitution list, before any P9-B0
  run) and `e5a2fe68` (the FIELDS-SUB witness).
- **Build tags used: none.** **Next free root tag: `p9-3h`.** Exes of record unchanged (agent 1's p9-3f / p9-3g): 27 / 27
  + 27 / 27 `disk = receipt` at start; the Release NAV exe = receipt before and after every run. EXE-FREEZE not started.
- **Trial ledger** 133 lines `27e40f9f` at start and hand-back (0 trials). `build-equity/research-store/` absent. No
  statistic read or recorded (rho as its value and flag only, B2-13).
- **Known-red set: EMPTY.** **Permission refusals: none.** GATE-MYPY: the 133 staged `gate-logs/tools/.mypy_cache/**`
  paths stay staged (owner's); every commit of this agent named explicit pathspecs plus the exclude; `git show --stat`
  of each: 0 `mypy_cache`.

## Done (evidence `gate-logs/` unless named)

| step | item | result | evidence |
|---|---|---|---|
| 0 | re-verification | holds | `exe-sha-{rel,dbg}-3.txt` |
| 4 | FIELDS-SUB (1) witness (`tools/fields_witness.py`) | **70 / 70** (v11 witness of v10, v15 `files`, on-disk SHA-256 of all 70); the NAV's 2 read fields equal | `fields-witness.txt` (`e5a2fe68`) |
| 4 | substitution list | committed `778d34b8` before any P9-B0 run | report "Step 4, agent 3" |
| 4 | R-2 NAV on v15 (`tools/b0_nav.py r2`) | completed; vs the v8 NAV of record: 13 / 27 identical; differs only at FIELDS (4 leaves), DERIVED digests (14), M1d-C1 `/v7/extras` (1), C1-COST cells (9 files, cost columns, 2-8 rows), plus the ALLOWED leaves; **PATH B** by the ruling's letter (R-2 NOT re-based; G-B6 / OD-3 / G-B8 lines as FIELDS-SUB words them) | `r2-cmp-raw.txt`, `r2-classify.txt`, `tools/r2_classify.py` |
| 4 | X-10 NAV (`b0_nav.py x10`) | **holds** (26 / 27; producer + `ann_mean_gross` only) | `x10-cmp.txt` |
| 4 | Y-1 NAV on `p9-m2i-dcov-v11` (`b0_nav.py y1`) | completed after COV-9 (S9 holds) and the store's pin re-check; differs from `p9-b0-y1-nav` and v8's Y-1 at 26 / 28 files (PRE-29) | `y1-cmp-{b0,v8}.txt`, `cov9-role-check-3.txt`, `v11-store-check-3.txt` |
| 4 | `ucrtbase.dll` at the P9-B0 runs | 10.0.26100.7623 `2C08E77F...65EF` (= CRT-2) | `ucrt-b0-3.txt` |
| 5 | rho Y-1 vs X-10 (`tools/rho_pairs.py`, bounded) | 0.9947113980219978; "1 - rho < 1e-6" false | `rho-pairs-1.txt` |
| 3, 7 | X-5 identity rows; X-5 opt-in cache identity | **CUT** (FIELDS-X5), recorded "not run at the gate: fields v13 absent (FIELDS-GONE)"; M6b items 5 / 6 cited | report |
| - | GATE-IDX check | file present: BLOCKED-OWNER | `cache-lo3-listing-agent3-idx.txt` |
| - | prepared, not run | `tools/b0_ic.py` (rows 4-5: Y-F0 u / w on the Release IC exe with GATE-IDX + GATE-RELCACHE pre / post checks); its `--dry` refuses while the file exists (tested); its listing helpers reproduce `155872de50833fca` (351) and `fb444f7d316a1a4c` (352) | - |

## Not done

- Step 4 rows 4-6: Y-F0 u / w (name the cache: GATE-IDX) and Y-F0 NAV (GATE-3-ORDER puts it after the check).
- Step 5: rho of the P9-X pair (re-based Y-1 vs re-based Y-F0) and X-10 vs Y-F0 (need row 6).
- Step 6: the re-pin (one commit, pins only; `exes.eval` + every exe's SHA-256; COVMV-PIN: `mv-aim-v1.json` NOT edited;
  W1-I2's registration SHA `7801c198...` of `info-clock-rate-v1.json`); EXE-FREEZE starts there.
- Step 7: catalog as a bounded root run to a root run dir (never `build-equity/research-store/`), `verify --pins`
  counts, render identity, a second from-scratch catalog reproducing the digest, the fixture digest of record
  `a50fdcf9...100c`, the `classes.json` SHA by hand (STORE-S S-4), G-P ticks with evidence rows, G-P10 (d) PARTIAL /
  (e) landed.
- Step 9: the final records (`handoff-root-gate.md`), the round-2b-ii items still "pending" in the report's table.

## Exact next step (after the owner deletes the file)

1. Step 0 re-verify (`python gate-logs/tools/exe_sha_end.py build-equity build-equity-rel` and `... build-equity
   build-equity`; ledger 133 / `27e40f9f`; research-store absent).
2. GATE-IDX + GATE-RELCACHE: `python gate-logs/tools/b0_ic.py yf0u --dry` (cwd pool-2, interpreter
   `C:/Program Files/Python312/python.exe`, `PYTHONDONTWRITEBYTECODE=1`): must print index absent, the 351 originals
   `155872de50833fca`, C: >= 15 GB, exe = receipt.
3. **Step 4 row 4:** `python gate-logs/tools/b0_ic.py yf0u` (cold Release pass, ~7 GB into the cache dir; the tool
   records the after-listing with its cause); compare `MSYS_NO_PATHCONV=1 python gate-logs/tools/cmp.py
   build-equity/p9-b0-yf0-u build-equity/p9-b0g-yf0-u --allow <regex> ...` with only row 4's classes (timing leaves,
   Release identity strings / cache-root paths, cache counters, `/roles/*/research_fields/{field_loads,loaded_bytes,
   peak_resident_fields}`), every allowed class listed; anything else = an identity miss (ROOT-1).
4. Row 5: `b0_ic.py yf0w`, the same comparison form. Row 6: `python gate-logs/tools/b0_nav.py yf0` and `cmp.py
   build-equity/p9-b0-yf0-nav build-equity/p9-b0g-yf0-nav --allow '^/producer/(build_type|engine_git_sha)$' --allow
   '^/scenarios/\d+/ann_mean_gross$' --allow '^/scenarios/\d+/info_clock/'` (opt-in side only; `tools/clock_optin_cmp.py`
   form). Re-record `ucrtbase.dll`.
5. Step 5: `run_bounded_research.py --output build-equity/p9-b0g-rho-2-run --seconds 600 --max-rss-mib 2048 -- <python>
   gate-logs/tools/rho_pairs.py build-equity-rel/bin/atx-research-eval.exe "P9-X pair (Y-1 vs Y-F0)=build-equity/p9-b0g-y1-nav,build-equity/p9-b0g-yf0-nav"
   "X-10 vs Y-F0=build-equity/p9-b0g-x10-nav,build-equity/p9-b0g-yf0-nav"`; then steps 6, 7, 9 as "Not done".

## Exe table

Unchanged: `handoff-root-gate-1.md` "Exe table" (Release p9-3f in `build-equity-rel/bin`, Debug p9-3g in
`build-equity/bin`; receipts `build-equity/mega-p9-3{f,g}-receipt.json`; source `06dc9e5a`, receipt Source `5f48a25d`).
Used by this agent: Release NAV `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565`, Release eval
`e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c`. Re-hash before use (SPDLOG-RELINK).

## Reference dirs so far (P9-B0 run dirs; never deleted)

| dir | role | files | listing SHA-256 (`gate-logs/b0-files-<dir>.txt`) | `summary.json` SHA-256 |
|---|---|---|---|---|
| `build-equity/p9-b0-r2-nav` | R-2 on v15 (PATH B: NOT the re-based R-2 unless the PM re-rules) | 27 | `04bb8060aadf692a63e7f1baafd23f297db67c24d9c1f0a43dce6678a755b1bd` | `48a52f6adb06c2e966cb366f2c71c9dbef55ba48de243afc2cce01df3085c1b9` |
| `build-equity/p9-b0g-x10-nav` | re-based X-10 | 27 | `6c87c8d516d7782c2484d6cc397323359f9f9b61cc242a5f894885240a74f31d` | `deabe71b5e2dedfb0b224fb46fdcd0e7194a6950ad8f5ecabe9601cd786bd0bd` |
| `build-equity/p9-b0g-y1-nav` | re-based Y-1 (v1.1 store) | 28 | `cac882e19a5baee9d3d60581d93a278f9f4fc6b769ed0e9154dd408b1552b813` | `b0d4dcbf53e81aff3863ca1d9c7230da73a5c0a911a60f622c9f78aa3796c028` |

Each with its `-run` dir (receipt, start, logs; receipts copied to `gate-logs/receipt-<dir>.json`). Also
`build-equity/p9-b0g-rho-1-run` (the rho run's receipt and stdout; 40 KB). Reserved for rows 4-6: `p9-b0g-yf0-{u,w,nav}`.

## Substitution list

`root-gate-report.md` "Step 4, agent 3: the P9-B0 substitution list" (commit `778d34b8`): 1 R-2 (fields v10 -> v15,
FIELDS-SUB); 2 X-10; 3 Y-1 (store -> `p9-m2i-dcov-v11`, PRE-29); 4-6 Y-F0 u / w / NAV; X-5 cut (FIELDS-X5); token 0 ->
the Release exe and `--output` -> the new dir everywhere; `--ann-mean-gross` on every NAV reference,
`--info-clock-print` on Y-F0's; `--seal-allow` on none (a wave-driver flag; no seal scan in a bounded hand run).

## Kept and deleted outputs

Kept: the three reference dirs above (+ `-run`), `p9-b0g-rho-1-run`; agent 2's `p9-gate-*` outputs as it left them.
Deleted: nothing. Disk: about 34 MB written; C: 66.6 GB free.

## Open questions for the PM (one line each)

1. FIELDS-SUB path B fired only on the fields-manifest leaves (written by the substitution), their derived digests and M1d's C1 class (the v8-era base predates P9's NAV change; no events, L0 or statistic leaf moved): rule path A (R-2 re-based = `p9-b0-r2-nav`) on `r2-classify.txt`?
2. Rows 4-5: GATE-RELCACHE's allowed list names only the counters; the Release u / w `summary.json` also carries the Release VM identity strings and cache-root paths (M1d's Release IC adoption list): allowed as substitution-list row 4 states?
3. PRE-34's printed-precision agreement of the re-based Y-1 is a figure comparison (B.7: the cells root, before the adoption print), not run at the gate: confirm.
4. GATE-3-ORDER keeps Y-F0's NAV (names no cache) behind GATE-IDX: keep, or may the next agent run it first?
5. GATE-IDX `index.sqlite` and the GATE-MYPY staged cache: still the owner's.
