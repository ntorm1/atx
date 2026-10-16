# Handoff: root GATE (GATE-1, the single wave-2 + wave-3 gate), FINAL, chain agent 4, 2026-10-16

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Dispatch `r13-prompts/gate-prompt-4.md` over
`gate-prompt-3.md` and `handoff-root-gate-3.md` "Exact next step". The record of every step: `root-gate-report.md`
("Status by step after chain agent 4 (FINAL)" at the top; "Chain agent 4" at the end; the round-2b-ii table "Agent 4
(FINAL)"). Evidence and tools: `gate-logs/` (`gate-logs/tools/`, changes in `tools/CHANGES.txt`). Rows:
`integration-log.md` "GATE (RESUME 13), chain agent 4". Earlier links: `handoff-root-gate-{1,2,3}.md`.

- **Status: DONE through step 7; the follow-up re-pin `3a1a4759` (REPIN-UW / REPIN-R2) is STOPPED at its `verify`**
  (two refusals, below and `root-gate-report.md` "Step 6, agent 4, follow-up"; the PM's). Steps 0-5 and 7 hold (step 3's
  X-5 rows and step 7's X-5 opt-in cache identity CUT by FIELDS-X5).
- **Code head: `3a1a4759`.** The re-pin = **`2078f877` + `3a1a4759`** (two pins-only commits over the gate code
  `06dc9e5a`; no exe pin moved in the second).
  **Docs head:** the records commit that adds this file (`git log -1 --format=%h -- <this file>`); this agent's
  docs commits below it: `bfbe64d2` (step 0, GATE-IDX, FIELDS-SUB-A, rows 4-6 amendments before the runs), `c0112afc`
  (row 4), `a24a77bd` (row 5), `cf6dc5df` (row 6, step 5), `883be9f2` (step 6 record).
- **Known-red set: EMPTY.** Named skips as `root-gate-report.md` step 2 (the Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` skip stays, OD5-2).
- **Build tags:** none used by agent 4 (no build at the gate after agent 1's p9-3f / p9-3g). **Next free tag: `p9-3h`.**
- **EXE-FREEZE in force from `2078f877`:** no exe of the frozen set (NAV, IC, eval, composition, admission, store) is
  rebuilt until the freeze gate; an unavoidable rebuild redoes P9-B0 and the re-pin first, no cell between.
- **Trial ledger** `build-equity/trials.jsonl` 133 lines `27e40f9f` (0 trials at the gate); head `5a3ef9d9`, N 62.
  `build-equity/research-store/` absent. No statistic read or recorded (rho as value and flag only, B2-13).
- **Permission refusals: none** (agent 4). GATE-MYPY: nothing staged since the owner's unstage; every commit by
  explicit pathspec, 0 `mypy_cache` paths in each.

## Exe table of record (the re-pin's; Release p9-3f = producer of record, OD5-2)

Source = gate code head `06dc9e5a` (receipt Source `5f48a25d`, a docs commit; `git diff 5f48a25d..2078f877` outside
`.superpowers/` = the three re-pinned specs only, no C++ / CMake file). Receipts `build-equity/mega-p9-3f-receipt.json`
(Release, `build-equity-rel/bin`), `build-equity/mega-p9-3g-receipt.json` (Debug, `build-equity/bin`). 27 / 27 + 27 / 27
`disk = receipt` at agent 4's start and end (`gate-logs/exe-sha-{rel,dbg}-4{,-end}.txt`). The 16 test targets: the
table in `root-gate-report.md` step 1.

| exe | Release p9-3f SHA-256 | Debug p9-3g SHA-256 |
|---|---|---|
| atx-equity-strategy-targets (NAV; pinned `exes.nav`) | `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565` | `5dced610e204ba860bd2c8c8da5c3c6bd58a1ef98c242bec352a6b64d8762a8f` |
| atx-equity-strategy-ic (IC; pinned `exes.ic`) | `ccd54db605f67fef1aa480050a6c785ffe179dae2f77e978dd6d01a5c117d705` | `adffe7421781145a9718e67be05b46038b89a46311f26390eeb3312da236d56d` |
| atx-research-eval (pinned `exes.eval`) | `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c` | `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` |
| atx-research-composition | `04db0d67c1563184edd6fe8ff66796877c8db20239d5cfb423fa78e047df16f2` | `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e` |
| atx-research-admission | `f53d9277e4ad2e3915343ccca032df3f0098a1a1cd7b4d832b640f5f7437063f` | `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` |
| atx-research-store | `0eeac21bb95677d8da5268ca829b88dc668521a4c9f625e0fbbde92f96fb3818` | `a54955e965ee2aa035e66cc9ed668a16246c089684a53515431dea428b605c73` |
| atx-research-fields | `883c5b74a95731eb310cfd30cb1ab39ee102e077ab2d7318bd5db941f486584a` | `1c670d86a0fdcae6a86b0eff56a184902d1eed8b244f8af6ab31425e18185fac` |
| atx-equity-strategy | `f883f81f0fbb928cb8fe2e8b1931841e0eef058e7b6398810e10de68cb0495a4` | `47d341c2541e792873ec5d3db0b739af6de61e9962ba46667c755a1ca56ca9a4` |
| atx-equity-strategy-risk | `0c110534dd2df43e995040a07e5368eac788166b5e55cc2e0e2fa5e088be586b` | `6696da719374777aefb49ce9e6b4707187549dff9b838e0c2338d21e852b47a1` |
| atx-equity-strategy-mine | `daf889437983628ccbc375e76a078d3decf079a9d3a79b6cca79cbcc4a063eba` | `4dee20571d4c4481bcf175d8bb688574ba95c60ae29d08b4bb474849f7945412` |
| atx-impl | `71f0c6311aaffffaa48f4c221c18c94e009a3b7ebc5a45798baff009d75bbdb9` | `7095d4477dd7fee80ead2d9bbdb516a726ed3ab943beed95ac1dc5335af63e55` |

## The re-pin (`2078f877`, then `3a1a4759`)

| spec | SHA-256 before -> after | pins |
|---|---|---|
| `scripts/specs/v8/lib-v8ysb-gm.json` (Y-F0, the lineage parent) | `1e3ec118...8626` -> `3631679cd18f1c6a63a6a5dc96cdbe6ee7669b81b8979f529544f1b237d9edea` | `exes` -> the Release NAV / IC + `exes.eval`; `exes_sha256` {eval, ic, nav} as the table; `nav.output` -> `build-equity/p9-b0g-yf0-nav` |
| `scripts/specs/v8/x-leverage-L2.0.json` (X-10) | `ebe68e6e...4975` -> `46e9d6c2fdbf42e362518118938bc89af038e3c09d7519d660aaa936554fd0bf` | `nav.output` -> `build-equity/p9-b0g-x10-nav`; `locked.reference_cell` -> `p9-b0g-yf0-nav/summary.json` `826d5ec6...8f38` |
| `scripts/specs/v8/y-vol-target-y-1.json` (Y-1) | `faffee17...5f77` -> `f4186573a41de3b67cdd815976639ed2123e5c6f43353516c61150d92dbb50d6` | `nav.output` -> `build-equity/p9-b0g-y1-nav`; `--risk-model` -> `build-equity/p9-m2i-dcov-v11` `397366473b3a...23c7`; `locked.reference_cell` -> `p9-b0g-x10-nav/summary.json` `deabe71b...0bd` |

Second commit `3a1a4759` (REPIN-UW, REPIN-R2): `lib-v8ysb-gm.json` (`3631679c` -> `4081f473173cd0068bb173a2cb3b2b761592b791705cf7be33bc95fe3f06e2ee`)
`ic.u_output` -> `build-equity/p9-b0g-yf0-u`, `ic.w_output` -> `build-equity/p9-b0g-yf0-w`; `lib-v80.json` (`306a070b` ->
`7f6e45ee53c1854f7a3147f0a1b6e1021d67bf854f0ae9c57c17c37e1fb1bb89`) `fields.output` -> fields v15, `manifest_sha256` ->
`26fee5ce...3b09` (the 70-name list unchanged), `nav.output` -> `build-equity/p9-b0-r2-nav`. **Its `verify` REFUSES**
(`gate-logs/repin-verify-2.txt`): (1) the inherited u / w resolve `p9-b0g-yf0-u-1` / `-w-1` **pending** (the cycle names
IC passes `<base>-<n>`, `scripts/cycle/phases.py:566-568`, `:596-598`), so a NAV-only cell would re-run u and w;
(2) `lib-v80.json`'s as-built fields check fails (v15's 84 rows vs the 70-name list, `scripts/cycle/gate.py:22-28`).
The fixture chain digest holds (`a50fdcf9...100c` twice, `fixture-chain-5.txt`). STOPPED as instructed: nothing
reverted, no second attempt. Not edited: `mv-aim-v1.json` (COVMV-PIN, `4b4af6e2`). Read: `info-clock-rate-v1.json`
`7801c198...a152` (W1-I2). `verify` after `2078f877`: `gate-logs/repin-verify.txt` (X-10 and Y-1 hold; the parents' NAVs made by the pinned
Release NAV exe); after `3a1a4759`: `repin-verify-2.txt` (refuses, above).

## Reference dirs of record (the gate's P9-B0; never deleted; per-file SHA-256 in `gate-logs/b0-files-<dir>.txt`)

| dir | role | files | listing SHA-256 | `summary.json` SHA-256 |
|---|---|---|---|---|
| `build-equity/p9-b0-r2-nav` | R-2 re-based (path A, FIELDS-SUB-A; fields v15; w = v8's `mega-v8-r1w-train-std-v80-1`) | 27 | `04bb8060aadf692a63e7f1baafd23f297db67c24d9c1f0a43dce6678a755b1bd` | `48a52f6adb06c2e966cb366f2c71c9dbef55ba48de243afc2cce01df3085c1b9` |
| `build-equity/p9-b0g-x10-nav` | X-10 re-based | 27 | `6c87c8d516d7782c2484d6cc397323359f9f9b61cc242a5f894885240a74f31d` | `deabe71b5e2dedfb0b224fb46fdcd0e7194a6950ad8f5ecabe9601cd786bd0bd` |
| `build-equity/p9-b0g-y1-nav` | Y-1 re-based on `p9-m2i-dcov-v11` (PRE-29; no identity claimed) | 28 | `cac882e19a5baee9d3d60581d93a278f9f4fc6b769ed0e9154dd408b1552b813` | `b0d4dcbf53e81aff3863ca1d9c7230da73a5c0a911a60f622c9f78aa3796c028` |
| `build-equity/p9-b0g-yf0-u` | Y-F0 u (Release IC, cold) | 5 | `1cb087abaa50746392269b3452491be8fa2999b9a39e1212f01049acec2916aa` | `acbf61cef24e909bb8975668b1b2206708b5c96cf03194a0728564445fae12b8` |
| `build-equity/p9-b0g-yf0-w` | Y-F0 w (Release IC, warm) | 12 | `ec69344005107bf0e2ab0bb859583db083889521a652dcfe134b2304e27c8ae1` | `f772b394a6885b5965d0c5ad93e4f023515a74ed7e02064a772814357ee904f2` |
| `build-equity/p9-b0g-yf0-nav` | Y-F0 NAV of record (reads `p9-b0g-yf0-w`; `--ann-mean-gross --info-clock-print`) | 27 | `e205d6ecb16e75cba02194b90b313849404c2052b3e1738c4bf020e486fcf797` | `826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38` |

Each with its `-run` dir (receipt, start, logs; receipts copied to `gate-logs/receipt-<dir>.json`). rho (B2-13): P9-X
pair (Y-1 vs Y-F0) 0.9947035686613187, X-10 vs Y-F0 0.9994863034956717, Y-1 vs X-10 0.9947113980219978; every
"1 - rho < 1e-6" flag false (`gate-logs/rho-pairs-{1,2}.txt`).

## Substitution list

`root-gate-report.md` "Step 4, agent 3: the P9-B0 substitution list" (`778d34b8`, committed before any P9-B0 run) and
"Step 4, agent 4: substitution-list amendments" (`bfbe64d2`, committed before rows 4-6): 1 R-2 fields v10 -> v15
(FIELDS-SUB), **ruled path A (FIELDS-SUB-A)**; 2 X-10; 3 Y-1 store -> `p9-m2i-dcov-v11` (PRE-29); 4-5 Y-F0 u / w (token 0
and `--output` only; RELCACHE-ALLOW's classes); 6 Y-F0 NAV `--combined` -> `p9-b0g-yf0-w/train_combined.json`
(YF0-NAV-ORDER) + `--ann-mean-gross --info-clock-print`; X-5 cut (FIELDS-X5); token 0 -> the Release exe and
`--output` -> the new dir everywhere; `--ann-mean-gross` on every NAV reference; `--seal-allow` on none (SEAL-REF).

## `ucrtbase.dll` record (C.6, M2c-RED-2 (d))

10.0.26100.7623, SHA-256 `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF`: at step 0 (agents 1-4), at
the cache-free P9-B0 runs (14:22:31Z, `gate-logs/ucrt-b0-3.txt`) and at the Y-F0 runs (15:39:47Z, `ucrt-b0-4.txt`) =
the CRT-2 record of the two D-COV stores. A change before a cell redoes P9-B0 and the re-pin.

## Cache listings (GATE-IDX, GATE-RELCACHE; `gate-logs/tools/dir_listing.py` form)

- Before the Release pass (after the owner's deletion of `index.sqlite`): `build-equity/mega-candidate-cache-v8-lo3`
  351 files, 7,799,539,072 bytes, **`155872de50833fca`** (`gate-logs/cache-lo3-listing-agent4-idx.txt`; with the GATE-IDX
  file it was 352 `fb444f7d316a1a4c`).
- After (cause: "Release identity subtree added at step 4 (p9-b0g-yf0-u)"): 552 files, 12,265,945,104 bytes,
  **`c9ee9291f5ddc0c2`** = the 351 originals unchanged (`155872de50833fca` restricted) + 201 files / 4,466,406,032 bytes
  under `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/` (`cache-lo3-listing-agent4-final.txt`); the warm w added nothing.

## Store step (step 7)

Catalog digest of the gate head **`adbcd348b5d82a1fd4f357274a34bbf387391cf82154a961e701bad39a0dd474`** (bounded root run
`build-equity/p9-b0g-cat-2-run`, DBs outside the root), reproduced by a second from-scratch catalog; `verify --pins`:
ok 1,620 / declared 169 / stale 220 / missing 630 / unresolved 1,512 (artifacts verified 4,190, declared 84); render
identity 435 checked, 0 mismatches; ledger verify ok. Fixture digest `a50fdcf9...100c` reproduced. `classes.json`
`5bc27ba4...6557` (by hand, STORE-S S-4). G-P: G-P1 not checkable (the first P9 wave), G-P5 PARTIAL (IC-S), G-P2 / 3 /
4 / 6 / 7 / 8 / 9 / 11 ticked; G-P1 measured at P9-H by the cells root (GP-TICKS); G-P10 (a) (b) (c) (e) landed, (d)
PARTIAL.

## What the cells agent must know

- **The lapse table of record is LAPSE-TABLE** (ledger :981): P9-L, P9-H, P9-M, P9-C, P9-MV, P9-V RUN; P9-W LAPSED (0)
  (D3-Q1a DIFFERENT); **P9-X pending PRE-30 / PRE30-MVX**; order unchanged (A.7). **Fit mode of every v2 cell:
  flag-absent** (the Python fit; P9-F0h produced flag-absent). Eval mode: research-fast (the reference's, PATH-1).
- **PRE-34's printed-precision agreement** of the re-based Y-1 vs v8's ledgered Y-1 is the CELLS root's, at the P9-X
  step, before the adoption print (PRE34-WHO); the gate recorded the path-level diff only (`gate-logs/y1-cmp-v8.txt`).
- **The lineage parent is the re-pinned `scripts/specs/v8/lib-v8ysb-gm.json` (`4081f473` at `3a1a4759`)**; until the PM
  rules on the follow-up's refusal, a NAV-only cell over it would re-run u / w into `p9-b0g-yf0-{u,w}-1` (verify item 1); set each P9 template's
  `parent` to it at the cell's registration commit and `lock --exes --write` the cell (the pins it inherits are the
  table above; B2-11 checks `exes.eval`). At `2078f877` a NAV-only cell inherited Y-F0's u / fit / card / w (the v8
  dirs, reused: pre-K-P9-10 receipts); at `3a1a4759` u / w name the P9-B0 dirs (pending, above); it pairs against
  `p9-b0g-yf0-nav` (W1-I2 (a) holds: the parent NAV is the pinned Release
  exe's). P9-X's reference spec is the re-pinned `y-vol-target-y-1.json` (`p9-b0g-y1-nav`, the v1.1 store).
- `lib-v8ysb-gm.json` alone cannot be built by `research_cycle.py status` (its own compare input `baseline_fields` is
  fields v13, FIELDS-GONE); templates over it resolve without it (`gate-logs/repin-verify.txt`).
- The reference NAVs are exe-direct bounded runs: they carry no C-13 binding to their specs and their argv carries the
  substitution list's opt-in flags (`--ann-mean-gross`; Y-F0 also `--info-clock-print`); every cell carries
  `--ann-mean-gross` (PRE-7a) and P9-C the clock flag, so the paired opt-in leaves are alike on both sides.
- R-2 (G-B6, G-B8, OD-3) is read by path: `p9-b0-r2-nav`; its TRAIN fields dir is `train-2020-2023-lo3-fields-v15`, its
  TRAIN w `mega-v8-r1w-train-std-v80-1` (the OD-3 runbook's R-2 [UNKNOWN] slots); `lib-v80.json` re-pinned at
  `3a1a4759` (REPIN-R2) and its fields check refuses there (70-name list vs 84 rows): OD-3's P1 would stop on it.
- The real cache now holds the Release identity subtree (Release u / w of a cell over the Y-F0 library run warm).
  No cache-index flag on any argv (SQL-11); never re-create `index.sqlite` (`cache init`).
- EXE-FREEZE from `2078f877`; `ucrtbase.dll` as above; the trial ledger is still 133 / `27e40f9f`.

## Open questions for the PM (one line each)

1. REPIN-UW: the cycle resolves `ic.u_output` `p9-b0g-yf0-u` as attempt `p9-b0g-yf0-u-1` (pending; same for w): rule how u / w references of record are named for the cycle, or revert to `2078f877`'s v8 dirs?
2. REPIN-R2: `lib-v80.json`'s 70-name `fields.list` against v15's 84 rows fails the cycle's as-built fields check: list -> 84 rows (not a pin), or fields pin back to v10?
3. Earlier Q3 (RELCACHE-ALLOW tool correction) accepted; GP-TICKS applied (G-P5 PARTIAL, G-P1 at P9-H).
