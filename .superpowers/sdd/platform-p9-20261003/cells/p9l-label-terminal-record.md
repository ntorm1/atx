# P9-L (`p9l-label-terminal`) cell record: STOPPED at w (driver HARD-STOP; no verdict)

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
