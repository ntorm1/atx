# P9-L (`p9l-label-terminal`) cell record: ACCEPTED; DERIV agrees; ledgered

**Current status (2026-10-17, cells root agent 2): ACCEPTED, DERIV agrees, 6 / 6 mechanics rows pass; ledgered.**
Section 10 is the resumed run and committed mechanics decision, before any return / IC / Sharpe read.
Sections 1-9 below preserve agent 1's historical stopped-run record; their status statements describe run 1 only.
Section 12 records the PM release and completed ledger step; earlier checkpoint statements are historical.

Cells root agent 1 (`r14-prompts/cells-prompt-1.md`), pool-2, branch `feat/platform-p9-20261003`, 2026-10-16. Block:
`p9-prereg.md` B.1; plan of record `cells/p9l-label-terminal-plan.md` (committed `0950cb57` before any run).

**Status: STOPPED, no verdict.** The cycle's w pass (the weighted IC pass) was refused by the pinned Release IC exe
before any payload load: its memory admission under `--label-terminal imputed-v1` needs more than the w budget the
registration carries (`--max-memory-mib 3072`, the parent's `ic.w_flags`, E-28; the run caps of C.14, "carried alike",
B.1). The cycle stopped (`HARD-STOP [w]`, exit 4). No NAV ran, so no mechanics, no rho, no bundle, no printed-only line
and no trial-ledger line exist. Running on needs a change to the registered template (a w budget >= 3,289 MiB), which
is a re-registration: the PM's ruling (open question 1). Not a void (B.0 lists "a driver stop" among the non-voids);
not a verdict; nothing was retried or re-parameterised (E-45).

## 1. Registration

| item | commit | SHA-256 |
|---|---|---|
| template `scripts/specs/p9/templates/label-terminal.json` | `0bba6c89` | `7483f4b10513d9000cfc89036c3f8f7f49626a9dd9c57999bcce11a258754219` |
| run recipe `scripts/specs/p9/templates/label-terminal-cal.json` (not run) | `6ea79e5a` | `7783967471f5b06dc1407d4425605156f7a3e647f28edd2a3ca5f2fb1a989d37` |
| plan `cells/p9l-label-terminal-plan.md`, `p9-prereg.md` (Appendix IV item 7, SG4-REF, SG-3) | `0950cb57` | plan `720893f6c597693fe845540e4fba912f4a713671332e22bbdb68ff72e3f19ec0`; prereg `e42d5d96aa6a349d4dba6ce5be936f1f35d27d1ba27c25b49566f4ae946b305c` |
| cell of record `label-terminal-gm.json` | not written (no calibrated NAV) | - |

G_parent `0.9862134133265011` (`build-equity/p9l-label-terminal-readers/mech-parent.json`
`7bd04ca02e3ce6a17ead49d7a03ef3cc1e00f84f5338943f4dbee7f43e8d483e`; the parent's construction gross, read for the
calibration flag only). FLAGS-ROUTE: the template inherits `--ann-mean-gross` and `--info-clock-print` (each x1 on the
planned NAV argv; the NAV never ran).

## 2. Pre-cell checks (CELLS step 0; `cells/logs/p9l-precell.txt`, 2026-10-16T19:39:27Z, before run 1)

- `ucrtbase.dll` 10.0.26100.7623 `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` = the P9-B0 record.
- Exes: Release 27 / 27 and Debug 27 / 27 "disk = receipt" (p9-3f / p9-3g, source `5f48a25d`); the pinned eval / IC /
  NAV re-hashed = `exes_sha256` (`e2b48c46...d962c`, `ccd54db6...7d705`, `255b58ac...63565`). No build.
- Six P9-B0 reference listings = `gate-logs/b0-files-<dir>.txt` (r2, x10, y1, yf0-u, yf0-w, yf0-nav).
- `build-equity/research-store/` absent; `index.sqlite` absent; no cache-index flag on any planned or run argv (SQL-11).
- Timings off (no timing flag on any argv). `--seal-allow`: no object on a v1 hand cycle (no seal scan in
  `research_cycle.py run`; SEAL-REF ground), disclosed; the hand scan below in its place.
- S2-6 HOLDS before the run (`cells/logs/p9l-s2-6-scan.txt`: 2 scope dirs, 184 IC-result records, 0 `imputed-v1`).
- Trial ledger before: 133 lines, `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
- Host: free RAM 3.84 of 15.69 GB (3,931 MiB); C: 43.8 GB free. No bounded run in flight; foreign pools ignored
  (HOST-6a).
- Real cache before: 552 files, listing `c9ee9291f5ddc0c2` (`cells/logs/p9l-cache-listing-before.txt`).

## 3. Run 1 (the only run): `research_cycle.py run label-terminal.json --stop-after w`

Command (cwd `C:/atx-wt/pool-2`, HEAD `0950cb57`, code head `6ea79e5a`):
`"C:/Program Files/Python312/python.exe" scripts/research_cycle.py run scripts/specs/p9/templates/label-terminal.json --stop-after w --admission-wait 1800`
Console: `cells/logs/p9l-run1-cycle-filtered.txt` (the read filter `cells/tools/cycle_filter.py`: step states, the
runner argvs, receipts, ledger and gate PASS / FAIL lines; 30 of 41 lines). The raw console
`cells/logs/p9l-run1-cycle.log` stays on disk uncommitted: it holds the gate's per-candidate admission lines, not read
(B.0 void (3)).

| phase | outcome | receipt |
|---|---|---|
| fields | done (pinned as built, `train-2020-2023-lo3-fields-v15`, manifest `26fee5ce...3b09`; fields check 84 rows == the spec list) | - |
| u | completed, exit 0, 51.6 s, peak 933 MiB (caps 300 s / 2,560 MiB) | `build-equity/mega-p9l-train-u-label-terminal-run1/receipt.json`, argv_sha256 `a46dbe1647ddefc70da69f03cef5bfc3a3809f3e3cd6193e87ce5a85442485e2` |
| fit | completed, exit 0, 1.6 s, peak 67 MiB (180 s / 1,536 MiB) | `build-equity/mega-weights-p9l-label-terminal-run1/receipt.json`, `2dc9ad1ff7a109c3eadc62df221acf6d9ad3b0db285fb73926038a4e980d44fe` |
| card | completed, exit 0, 31.7 s, peak 1,405 MiB (300 s / 2,560 MiB) | `build-equity/mega-cards-p9l-label-terminal-run/receipt.json`, `bffd01bc78ca75f2ea72e4f31fa4f3549ef6c6696a58c8404e2196f9a682b4d3` |
| gate (internal) | "ledger build-equity/trials.jsonl: 0 admission trial line(s) appended, 9 already ledgered"; "gate p1-v8ysb PASS" | - |
| w | **process-error, exit 1, 0.3 s, peak 1 MiB** (300 s / 3,072 MiB) | `build-equity/mega-p9lw-train-label-terminal-run1/receipt.json`, `4e49bfb6230f7d2399bb33cbd79c64b6b4743e16b28dd5c96be4ac12c60c7e20` |

w's `stderr.log` (125 bytes, whole): `Unavailable: IC runner: required_bytes=3448251496 max_compiled_slots=8 exceeds
configured memory budget before payload load`. The cycle: `research_cycle: HARD-STOP [w]: receipt
build-equity/mega-p9lw-train-label-terminal-run1: outcome process-error, exit_code 1`; cycle exit 4. Cause (code read,
`atx-impl/src/strategy_ic_admission.cpp:322-340`, `within_budget` `:353-362`): under `--label-terminal` the admission
adds `label_terminal_bytes` (the pinned label role resident beside the role, 30 B/cell, plus a second label / rank /
scratch set for h 5, 21, 63) to an estimate that on the w pass already carries the composition plane; 3,448,251,496 B
(3,288.5 MiB) > 3,072 MiB (3,221,225,472 B), short by 227,026,024 B (216.5 MiB). The u pass fit its 2,560 MiB budget.
The estimate is the exe's conservative admission, not a measured RSS (the parent's w peaked 1,426 MiB at the same
budget, `p9-b0g-yf0-w-run`).

Each phase's exe command (from its receipt, verbatim; the bounded-runner argvs are in the filtered console):

- u: `C:\atx-wt\pool-2\build-equity-rel\bin\atx-equity-strategy-ic.exe --library atx-impl/strategies/fund_industry_ic_v8ysb.json --library-sha256 41010b0bbc662922e6e873057e1fc39d046d6f01a0a5124dd8a601c25a1308f5 --train build-equity/train-2020-2023-lo3/manifest.json --train-sha256 e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4 --train-fields build-equity/train-2020-2023-lo3-fields-v15 --train-fields-sha256 26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09 --output build-equity/mega-p9l-train-u-label-terminal-1 --max-memory-mib 2560 --min-names 1000 --workers 4 --save-combined --label-terminal imputed-v1 --train-label-role build-equity/train-2020-2023-lo3-dlret/manifest.json --train-label-role-sha256 95e16cfe3ad0e0c9e7cf94acb6df4004e6f286ae59dff7c0e725ea33aace5069 --candidate-cache build-equity/mega-candidate-cache-v8-lo3`
- fit: `"C:\Program Files\Python312\python.exe" atx-impl/tools/fit_composition_weights.py --library atx-impl/strategies/fund_industry_ic_v8ysb.json --library-sha256 41010b0bbc662922e6e873057e1fc39d046d6f01a0a5124dd8a601c25a1308f5 --train build-equity/train-2020-2023-lo3/manifest.json --train-sha256 e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4 --orientations build-equity/mega-p9l-train-u-label-terminal-1/orientations.json --orientations-sha256 685f3228546dffea12055512da023f37956ceb075c8a74cc87bbee61ce52926e --runner-summary build-equity/mega-p9l-train-u-label-terminal-1/summary.json --runner-summary-sha256 ced20f07f18ed8b1b4d206331c15f1b41eaaf9e568187e1632ad1ab9a1ce98e3 --orientation prior --screen v4-prior-v1 --composition ew-theme-std-v1 --theme-erc theme-erc-v1 --recipe atx-impl/strategies/fund_industry_ic_v8ysb.recipe.v2.json --recipe-sha256 9d357c757943379a2aff1978de73594a2c7cbd70983f10ac65c0dd02df458759 --work-dir build-equity/fit-work --max-seconds 150 --output build-equity/mega-weights-p9l-label-terminal`
- card: `"C:\Program Files\Python312\python.exe" atx-impl/tools/alpha_report_card.py --u-pass build-equity/mega-p9l-train-u-label-terminal-1 --train build-equity/train-2020-2023-lo3/manifest.json --train-sha256 e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4 --admission build-equity/mega-weights-p9l-label-terminal/admission.json --admission-sha256 52c84c3aa0f9217af4d01e2fb7c36254fe1dd62720649dabce3b2e0b4faffe0d --workers 4 --work-dir build-equity/fit-work --output build-equity/mega-cards-p9l-label-terminal`
- w (refused): `C:\atx-wt\pool-2\build-equity-rel\bin\atx-equity-strategy-ic.exe --library atx-impl/strategies/fund_industry_ic_v8ysb.json --library-sha256 41010b0bbc662922e6e873057e1fc39d046d6f01a0a5124dd8a601c25a1308f5 --train build-equity/train-2020-2023-lo3/manifest.json --train-sha256 e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4 --train-fields build-equity/train-2020-2023-lo3-fields-v15 --train-fields-sha256 26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09 --output build-equity/mega-p9lw-train-label-terminal-1 --max-memory-mib 3072 --min-names 1000 --workers 4 --save-combined --label-terminal imputed-v1 --train-label-role build-equity/train-2020-2023-lo3-dlret/manifest.json --train-label-role-sha256 95e16cfe3ad0e0c9e7cf94acb6df4004e6f286ae59dff7c0e725ea33aace5069 --candidate-cache build-equity/mega-candidate-cache-v8-lo3 --composition-weights build-equity/mega-weights-p9l-label-terminal/composition_weights.json --composition-weights-sha256 e3a9e6fd45bc8dc645e09f88e8f92ba65bcacc5b1d9598674377e8c763428c1e`

## 4. Output dirs (every file's SHA-256: `cells/logs/p9l-outputs-run1.txt`, relpath TAB size TAB sha256)

| dir | files | listing SHA-256 |
|---|---|---|
| `build-equity/mega-p9l-train-u-label-terminal-1` (u) | 12 | `c0492c149bc38fb8ccf700fcbebe24c1484cdb8dd79e852cc25eeb9f2c7ea3f8` |
| `build-equity/mega-p9l-train-u-label-terminal-run1` | 4 | `f5a412232a04cb15d8efdb0c2280d414cb3e9740f3559b50a6e080deac9ee411` |
| `build-equity/mega-weights-p9l-label-terminal` (fit) | 3 | `267b1febdc8b80d8812d32e85d0b41b68311cffb313b8fc18ce3e906766ca2c1` |
| `build-equity/mega-weights-p9l-label-terminal-run1` | 4 | `7f178f5abe2ed28c47e47cfb69c59c2c63f06bc05a3ad7a88b5cf00f1956c9e6` |
| `build-equity/mega-cards-p9l-label-terminal` (card) | 138 | `54d7b53cf5110b492f7fa0e645fc22ca7e69312d9e3bb7dc09d8c479f4e53c8a` |
| `build-equity/mega-cards-p9l-label-terminal-run` | 4 | `2779830aa13789744bb54e72653be49808cda54d8be25449effa5d2a9bf6446b` |
| `build-equity/mega-p9lw-train-label-terminal-run1` (w, refused; no output dir) | 4 | `6f1bf1ebf345229dd90f759b4aaf3a8f9762f0b6a2e8dac609de240fe6a1021b` |
| `build-equity/p9l-label-terminal-readers` (mech-parent) | 5 | `f3af81bfc4360e7bd72f7b606e9115ce0f371c31952360a1b3bf13bfa9077462` |

Key files: u `summary.json` `ced20f07...98e3`, `orientations.json` `685f3228...926e`; fit `admission.json`
`52c84c3a...fe0d`, `composition_weights.json` `e3a9e6fd...8c1e`. None of their contents was read (hashes only).

## 5. Trial ledger, real cache, seal

- Trial ledger after run 1: 133 lines, `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554` (unchanged;
  the gate's 9 admission trial ids already ledgered, as the plan recomputed). No trial line (no verdict; CELLS-2).
- Real cache after run 1 (`cells/logs/p9l-cache-listing-after-run1.txt`, 686 files, listing `329f1368c2fca0a5`): the
  552 original rows unchanged (name, size, mtime), 0 removed, 134 added: two new IC-result scope dirs under the Release
  VM entry of role `e1c67101`, `ic1_af219b3f80b78243` (67 records, `label_terminal` `imputed-v1`) and
  `ic1_ea0a12a3bb527150` (67, `missing-exit-v1`), each recorded by engine `5f48a25d` (the frozen build; `110fd735` is
  its ancestor) (`cells/logs/p9l-s2-6-scan-after-run1.txt`, keys only). These 67 `imputed-v1` records are P9-L's own,
  post-fix: S2-6 (pre-`110fd735` entries) is not engaged; the scan tool's last line keys on presence only.
- Hand seal scan (`cells/tools/hand_seal.py`, `cells/logs/p9l-seal-scan-1.txt`): 11 logs (the u / fit / card / w run
  dirs, the mech-parent reader, the cycle console), forms iso / compact / year / quarter: 5 sealed tokens, all file
  names of the untracked owner PNGs in the cycle's "dirty outside the code pathspec" line (2026-10-02 A x1, 2026-10-03
  A2 x3, 2026-10-06 A2 x1); OTHER 0.

## 6. Verdict, rho, printed-only

- Verdict: **none** (STOPPED at w before any NAV; the plan's "No verdict, STOP" branch: a run failed).
- rho: not computed (no cell NAV). Printed-only lines of B.1: none produced, none read. Predictions: none registered.
  Appendix A block and identity-bridge flag line: not printed (no verdict).

## 7. Void checklist (B.0)

(1) held (ruled, P9-B0, re-pin `2078f877`, ALD-DIAG printed before the run). (2) held: every input = the block and the
registration (template / recipe SHA-256s, parent, flags, fields v15, L, exes, eval and fit mode); nothing changed after
the stop. (3) held: no return, IC, Sharpe or NAV statistic of the cell read (the gate's per-candidate lines filtered;
the u / fit / card outputs only hashed). (4) held: nothing at or after 2024-01-01 opened (hand seal scan OTHER 0). (5)
held: exes 27 / 27 = the table, no rebuild. (6) held: `ucrtbase.dll` = the record. (7) held: no cache-index flag;
`index.sqlite` absent.

## 8. Disclosures

- `--seal-allow` has no object on the v1 hand cycle (SEAL-REF ground); the hand scan replaced it.
- The u pass ran the full-run flags (`--save-combined`), the parent's u the screen route (`--no-composition`, B-1):
  route, not an input of the block.
- The u / card outputs and the cache additions stay on disk, reusable by a resumed cycle (u, fit, card "done"); the w
  run dir `-run1` is never overwritten (a resume runs `--attempt w=2`).

## 9. Open questions (for the PM)

1. The registered w budget (`ic.w_flags --max-memory-mib 3072`, runner w cap 3,072 MiB; C.14 "carried alike") is below
   the IC exe's admission estimate under `--label-terminal` (3,448,251,496 B; minimum 3,289 MiB). Running P9-L needs a
   template change (e.g. `change.flags.w`-level `--max-memory-mib` >= 3,289 and the runner's w `max_rss_mib` alike, free
   RAM stated first) = a re-registration with a new template SHA-256: is a resource budget an "input" under void (2),
   and is that edit allowed?
2. If ruled yes: resume from u / fit / card done (the same outputs), w as attempt 2, then the plan unchanged.

## 10. Resumed run and own mechanics derivation (2026-10-17)

P9L-WBUDGET and SEAL-HAND answered section 9. Restart `89b8ae72`: all 27 Release and 27 Debug exes
equal receipts; all six P9-B0 reference listings and CRT match; absent catalog/index and missing old fields
remain absent. Evidence: `cells/start-state-2.txt`, `cells/logs/exe-sha-{rel,dbg}-2.txt`.

Resource registration `f560f741` changes only w IC admission and runner RSS to 3584 MiB (two added
JSON entries per file). Template SHA `1d15513e74851e776a7e40d538f528690fbab04c4a92bb8afdcc240a805e3170`;
run recipe SHA `1f00e19ec9dc3092ebe570b9645700910062881835dc9cd391623cff76c00062`.
PRE-40/plan commit `7a49c825`; exact diff/body supplement `9e17644f`,
`cells/p9l-resource-registration-supplement.md` (the original commit body omitted the required diff).
No research input or exe changed. The original description's "run caps carried alike" is superseded by this ruling.

RAM polling ran 13:22:23Z to the owner's stop at about 13:36Z; its log is preserved in
`cells/logs/p9l-ram-wait-2.txt`. R15-RAM-NOW, committed before launch at `f6512b51`, removes both the
extra idle reservation and optional admission-wait route. No source/spec change accompanied that direction.
Only our RAM helper PID 14312 was stopped. All real runs kept the registered time/RSS caps and 512 MiB
system-free launch/runtime floor; no host-budget or admission-wait option. C: stayed above 15 GB.

| stage | exact cycle command / reader | result | receipt |
|---|---|---|---|
| w | `research_cycle.py run scripts/specs/p9/templates/label-terminal.json --stop-after w --attempt w=2` | completed exit 0; 24.609 s; peak 1693 MiB; prelaunch free 2188 MiB, C: 40.7 GB | `build-equity/mega-p9lw-train-label-terminal-run2/receipt.json` |
| NAV | `research_cycle.py run scripts/specs/p9/templates/label-terminal-cal.json --stop-after nav` | completed exit 0; 39.187 s; peak 694 MiB; prelaunch free 3971 MiB, C: 40.7 GB | `build-equity/mega-nav-p9l-label-terminal-gm-run/receipt.json` |
| mechanics | `hand_readers.mech`, DRIVER empty under R15-RAM-NOW, `mech-matched`, cell's published NAV | completed exit 0; 0.532 s; peak 40 MiB; prelaunch free 4077 MiB, C: 40.6 GB | `build-equity/p9l-label-terminal-readers/mech-matched-run1/receipt.json` |

All interpreter invocations use `C:/Program Files/Python312/python.exe`. u / fit / card were reused;
failed w run1 preserved; w output is `build-equity/mega-p9lw-train-label-terminal-2`.
The gate appended zero admission lines, nine already ledgered. Child w argv differs from run1 only in
the registered budget and attempt output path. NAV is the one calibrated publication; no second NAV.
Complete bounded argvs are in `p9l-run2-cycle-filtered.txt`, `p9l-nav-cycle-filtered.txt`, `p9l-mechanics.txt`
under `cells/logs/`; raw cycle logs remain unread except through the filter and seal scanner.
w receipt SHA `5abc456422ee22ec138f3e5383a07093273f123f0c00a6422f070198aca4c257`;
NAV receipt SHA `6a7a4ff131f3a0c9791cbd00219942a35b2db9e2776e53d553c50eae1071ea57`.
Every output file is hashed in `cells/logs/p9l-outputs-run2.txt`.

Calibration (`cells/logs/p9l-match-check.txt`): target and gross both `0.9862134133265011`, tolerance `.005`,
requested leverage `1.1828`, requested gross `0.9862134133265011`, corrected false, published leverage
`1.1828`, matched true, construction_reused true. All required keys present; requested L and target equal
registration; reader gross equals calibration gross bit for bit. L-prime is therefore `1.1828`.
Plain record spec generated with `wave_stage_cell.record_doc`, dry-locked, committed `ac903247`:
`scripts/specs/p9/templates/label-terminal-gm.json`, SHA
`de44a668999ab8997c7118afb52efd93551ac2e7df93efe9118edbf719aa9a4d`.
Saved plain planned argv: `cells/logs/p9l-plan-label-terminal-gm.txt` (`--aim-leverage 1.1828`, no calibrate
flags). The actual receipt correctly carries the -cal flags; both bind the same -gm output/cycle directory.

Own derivation from B.1's six unscaled G-B9 predicates, not from a return statistic:

| predicate | value | registered limit | result |
|---|---:|---|---|
| all-rows S2 gross | 0.9862134133265011 | [0.90, 1.05] | pass |
| absolute all-rows mean net | 0.005339981852022642 | <= 0.02 | pass |
| tau mean | 0.028134396709054203 | <= 0.20 | pass |
| tau p95 | 0.033410443831757175 | <= 0.30 | pass |
| maximum return identity error | 3.67544536472586e-16 | <= 1e-9 | pass |
| maximum cash-book relative error | 1.1074477435259236e-13 | <= 1e-9 | pass |

Every value is finite; all six inequalities hold. **P9-L ACCEPTED by the registered mechanics-only rule.**
Reader file SHA `2ba459ca68cc513f4b896f99a5507cdec946deb34eb501590cd165c9f929fc68`.
This decision is committed before any registered rho, return bundle or IC/admission/orientation print is read.
Fresh DERIV is still required; the decision is not yet ledgered.

Hand seal scans after w, NAV and mechanics: OTHER 0 (`p9l-seal-w2.txt`, `p9l-seal-nav.txt`,
`p9l-seal-mechanics.txt`). Cache unchanged since run1: all 686 filename/size/mtime rows identical,
listing `329f1368c2fca0a5`; no new records from w2. Existing 67 imputed-v1 records come from frozen
post-`110fd735` build `5f48a25d`, so S2-6's pre-fix exclusion is not engaged; none deleted.
Void checklist (1)-(7): all held; no cache index/timings flags; print flags inherited alike; no risk model;
no hidden data read; no rebuild. SEAL-HAND replaces the inapplicable v1 --seal-allow switch as ruled.
Ledger before and after w/NAV/mechanics: 133 lines,
`27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.

Identity-bridge flag: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit`
depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0);
size unmeasured. Absolute returns may be overstated by survivorship; paired comparisons are relative evidence.
PRE-4 exposure remains: labels within 30 days of TRAIN end can embed post-TRAIN information, bounded
by ALD-DIAG's two unclassified exits; no truncation code. Predictions: none registered.

Rho, printed-only rows and Appendix A accounting block follow only after this mechanics commit.
No permission refusal. One pathspec typo (`idx-final-chain.md` at sprint root) failed before staging;
the correct path `r14-prompts/idx-final-chain.md` was located and used. No research stop in the resumed run.

## 11. Registered printed-only rows (after mechanics commit `7bceff42`)

Rho ran first: `cells/logs/p9l-rho.txt`, bounded receipt
`build-equity/p9l-label-terminal-readers/rho-run1/receipt.json`, completed exit 0.
**rho = 1.0; 1 - rho = 0.0; `1 - rho < 1e-6` true.** The primary daily CSVs have the same SHA prefix
`e7eb57201ea5`; 1005 common sessions. This is a degenerate pair under C.11 / PRE-12 / PRE-33.
P9-L's acceptance remains mechanics-only, so the degenerate printed comparison does not change its verdict.

Paired bundle (cell minus parent): **dSR 0.0; Memmel SE na; two-sided p na; one-sided p na**.
The bundle emits null for these undefined statistics, rendered "na" under the registered fallback;
the point dSR is not positive, which is a printed observation, not P9-L's acceptance predicate.
`build-equity/p9l-label-terminal-readers/bundle-parent.json` SHA
`e3b60f29c31c9a7f75139abc24ab8afc556f18eefd7e4c66cdb314469bb20038`;
`nav_summ_run.engine_exe_sha256` equals pinned eval
`e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c`.
Receipt `build-equity/p9l-label-terminal-readers/bundle-parent-run1/receipt.json`, completed exit 0;
bounded argv and permitted keys in `cells/logs/p9l-bundle.txt`. No year tables or other bundle keys read.
Rho prelaunch free RAM 3949 MiB; bundle 3375 MiB; accounting 4611 MiB. C: >= 40.5 GB.

Orientations: 67 candidate ids match; one sign change, `earn_surprise_comp`: parent 0 -> cell -1
(undefined/zero to negative, rather than a positive-to-negative flip). Admission: 67 ids match,
**zero status changes**. No admission t, Sharpe, return, tau or correlation value was read.

The three named coverage diagnostics below are identical for u and w, for each of their 67 candidates
and the combined coverage. They are per horizon, not summed over candidates:

| horizon | terminal_imputed_pairs | missing_exit_share | guard_excluded_pairs |
|---:|---:|---:|---:|
| 5 | 1852 | 0.0015602313756518339 | 51 |
| 21 | 7638 | 0.005423291773097393 | 226 |
| 63 | 20739 | 0.01490568549284752 | 595 |

The registered `ic_missing_exit_v1` blocks for all 67 candidates in each pass are printed in
`cells/logs/p9l-printed-only.json`, beside the orientation/status changes and named coverage counters;
SHA `1702efc858c1acb157b7df9df48344cf4b582bba2d23067629e1811c302debd0`.
Extractor `cells/tools/p9l_prints.py` selects only these registered fields. An exploratory structure/key
print emitted the same permitted `ic_missing_exit_v1` blocks to the tool console and was truncated there;
the committed artifact is complete. No unregistered candidate IC block was emitted.

All later seal scans (rho, bundle, accounting) have OTHER 0; evidence `cells/logs/p9l-seal-{rho,bundle,accounting}.txt`.
No extra research variant, no prediction, no trial write. The accounting reader used only `--ledger-n`,
not the ledger-writing `--ledger` option; receipt
`build-equity/p9l-label-terminal-readers/accounting-run1/receipt.json`, completed exit 0.

**Appendix A accounting block, current ledger before DERIV:** TRAIN construction cells 62; admission
trials in the inherited ledger 61, P9 additions 0 (v8 12, X hand-written 25, X-4 re-screens 9,
Y hand-written 15, P9 hand-written 0 [P9-S lapsed, W3-15, OD5-4]); K_a 40 historical hand-written strings;
mined campaigns in P9 0 (v9-mine-c1 from v8: M 110); N_tot 233; N_hand 123; window research-window-v2
(2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024);
history reads 0; 2025+ never read; identity-bridge flag as section 10. Machine evidence
`cells/logs/p9l-accounting.txt`: 123 counted trials in 133 lines. P9-L's N_c +1 is pending DERIV and
the authorized driver ledger step: prospective N_c 63, N_hand 124, N_tot 234; these are not yet ledgered counts.
P9-X under PRE-30 adds 0; no P9-X work was done here.

Final verification (`cells/logs/p9l-end-state-2.txt`, `exe-sha-{rel,dbg}-2-end.txt`): frozen exes 27/27
each, CRT, all six references, absent catalog/index and 133-line ledger SHA all unchanged.
Reader files' complete final listing: `cells/logs/p9l-readers-final-listing.txt`, 23 files,
SHA `b59fd5f44847f36f566ff4dc1274a75559b77f46922588ef040473dac715c026`.

**STOP for PM / fresh DERIV.** Own verdict ACCEPTED; all void checks held. The plain planned argv for
DERIV is `cells/logs/p9l-plan-label-terminal-gm.txt`; the actual NAV receipt carries the calibration flags.
No monitor, summ/trial write, P9-H registration or run has started. Next cell, only on PM release, is P9-H
over this accepted plain record spec if DERIV agrees and the PM rules it. No open question or permission refusal.

## 12. DERIV agreement and authorized ledger completion

Fresh DERIV `86fcd5df` independently found ACCEPTED and the same six deciding values.
Its disclosed broad prereg extraction and direct PowerShell / unavailable-utility deviations were accepted
by PM ruling R15-DERIV-SCOPE; its independence is accepted with that limitation, not described as pristine
section-only access. The PM then ruled P9-L ACCEPTED and explicitly released plan section 3 step 9.
Release and prepared next-root docs committed before execution at `f6a94011`.

Both commands used `C:/Program Files/Python312/python.exe`, no admission-wait or host-budget option,
and unchanged bounded limits: 180 s, RSS 1536 MiB, system-free launch/runtime floor 512 MiB.
They reused completed fields/u/fit/card/w/NAV and appended no admission line.

1. `research_cycle.py run scripts/specs/p9/templates/label-terminal-cal.json --stop-after monitor`:
   free RAM 3512 MiB, C: 40.1 GB; monitor completed exit 0 in 1.844 s, peak 133 MiB.
   Receipt `build-equity/mega-monitor-p9l-label-terminal-run/receipt.json`, SHA
   `62a5046b4f53e116cd654df1819b3ff677db379fa21d52bc2ede88a1b6a7b055`.
   The stop-after checkpoint allowed the hand seal check before summ: OTHER 0.
   Ledger still 133 / original full SHA.
2. `research_cycle.py run scripts/specs/p9/templates/label-terminal-cal.json`:
   free RAM 3846 MiB, C: 40.1 GB; monitor reused, summ completed exit 0 in 11.453 s, peak 75 MiB.
   Receipt `build-equity/cycle-p9l-label-terminal-gm/summ-run1/receipt.json`, SHA
   `f31b3a62e0b3404c2b8d8ee6ab7c59305495b1c6cf7b6b8663faaef8044dd244`.
   Cycle completed; hand seal OTHER 0. The cycle's generic `cycle_verdict.json` was generated but not
   read or used as P9-L's decision; B.1's mechanics derivations and the PM ruling remain authoritative.

**Exactly one appended construction trial:** `trial_id 4a8141373c779aa1`, count 1, origin `prior`,
cell `build-equity/mega-nav-p9l-label-terminal-gm`. Before: 133 lines,
`27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
After: **134 lines, `b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61`**.
The first 133 complete lines hash to the exact before SHA: no prior ledger byte changed.
The new row's `prev_sha256` is `5a3ef9d9bf242dbb416a9b315b45e20377c234d3533e45cd1ae4bafaff8c7503`.
Audit `cells/logs/p9l-ledger-completion.json`; all completion-file hashes in
`cells/logs/p9l-completion-outputs.txt`. Filtered cycle logs and seals are
`p9l-{monitor,ledger}-cycle-filtered.txt`, `p9l-seal-{monitor,ledger}.txt` under `cells/logs/`.
Raw monitor/summ output statistics were not read.

**Appendix A after ledger completion:** TRAIN construction cells 63; historical admission trials 61,
P9 admission additions 0 (split unchanged from section 11); K_a 40 historical hand-written strings;
mined campaigns in P9 0, M 110; N_hand 124; N_tot 234; research-window-v2 (2020-2023);
hidden 2024+ unread in this sprint; validation reads before v8 2 (2023-2024); history reads 0;
2025+ never read; identity-bridge flag as section 10. Counts use `backtest_integrity.trial_counts`
(eight window reruns and event/campaign lines excluded), with the existing Appendix A functions
on the ledger only; evidence `cells/logs/p9l-accounting-after-ledger.txt`. No research reader was rerun.

**P9-L complete; STOP before P9-H.** The exact next parent is
`scripts/specs/p9/templates/label-terminal-gm.json` (SHA `de44a668...9a4d`), paired reference
`build-equity/mega-nav-p9l-label-terminal-gm`. Fresh cells root 3 must receive the separate PM release
for `p9h-cov-hedge`; no H template edit or run occurred here. R15-RAM-NOW applies to every remaining stage.
H is the first v2 cell and must capture G-P1 end-to-end timing/integration evidence. No permission refusal.
