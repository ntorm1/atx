# Root gate report (GATE-1: the single wave-2 + wave-3 gate), P9, 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Chain agent 1 (GATE-DISPATCH-12; dispatch
`r12-dispatch.md` "ROOT GATE", `r11-dispatch.md` "ROOT GATE" + "ROOT GATE: rulings from the whole-wave review",
`root-gate-brief.md` steps 0-9). Evidence and tools: `gate-logs/` (tools in `gate-logs/tools/`). Compared by SHA-256,
JSON path, exit code and counts only; no 2020-2023 statistic is read or recorded anywhere in this file.

- **Gate head (code): `06dc9e5a`** (`Merge lane SQL4 at 44a76937 ...`). Docs head at dispatch `0a481348`; the docs
  head moves under PAR-1 (the IDX docs agent committed `5f48a25d` `rulings-index.md` beside root; the PRE agent edits
  `p9-prereg.md`). `git diff --name-only 06dc9e5a..HEAD` outside `.superpowers/` is empty at every build and run below.
- **SQL4 verdict carried (SQL4-CUTOFF-2):** checks green at M6B-FINISH: SQL4 is in the gate head; a red in any of its
  24 files attributes to merge commit `06dc9e5a`.
- **Known-red set: EMPTY.** Named skips are listed per suite below; the Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` named skip stays (OD5-2).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 `27e40f9f...` at every check (0 trials).
- **GATE-IDX:** `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` (73,728 bytes, sha256 `3a177dfd...c6f0`) was
  present at step 0; this agent never deletes it and runs nothing naming that cache while it exists (see step 3).

## Status by step after chain agent 2 (2026-10-16 ~13:45Z; the tables below it are agent 1's, as they stood at OWNER STOP 12)

**Gate status: BLOCKED-OWNER** at step 4: (i) GATE-IDX: `build-equity/mega-candidate-cache-v8-lo3/index.sqlite`
still present (the owner's to delete; no run of record named the cache); (ii) NEW: the fields directories
`build-equity/train-2020-2023-lo3-fields-v10/` and `-v13/` (with v9 / v14 / v15a) are absent from disk: R-2's NAV argv
(the P9-B0 re-base) and X-5's NAV / u / w identity argvs cannot run. No code red; known-red set EMPTY; 0 trials.

| step | state | evidence (`gate-logs/`) |
|---|---|---|
| 0 start state | **holds** (re-verified by agent 2; exes re-hashed `disk = receipt` 27 / 27 + 27 / 27) | `start-state-2.txt` |
| 1 exes of record | **holds** (agent 1; unchanged) | `mega-p9-3{f,g}-*`, `exe-sha-{rel,dbg}.txt` |
| 2 suites | **holds**: Release gtests 4,098 / 0 failed; Debug gtests 4,099 / 0 failed; ctest 291 + 728 on both trees, 0 failed; `scripts/tests` seeds 0 and 1: 703 / 0 failed / 3 named skips each; stage chain 10; `atx-engine/tools` 391; `atx-impl/tools` 770; `atx-impl/strategies` 14; fields 4 / 26 / 16 / 37; mirror guard 7; GATE-TMP closed (the tool's TMP root; two TMP-root effects shown, `tools/CHANGES.txt`); GATE-COVDIFF holds (3 x exit 0) | `gt-exits.txt`, `ctest-exits.txt`, `pyt-exits.txt`, `covdiff-runs.txt` |
| 3 identity, canaries | **PARTIAL**: Y-F0 NAV (argv, `--spec`) and fit identity hold; ALCLOCK-1 holds; D3-Q1a DIFFERENT (P9-W lapses at 0, PRE-35; finding); COMPOSE-1a ACCEPT; D3-Q2 / D3-S1 0 / 0; golden 1 / 4 workers OK both builds; canaries 15 / 15 both builds; **X-5 NAV identity not run (fields v13 absent); X-5 u / w not run (GATE-IDX + v13)** | `nav-identity-yf0.txt`, `fit-identity-yf0.txt`, `d3q1a.txt`, `compose-1a.txt`, `d3-greps.txt`, `inputs-check.txt` |
| 4 P9-B0 | **not started: BLOCKED-OWNER** (GATE-IDX; R-2's fields v10 absent) | this file "Steps 4-6" |
| 5 rho | not reached (needs step 4) | |
| 6 re-pin | not reached; EXE-FREEZE not started | |
| 7 store step, scoreboard, G-P, CRT | **PARTIAL**: scoreboard + timings reproduce the lineage; PY-S S-6 "equal"; CRT record; catalog / G-P / X-5 opt-in cache identity not run (post-re-pin; GATE-IDX) | `scoreboard-gate.txt`, `pys-s6-ledger-n.txt`, `crt-record.txt` |
| 8 whole-wave review | DONE before the chain | `wave-review-*.md` |
| 9 records | incremental: this file, `integration-log.md`, `handoff-root-gate-2.md` | |

## Status by step (agent 1, at OWNER STOP 12)

| step | state | evidence |
|---|---|---|
| 0 start state | **holds** (re-verified) | `gate-logs/start-state.txt`, below |
| 1 exes of record | **holds**: p9-3f Release + p9-3g Debug, 27 targets each, 0 warnings, 27 / 27 + 27 / 27 `disk = receipt` | `gate-logs/mega-p9-3{f,g}-*`, `exe-sha-{rel,dbg}.txt` |
| 2 suites | **PARTIAL** (OWNER STOP 12): Release gtests 16 / 16 targets 0 failed; `scripts/tests` seed 0 on Release exit 1 (1 failed / 702 passed / 3 skipped; the one red = GATE-TMP's cause, re-run pending); S-7 store hand check holds; the rest not run | `gate-logs/gt-exits.txt`, `pyt-exits.txt`, `covdiff-s7-files.txt` |
| 3 identity, canaries | **PARTIAL**: canaries 15 / 15 on both builds (inside the builds); Release golden cases OK; the rest not reached | step 1, step 2 |
| 4 P9-B0 | not reached | |
| 5 rho | not reached | |
| 6 re-pin | not reached | |
| 7 store step, scoreboard, G-P, CRT | not reached | |
| 8 whole-wave review | DONE before this agent (WAVE-REV-EARLY, SQL4-ADD; GATE-0 committed the five files) | `wave-review-*.md`, ledger :905, :913 |
| 9 records | incremental | this file, `integration-log.md`, `handoff-root-gate*.md` |

## The rulings: state after chain agent 2 (RESUME 13; rows not listed are as in agent 1's table below)

| ruling | step | state | evidence (`gate-logs/`) |
|---|---|---|---|
| GATE-TMP | 2 | **closed**: the tool's TMP root moved to a short root OUTSIDE the work tree (the ruled `build-equity/pt/<inv>` is refused by K3 `fresh_root` / `no_git_refusal`, both pre-P9; open question for the PM); seed 0 re-run green, the single named test green | `tools/CHANGES.txt`, `pyt-exits.txt` |
| TEST-2 (owner 2026-10-16) | 2 | received after every whole suite of step 2 had completed and been logged: all stand as run; focused checks (a) single test green and (b) the three `cov-diff --legacy` runs done; no new whole-suite invocation started | this file "Chain agent 2" |
| GATE-COVDIFF | 2 | **holds**: COV-9 role check first (S9 0 of 990), then the three bounded `cov-diff --legacy` runs exit 0, "legacy equal: 1405 sessions", no store block | `cov9-role-check.txt`, `covdiff-runs.txt` |
| D3-Q1a | 3 | **DIFFERENT**: admission identical, the engine-fit weights differ from the Python fit in 694 float leaves (`status` "agree"); P9-W lapses at 0 (PRE-35); P9-F0h is produced flag-absent (Python fit) | `d3q1a.txt` |
| COMPOSE-1 (1a) | 3 | **ACCEPT** at the store-backed parse stage on the tiny world (both flags accepted together; stops at the same later stage as the rate-only control); `validate_nav_config` / `validate_nav_input` left to P9-MV's preflight | `compose-1a.txt` |
| ALCLOCK-1 | 3 | **holds**: flag-absent Y-F0 NAV identity; opt-in adds only `/scenarios/*/info_clock` leaves (5 / 5 scenarios) | `nav-identity-yf0.txt` |
| D3-Q2, D3-S1 | 3 | **hold**: 0 / 0 over `scripts/specs/p9/` | `d3-greps.txt` |
| GATE-IDX | 3, 4, 7 | file **still present** at 13:40:01Z: no run naming the cache; step 4 not started: **BLOCKED-OWNER** | `cache-lo3-listing-agent2-start.txt` |
| PY-S S-6 | 7 | **equal** (unscored 63, scored 62, each three ways) | `pys-s6-ledger-n.txt` |
| CRT-2 | 7 | recorded: stores = pins, CRT unchanged | `crt-record.txt` |
| PERM-1 | - | one refusal (the unstage of an editor-side `.mypy_cache`); not retried | `perm-refusals.txt` |

## The "ROOT GATE: rulings" list (`r11-dispatch.md`) and the dispatch rulings: state at OWNER STOP 12

| ruling | step | state | evidence |
|---|---|---|---|
| STORE-M1 (+ STORE-S S-1) | 0 | DONE at GATE-0 (`7320e1b9`) | `handoff-root-gate0.md` |
| IC-S | 0 | DONE at GATE-0: no edit; G-P5 prints "PARTIAL: four pairs unlisted, P10 retirement" | `handoff-root-gate0.md`, `gate0-logs/frozen-pairs-probe.txt` |
| five `wave-review-<area>.md` committed | 0 | DONE at GATE-0 (`22f99018`) | git |
| GATE-EXES | 1 | **holds**: `atx-research-composition` and `atx-research-admission` built Debug (p9-3g) and Release (p9-3f), receipts, in the exe table | step 1 |
| GATE-COVDIFF | 2 | **PARTIAL**: S-7 hand check holds on the three stores; the three `cov-diff --legacy` runs not run; `v8-risk-lo3-v10` cited from M2d | step 2 |
| D3-Q1a | 3 | not reached | - |
| COMPOSE-1 | 3 | not reached | - |
| PRE-MVX | 4 / cells | the prereg's lines (B.7 "Parent", B.8, E) are the PRE agent's (PRE-2BII-PAR part A); nothing for root before P9-B0 | ledger :889, :932 |
| COVMV-S2 S-b + NAV-S S-2 | 6 | not reached | - |
| G-P10 (d) / (e), STORE-S S-4 | 7 | not reached | - |
| Fields area | - | APPROVE, nothing for root; its step-2 runs not reached (fields gtests Release 110 / 110 ran) | step 2 |
| Python area | 2, 7 | its whole-suite runs: seed 0 ran (step 2), the rest not reached; SQL3-2 holds at step 0; PY-S S-6 hand comparison (step 7) not reached | step 2 |
| GATE-IDX | 3, 4, 7 | the file was present at step 0 and at the stop; nothing naming the cache ran | `handoff-root-gate-1.md` |
| GATE-PAR | 2 | builds alone; at most two low-RAM suite invocations at a time (Release gtests beside `scripts/tests` seed 0); the S-7 hashing beside them | `gate-logs/builds.txt`, `gt-exits.txt`, `pyt-exits.txt` |
| PAR-1 | all | root added only its own paths; the PRE agent's `p9-prereg.md` and the IDX agent's file are theirs | git |

## Round-2b-ii facts (Appendix IV items) delivered so far

None of items 1-6, 8, 9, 12, 15, 20, 22, 23, 25, 26 is final yet (P9-B0, the re-pin and rho are not reached). Inputs
already fixed for them: item 4's exe paths, SHA-256 and tags are step 1's table (final only when the re-pin commit
names them); item 25: the `ucrtbase.dll` value at step 0 (10.0.26100.7623 `2C08E77F...65EF`), to be re-recorded at
P9-B0 itself.

Agent 2: item 25's value re-read at its step 0 (2026-10-16): unchanged (10.0.26100.7623 `2C08E77F...65EF`); still to be
re-recorded at P9-B0 itself. Delivered as gate outcomes (not Appendix IV items): D3-Q1a DIFFERENT (P9-W lapses at 0)
and COMPOSE-1a ACCEPT. The items tied to P9-B0, rho and the re-pin stay open (step 4 not started).

## Step 0: start state (re-verification only; GATE-0 did the slips)

`gate-logs/start-state.txt` (2026-10-15T20:07Z):

- HEAD `5f48a25d` (docs; IDX's commit above `0a481348`); code head `06dc9e5a`: 0 files differ outside
  `.superpowers/`. Branch `feat/platform-p9-20261003`. `git log --format=%s -40` holds every wave-2 / wave-3 merge
  named in `r11-dispatch.md` "ROOT GATE" through M6b (12 merge subjects in the last 40, SQL4 `06dc9e5a` on top).
- `git status --porcelain`: the five untracked PNGs under `docs/plans/` (not root's, never added) plus the PM's /
  docs agents' tracked edits in the sprint dir (`progress.md`, `r12-dispatch.md`, the PRE agent's `p9-prereg.md`).
  No code path dirty.
- Trial ledger 133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
- `build-equity/research-store/` absent (SQL3-2).
- `ucrtbase.dll` (C:\Windows\System32) file version 10.0.26100.7623, SHA-256
  `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` = the M2i record of the two Release D-COV
  stores (CRT-2): unchanged. `ucrtbased.dll` 10.0.22000.832 `19E79E91...F48D` (unchanged).
- Host: free RAM 5,776 of 16,069 MB, commit free 4,966 MB; C: 72.8 GB free; no build running (editor-side python
  only).
- GATE-IDX file present (above).

## Step 1: exes of record (EXE-CUR, EXE-FREEZE, GATE-EXES, SPDLOG-RELINK)

Two target-scoped builds through `scripts/research-build.ps1` (wrapper `gate-logs/tools/build.sh`, HOST-4 / HOST-5
admission), sequential, nothing else running beside either, both with `-Canary`. The same 27 targets each: the 11
exes `atx-equity-strategy-targets` (NAV), `atx-equity-strategy-ic` (IC), `atx-equity-strategy`, `-risk`, `-mine`,
`atx-research-eval`, `atx-research-composition`, `atx-research-admission` (GATE-EXES), `atx-research-store`,
`atx-research-fields`, `atx-impl`, and the 16 test targets the suites run (`atx-impl-strategy-{ic,target,mine}-tests`,
`atx-impl-strategy-tests`, `atx-impl-tests`, `atx-engine-{alpha,factory,book,risk}-tests`,
`atx-engine-research-{fields,admission,store,catalog,composition,ledger,eval}-tests`).

| tag | preset | receipt Source (code) | exit | TUs / links | warning lines | canary (G-P8) | wall | evidence |
|---|---|---|---|---|---|---|---|---|
| **p9-3f** | `equity-rel` (Release, `build-equity-rel`) | `5f48a25d` (= code `06dc9e5a`; 8 dirty entries = the sprint-dir docs and the five PNGs, no code path) | 0 | 244 / 34 | **0** | check, Release goldens: **15 passed**, exit 0 | 526 s | `gate-logs/mega-p9-3f-{receipt.json,build.log,canary.log,cache-stats.log}` |
| **p9-3g** | `equity-dev` (Debug, `build-equity`) | `5f48a25d` (same) | 0 | 68 / 19 | **0** | check, Debug goldens: **15 passed**, exit 0 | 183 s | `gate-logs/mega-p9-3g-*`, `porcelain-before-p9-3g.txt` |

SPDLOG-RELINK: after the last link every exe of both trees was re-hashed against its newest receipt
(`gate-logs/tools/exe_sha_end.py`): **27 / 27 Release and 27 / 27 Debug `disk = receipt`** (`gate-logs/exe-sha-rel.txt`,
`exe-sha-dbg.txt`). Superseded by these builds: every earlier root exe (p9-2p .. p9-3e Debug, p9-2q / p9-2s Release).
Release is the producer of record (OD5-2, M2c-RED-2 (e)). EXE-FREEZE starts at the re-pin (step 6); it covers the NAV,
IC, eval, composition, admission and store exes (GATE-EXES).

### Exe table (tag, SHA-256; source = gate code head `06dc9e5a`, receipt Source `5f48a25d`)

| exe | Release p9-3f (`build-equity-rel/bin`) | Debug p9-3g (`build-equity/bin`) |
|---|---|---|
| atx-equity-strategy-targets (NAV) | `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565` | `5dced610e204ba860bd2c8c8da5c3c6bd58a1ef98c242bec352a6b64d8762a8f` |
| atx-equity-strategy-ic (IC) | `ccd54db605f67fef1aa480050a6c785ffe179dae2f77e978dd6d01a5c117d705` | `adffe7421781145a9718e67be05b46038b89a46311f26390eeb3312da236d56d` |
| atx-research-eval | `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c` | `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` |
| atx-research-composition | `04db0d67c1563184edd6fe8ff66796877c8db20239d5cfb423fa78e047df16f2` | `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e` |
| atx-research-admission | `f53d9277e4ad2e3915343ccca032df3f0098a1a1cd7b4d832b640f5f7437063f` | `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` |
| atx-research-store | `0eeac21bb95677d8da5268ca829b88dc668521a4c9f625e0fbbde92f96fb3818` | `a54955e965ee2aa035e66cc9ed668a16246c089684a53515431dea428b605c73` |
| atx-research-fields | `883c5b74a95731eb310cfd30cb1ab39ee102e077ab2d7318bd5db941f486584a` | `1c670d86a0fdcae6a86b0eff56a184902d1eed8b244f8af6ab31425e18185fac` |
| atx-equity-strategy | `f883f81f0fbb928cb8fe2e8b1931841e0eef058e7b6398810e10de68cb0495a4` | `47d341c2541e792873ec5d3db0b739af6de61e9962ba46667c755a1ca56ca9a4` |
| atx-equity-strategy-risk | `0c110534dd2df43e995040a07e5368eac788166b5e55cc2e0e2fa5e088be586b` | `6696da719374777aefb49ce9e6b4707187549dff9b838e0c2338d21e852b47a1` |
| atx-equity-strategy-mine | `daf889437983628ccbc375e76a078d3decf079a9d3a79b6cca79cbcc4a063eba` | `4dee20571d4c4481bcf175d8bb688574ba95c60ae29d08b4bb474849f7945412` |
| atx-impl | `71f0c6311aaffffaa48f4c221c18c94e009a3b7ebc5a45798baff009d75bbdb9` | `7095d4477dd7fee80ead2d9bbdb516a726ed3ab943beed95ac1dc5335af63e55` |
| atx-impl-strategy-ic-tests | `9e979065f2b56f135c819baab9da80c89e5745b6ba249c284c428d3497d0fb5b` | `55db8666c03533439ecadd10d9412f4c65167581e0a11a806de2572ea69ebd6b` |
| atx-impl-strategy-target-tests | `5e781705c1fab53c42cb49d0777753a1f792ff022847923f4dce901411cd6db5` | `18f3855468ca76c083eae9fabdb8d0cd1a237996ac8bba3136ed06782eb74318` |
| atx-impl-strategy-mine-tests | `2ae349c689f9423f2847ffbb63a3ad9598b4c26aab0ca63136dc16a83019fa68` | `7502d0fb39e125a9cce9feabab4c35a8da5681a37eacad974b793c555822abb0` |
| atx-impl-strategy-tests | `0ae6ab3f4a09af6a33d4dec3f6eba8fe8324e7c3ea85aaae6f745cb5e03712e1` | `6dc6d4426370e256dc1e1331292e84da180a292cb72c86456c70eb9255246fba` |
| atx-impl-tests | `9f4cf8076d17b93891a70fb5944e3f43975230a84bc0a82a1e8fb46d90e0b545` | `20f3e2378ea4373e4812ba6c1d5bf1257945b2c5a9488cabeb6df04071f523db` |
| atx-engine-alpha-tests | `534efe4928e84ae3f9fcc9f0ed79b7e823b51689987edd84c513b6ba7917bf56` | `3f8f1c7ce851998d3b2e515cdcbc9ae1634a61550ac2d975a3828850356c9637` |
| atx-engine-factory-tests | `6bf91318724f77c0413dfaada365708d4360fbc754320f05a9e074655575483c` | `a9d538ae4d922afa26c78da60e7114b6cabcf78dc32c3274d7f698cf339354d6` |
| atx-engine-book-tests | `88f195dba54faa428eed45b4d53cf64d7e8f8a28702c7f3b3d68dd80ab4af0ac` | `6977a5a75b386d283c81e148c0fec31a2da1f95536dc98cab2366d424d1f8360` |
| atx-engine-risk-tests | `b654425d22fc671b3b24a7dbbb35ccad4288b655afb8d1925853362ca624e99d` | `0008934517be7b3a36eede7eae65fde92cab50dab10b9835af585186e26bbbb1` |
| atx-engine-research-fields-tests | `165ebeea3d7f01fb330856240c12d125b18c0a491bac66183ab9a229c49b3651` | `a037964fc902f143541d204624e9bd31e0249c7e3a216bd9dac325ab1d47ef7d` |
| atx-engine-research-admission-tests | `39c4a81bda54df9712e2e818684cb78293486e7d664dec8b308883c505da1581` | `310aa78298b1bbb75c5d6104f1af581aa962c909cccfdd846d5b6628e8200a62` |
| atx-engine-research-store-tests | `bf9c312941fc5d8668bb7024c476f915138679fb26f3f3fa21fd88e94d953d26` | `d2330e4b387b832545ada353706c62cb41c02981bff61728365ac7329713661f` |
| atx-engine-research-catalog-tests | `c996b733b5b92f58bee1760a0c7505ec6349c3847bb392d5014e762b58dfefc5` | `b73b2199db7fd1bc451bd12872a5eb0851453db4299ed29e812dbbd151d9835a` |
| atx-engine-research-composition-tests | `17cee392663a5df0432e3293bec06bb5126f6931672b0d827cdaa0e336a731f6` | `76b664b7483a77a717015a3fdebb38f97dc39fbcfecd04a98e52d3cd016023a9` |
| atx-engine-research-ledger-tests | `bfe2afc39770a316810ea5f33650e6d15f66ad013374b123ea16e42f8865bf94` | `8c5a6065f31f061fe6a06b474b0bd9987f45a640c5f59a9ade4b61b25fe322cd` |
| atx-engine-research-eval-tests | `2b9910b3665798c6d93cca37998580db5942cc7284b88cfef4b9830b946a4a02` | `58b5681bc8373e9a1ddf0a84602d5502acc01070708a09ddc52d747106ec3557` |

Several Debug exes have the same bytes as their last root build (the IC exe `adffe742` = p9-3e, the eval verb
`4791019` = p9-2z, composition / admission = p9-3a, book tests = p9-2y): ninja relinked nothing whose inputs did not
change; the receipts name them under p9-3g regardless.

## Step 2: suites (PARTIAL at OWNER STOP 12; plan 4.2 "suites", GATE-COVDIFF, GATE-PAR)

### Release gtests, whole targets (p9-3f; `gate-logs/tools/suite_gt.sh rel`; ENV-1 0 hits; `gate-logs/gt-exits.txt`, `gt-rel-<target>.log`)

| target | ran | passed | skipped (names) | failed | disabled |
|---|---|---|---|---|---|
| atx-impl-strategy-ic-tests | 261 | 261 | 0 | 0 | 0 |
| atx-impl-strategy-target-tests (incl. the `Cov*`, `MvAimRule.*`, `InfoClock*` suites) | 421 | 421 | 0 | 0 | 1 |
| atx-impl-strategy-tests | 46 | 46 | 0 | 0 | 0 |
| atx-impl-strategy-mine-tests | 44 | 44 | 0 | 0 | 0 |
| atx-impl-tests | 1,272 | 1,267 | 5: `Alpha101Orats.RankByOptimizerSharpe`, `Alpha101Orats.RankBySharpeRiskParity`, `AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha`, `FundamentalZoo.RealDataIcReport`, `SingleAlphaCapacity.SweepAndVerify` (the same five as M2j's Release / Debug whole runs) | 0 | 0 |
| atx-engine-alpha-tests | 782 | 780 | 2: **`AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`** (OD5-2 named skip), `AlphaVm_ZeroAlloc.WarmDispatchLoop_BoundedByOutputsAndSizeIndependent` (Release-only skip, the same as S2R's Release run `m2f-logs/s2r-rel-alpha-whole.log`) | 0 | 1 |
| atx-engine-factory-tests | 396 | 396 | 0 | 0 | 0 |
| atx-engine-book-tests | 197 | 197 | 0 | 0 | 0 |
| atx-engine-risk-tests `Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` (incl. `RiskCovContainer.*`) | 432 | 430 | 2: `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`, `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read` (the same two as M2i's Release run) | 0 | 0 |
| atx-engine-research-fields-tests | 110 | 110 | 0 | 0 | 0 |
| atx-engine-research-admission-tests | 16 | 16 | 0 | 0 | 0 |
| atx-engine-research-store-tests | 27 | 27 | 0 | 0 | 0 |
| atx-engine-research-catalog-tests | 30 | 30 | 0 | 0 | 0 |
| atx-engine-research-composition-tests | 21 | 21 | 0 | 0 | 0 |
| atx-engine-research-ledger-tests | 16 | 16 | 0 | 0 | 0 |
| atx-engine-research-eval-tests | 27 | 27 | 0 | 0 | 0 |

**0 failed in 4,098 Release tests; every skip a pre-existing named one.** Golden `0x889874a3b9b29c55` (step 3 item,
recorded here): Release `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest` and
`...ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (workers 1 and 4; mine-tests) and factory-tests
`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `NsgaSearch.ScalarRaw_DigestInvariantAcrossWorkers` (1, 2, 4, 8),
`NsgaSearch.DeflateSelection_DefaultIsOff_ByteIdentical` all OK (the M1d form). Debug side not run yet.

### Pytest (Release exes, `gate-logs/tools/pyt.sh` with `GATE_BUILD=rel`)

| invocation | seed | result | log |
|---|---|---|---|
| `scripts/tests` (whole) | 0 | **exit 1**: 1 failed, 702 passed, 3 skipped; wall 36,486 s from 2026-10-15T20:20:25Z (spans OWNER STOP 12); ENV-1 0 hits. The one red: `test_cycle_e2e.py::test_cycle_e2e_goldens_ic_reference_redundant_copy_and_idempotent_rerun`, `research_cycle.py` exit 2 at plan time: "IC cache path too long (ruling W1-I1): ... up to 327 characters (> 250)" under the tool's TMP root in the session scratchpad = GATE-TMP's cause; re-run pending (GATE-TMP) | `gate-logs/pyt-scripts-tests-rel-s0.log`, `pyt-exits.txt` |

### GATE-COVDIFF

- S-7 hand check (read-only, `gate-logs/tools/store_files_check.py`, `gate-logs/covdiff-s7-files.txt`): **holds** on
  all three container stores: `p9-m2i-dcov-v11` (manifest `397366473b3a...` = the M2i pin; container
  `model.atxcov` `cd48d574...` root `b39a1b77...`), `p9-m2i-dcov-covv1` (`ea58691fc407...` = pin; `0f90bf7c...` root
  `20919f7b...`), `p9-m2d-cov-risk-container` (`9c12852ea0cd...` = the pin the M2i Y-1 NAV receipt names;
  `0a04b233...` root `5462e923...`): every one of the 11 listed files and the container equal the manifest's bytes
  and SHA-256; no unlisted file.
- The three `cov-diff --legacy` bounded runs: **not run** (OWNER STOP 12 came first). `v8-risk-lo3-v10`'s identity
  with its container is cited, not re-run (as ruled): `handoff-root-m2d.md:23`, `m2d-logs/cov-diff-legacy.log`.

### Not run (OWNER STOP 12)

Debug gtests whole; `ctest -L atx_research` / `-L atx_equity_strategy` on both trees; `scripts/tests` seed 1; the stage
chain; `atx-engine/tools`; `atx-impl/tools`; `atx-impl/strategies`; the fields fixture pytests; the mirror guard alone;
the three cov-diff runs. Commands: `handoff-root-gate-1.md` "Exact next step".

---

## Chain agent 2 (RESUME 13, 2026-10-16; `r13-prompts/gate-prompt-2.md`; rulings R13-RESUME, GATE-TMP, GATE-RELCACHE, COMPOSE-1a, HOST-6)

Started from `handoff-root-gate-1.md`. First act (R13-RESUME, DIRTY-1 form): agent 1's five placeholders (and the
sixth, `@@PY_S0_LOG@@` in its `integration-log.md` row) filled from `gate-logs/gt-exits.txt` / `pyt-exits.txt` only;
agent 1's records and the PM's files committed as they stood: docs commit `0c8bc8da`.

### Step 0 re-verification (agent 2; `gate-logs/start-state-2.txt`, 2026-10-16T12:36:59Z)

- HEAD `0c8bc8da` (docs); code head `06dc9e5a`: `git diff --name-only 06dc9e5a..HEAD` outside `.superpowers/` = 0
  files. `git status --porcelain` = the five untracked PNGs only. First-parent merges down from `06dc9e5a`: SQL4,
  SQL3 item 5, AL-CLOCK fix 1, A4 pre-flip, SIGN x3, D3W-PB x3, OD3P follow-up, B2 fix 2, COV-MV, AL-CLOCK (14 listed).
- Trial ledger 133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
- `build-equity/research-store/` absent (SQL3-2).
- GATE-IDX file `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` **still present** (73,728 bytes, sha256
  `3a177dfd...c6f0`).
- `ucrtbase.dll` 10.0.26100.7623 `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` (= CRT-2 record,
  unchanged); `ucrtbased.dll` 10.0.22000.832 `19E79E91...F48D` (unchanged).
- Host: free RAM 4,965 of 16,069 MB, commit free 9,034 MB, C: 67.8 GB free; our build processes 0; foreign: the
  atx-vol sessions of pools 33 / 34 (HOST-6: a `cmake --build C:\atx-wt\pool-34\build-vol-rel-avx2` and a pool-33
  quote benchmark started during the suites; never touched).
- SPDLOG-RELINK re-hash (`gate-logs/tools/exe_sha_end.py`): **27 / 27 Release (p9-3f) and 27 / 27 Debug (p9-3g)
  `disk = receipt`**: the exe table of step 1 stands, no re-receipt needed; next free tag `p9-3h` unused.

### GATE-TMP: the tool change and where the short root sits (`gate-logs/tools/CHANGES.txt`)

`gate-logs/tools/pyt.sh` now takes a short invocation name and puts the basetemp (`<root>/b`) AND TMP / TEMP of every
invocation under ONE fresh short root, deleted by literal name after its record (the one logged change). The ruled
root `C:/atx-wt/pool-2/build-equity/pt/<invocation>` lies **inside pool-2's git work tree**, where the code under test
refuses a tiny-world root by design: `scripts/tests/test_cycle_e2e.py:212-217` (`fresh_root`: "temporary root ... lies
inside the repository") and `scripts/research_tree.py:75-81` (`no_git_refusal`: `research_cycle.py --no-git`, contract
K3, only for a root outside any git repository; the `--no-git` tests root tiny worlds under the basetemp too). Probe on
the ruled root, the one e2e node (`GATE_PT=ruled`): **1 failed**, `RuntimeError: temporary root
C:\atx-wt\pool-2\build-equity\pt\pr\t\tiny-world-luarx0j_ lies inside the repository`
(`gate-logs/pyt-tmp-probe-ruled-root-s0.log`; probe root deleted by literal name). Decision within GATE-TMP's purpose
(a short TMP root so W1-I1 does not refuse; the slip is the tool's, not the code's): every run of record uses the
short root `C:/Users/natha/AppData/Local/Temp/g2<inv>` (the OS temp dir, outside any work tree, where pytest and
`tempfile` put these tiny worlds when nothing overrides TMP, as at every ROOT-1 run before agent 1): deepest tiny-world
IC cache path 247 <= 250 (agent 1's refusal: 327 with a 138-character root; the tail is 189).

### Step 2, agent 2: `scripts/tests` whole, seeds 0 (again) and 1 (GATE-TMP; Release exes, `GATE_BUILD=rel`)

Run side by side (GATE-PAR: separate roots `g2s0` / `g2s1`), beside the Debug gtests; head `0c8bc8da` (code
`06dc9e5a`), code-dirty none, ENV-1 0 hits each (`gate-logs/pyt-exits.txt`).

| invocation | seed | result | wall | log |
|---|---|---|---|---|
| `scripts/tests` (whole), agent 1's run (TMP under the session scratchpad) | 0 | exit 1: 1 failed (GATE-TMP's cause), 702 passed, 3 skipped | 36,486 s (spans OWNER STOP 12; not a timing finding) | `pyt-scripts-tests-rel-s0.log` |
| probe: the e2e node alone on the ruled root inside the work tree | 0 | 1 failed: `fresh_root` "lies inside the repository" (the test's own guard; see GATE-TMP above) | 1 s | `pyt-tmp-probe-ruled-root-s0.log` |
| `scripts/tests` (whole), re-run on the short root `g2s0` | 0 | **exit 0: 703 passed, 3 skipped, 0 failed**; `test_cycle_e2e.py` 15 / 15 incl. the GATE-TMP case | 1,098 s | `pyt-scripts-tests-rel-s0-rerun.log` |
| `scripts/tests` (whole), short root `g2s1` | 1 | **exit 0: 703 passed, 3 skipped, 0 failed** | 1,096 s | `pyt-scripts-tests-rel-s1.log` |

GATE-TMP closes: the one red of agent 1's run was the tool's TMP root; on the short root the case passes at both
seeds. No red to attribute. The 3 skips are all in `scripts/tests/test_research_cycle.py`, opt-in live-root cases
(a `-rs` run of that file, 93 passed / 3 skipped, `gate-logs/pyt-research-cycle-rs-rel-s0.log`): `:56`, `:706`, `:946`
"set RESEARCH_CYCLE_LIVE_ROOT to hash the real pool-2 files" (the same class as M2j's 4 skips; named, pre-existing).

### Step 2, agent 2: the other pytest invocations (each its own; Release exes; seed 0; `gate-logs/pyt-exits.txt`)

Two chains side by side (`tools/pyt_batch.sh A|B`, GATE-PAR), beside the Debug gtests; head `aef2e033` (the PRE
agent's pathspec commit of `p9-prereg.md` above root's `0c8bc8da`; code `06dc9e5a`), code-dirty none, ENV-1 0 hits.

| invocation | result | log |
|---|---|---|
| `atx-engine/tools/test_stage_chain.py` alone | **10 passed** | `pyt-stage-chain-rel-s0.log` |
| `atx-engine/tools` (the other 42 `test_*.py`, explicit paths; without the stage chain and the two fields files below) | **391 passed** | `pyt-engine-tools-rel-s0.log` |
| `scripts/tests/test_no_python_mirror.py` alone (IC review item 2) | **7 passed**; "G-P5: 0 mirrored rule function(s) in 0 row(s) left for P9 lanes" | `pyt-mirror-guard-rel-s0.log` |
| `atx-impl/strategies` | **14 passed** | `pyt-impl-strategies-rel-s0.log` |
| fields: the holdings pair (`research_fields_holdings/test_holdings_{engine_path,fixture}.py`) | **4 passed** | `pyt-fields-holdings-pair-rel-s0.log` |
| fields: the four `research_fields/test_*.py` | **26 passed** | `pyt-fields-four-rel-s0.log` |
| fields: `atx-engine/tools/test_research_fields_holdings.py` alone | **16 passed** | `pyt-fields-tools-holdings-rel-s0.log` |
| fields: `atx-engine/tools/test_field_registry.py` alone | **37 passed** | `pyt-field-registry-rel-s0.log` |
| `atx-impl/tools` (g2 TMP) | exit 1: **1 failed**, 769 passed: `test_mega_report_v8.py::test_v7_config_renders_the_pre_v8_bytes`, `assert 109873 == 109868` | `pyt-impl-tools-rel-s0.log` |

The `atx-impl/tools` red is the gate tool's again, not the code's: that test asserts the rendered html's LENGTH
(`V7_GOLDEN_BYTES` 109868) before it normalises the `tempfile.mkdtemp` root out of the html, so it holds only for a TMP
of exactly 33 characters (the OS default `C:\Users\natha\AppData\Local\Temp`); the g2 TMP is 38 characters: +5, the
difference. The node alone (no load): on the g2 TMP 1 failed (109873, deterministic); on the OS TMP 1 passed
(`pyt-mega-v7-node-{g2tmp,systmp}-s0.log`; `tools/CHANGES.txt` second entry, mode `GATE_PT=systmp`). Not attributed
to a merge; P10 note (the test should normalise before it measures). The whole `atx-impl/tools` run of record is the
re-run alone under `GATE_PT=systmp`:

| invocation | result | log |
|---|---|---|
| `atx-impl/tools` (whole), alone, basetemp `g2i2`, TMP = the OS temp dir | **exit 0: 770 passed, 0 skipped, 0 failed** (the fitter `RealExecutables` and engine-fit real-exe cases ran on the Release admission / composition exes: 0 skipped; the n37 test on the Release verb passed, B2-N37 item 4) | `pyt-impl-tools-systmp-rel-s0.log` |

### Step 2, agent 2: Debug gtests, whole targets (p9-3g; `tools/suite_gt.sh dbg`; `gate-logs/gt-exits.txt`, `gt-dbg-<target>.log`)

Every exe re-hashed `disk = receipt` before the run (step 0 above); ENV-1 0 hits.

| target | ran | passed | skipped (names) | failed | disabled |
|---|---|---|---|---|---|
| atx-impl-strategy-ic-tests | 261 | 261 | 0 | 0 | 0 |
| atx-impl-strategy-target-tests | 421 | 421 | 0 | 0 | 1 |
| atx-impl-strategy-tests | 46 | 46 | 0 | 0 | 0 |
| atx-impl-strategy-mine-tests | 44 | 44 | 0 | 0 | 0 |
| atx-impl-tests | 1,272 | 1,267 | 5 (the same five real-data cases as Release) | 0 | 0 |
| atx-engine-alpha-tests | 783 | 783 | 0 (Debug runs `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`, the Debug-CRT golden, and `AlphaVm_ZeroAlloc.*`: both OK) | 0 | 1 |
| atx-engine-factory-tests | 396 | 396 | 0 | 0 | 0 |
| atx-engine-book-tests | 197 | 197 | 0 | 0 | 0 |
| atx-engine-risk-tests `Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` | 432 | 431 | 1: `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery` | 0 | 0 |
| atx-engine-research-fields / admission / store / catalog / composition / ledger / eval tests | 110 / 16 / 27 / 30 / 21 / 16 / 27 | all | 0 | 0 | 0 |

**Debug: 16 / 16 targets exit 0; 4,099 run, 0 failed, 6 skipped (all named, pre-existing).** Golden
`0x889874a3b9b29c55` (`kGoldenDigest`, `atx-engine/tests/factory/factory_nsga_search_test.cpp:77`,
`factory_signal_fitness_test.cpp:79`): `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest`,
`...ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (1 and 4 workers), `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
`NsgaSearch.ScalarRaw_DigestInvariantAcrossWorkers`, `NsgaSearch.DeflateSelection_DefaultIsOff_ByteIdentical`: **OK
on Debug and on Release** (step 3's golden item holds).

### Step 2, agent 2: `ctest -L` through the wrapper (`tools/ctest_label.sh`; `scripts/atx-build.ps1 -Ctest -Jobs 1`; `gate-logs/ctest-exits.txt`)

| tree | label | result | log |
|---|---|---|---|
| Release `build-equity-rel` | `atx_research` | 100% passed, 0 failed out of 291 | `ctest-rel-atx_research.log` |
| Release | `atx_equity_strategy` | 100% passed, 0 failed out of 728; 1 "Not Run (Disabled)": `atx-impl-strategy-target-tests.CovStageDigest.DumpPerStageOnTheLaneFixture` (the target's one DISABLED test) | `ctest-rel-atx_equity_strategy.log` |
| Debug `build-equity` | `atx_research` | 100% passed, 0 failed out of 291 | `ctest-dbg-atx_research.log` |
| Debug | `atx_equity_strategy` | 100% passed, 0 failed out of 728; the same 1 disabled | `ctest-dbg-atx_equity_strategy.log` |

G-P4 (CTest registration): both labels register and run on both trees (291 / 728 tests).

### TEST-2 (owner 2026-10-16; ledger :972-973; the PM's message to this agent) and what it changes here

TEST-2 cuts every remaining whole-suite item of step 2. It reached this agent AFTER every item of its cut list had
already completed and been logged (Debug gtests whole, the four `ctest -L` runs, `scripts/tests` seed 1 and the
seed-0 re-run, the stage chain, `atx-engine/tools`, `atx-impl/tools` (and its re-run), `atx-impl/strategies`, the
fields pytests, the mirror guard alone; the times are in `gt-exits.txt`, `ctest-exits.txt`, `pyt-exits.txt`, last
line 13:25Z). Per the PM's message ("anything of step 2 you have ALREADY completed and logged before reading this:
keep it in the report as run; stop nothing mid-invocation, start NO new whole-suite invocation") they stand above as
run; no whole-suite invocation was started after it. Cut list items not run at the gate: **none left** (all had run).

Focused check (a), GATE-TMP's one red as the SINGLE test, Release exes, seed 0:

| root | basetemp / TMP | result | log |
|---|---|---|---|
| the ruled short root `C:/atx-wt/pool-2/build-equity/pt/pr` (inside pool-2's work tree) | `pt/pr/b` / `pt/pr/t` | 1 failed at the test's own setup: `fresh_root` "temporary root ... lies inside the repository" | `pyt-tmp-probe-ruled-root-s0.log` |
| the short root outside the work tree `C:/Users/natha/AppData/Local/Temp/g2e1` | `g2e1/b` / `g2e1` | **1 passed** (5.9 s) | `pyt-gatetmp-single-g2-s0.log` |

ROOT-1 attribution of the ruled-root failure: the refusing lines are `scripts/tests/test_cycle_e2e.py:212-217`
(`fresh_root`, commit `d6083b33`, 2026-09-29) and `scripts/research_tree.py:75-81` (`no_git_refusal`, `fa61c67f`,
2026-09-29), both ancestors of the wave-1 gate head `3c9eeb01`: no wave-2 / wave-3 merge commit touches them, so no
lane is attributable; it is the test's v8-era precondition (contract K3: a tiny-world root outside any git
repository), which no root inside `C:/atx-wt/pool-2/` can meet. **Verdict (a): the seed-0 record is green with the
cause stated** (the GATE-TMP red was the tool's TMP root; the case passes alone on a short root outside the work
tree and inside both whole runs above). Open question for the PM: GATE-TMP / TEST-2 name a root inside the work tree;
this agent used the nearest root that the test admits (stated, not a code change).

Focused check (b), GATE-COVDIFF: the COV-9 role check first (`gate-logs/cov9-role-check.txt`, 13:26:52Z: role
manifest `e1c67101...95f4`, `universe.delisting.returns_applied = false`, 0 of 990 events true: **S9 HOLDS**), then
`atx-equity-strategy-risk cov-diff --legacy <store>` on the step-1 Release exe (p9-3f `0c110534...586b`, = receipt
before and after) through the bounded runner (`--seconds 600 --max-rss-mib 2048 --build-type Release`; alone on the
host for our side; `tools/covdiff_run.sh`; `gate-logs/covdiff-runs.txt`, `covdiff-<name>-stdout.log`,
`receipt-p9-gate-covdiff-<name>.json`):

| store | runner | verb exit | the verb's line | `store` block (SQL3-2) |
|---|---|---|---|---|
| `build-equity/p9-m2i-dcov-v11` | completed, 1.3 s, peak 501 MiB | **0** | `legacy equal: 1405 sessions factor_covariance.f64 specific_variance.f64 style_exposures.f32 industry_slot.u8` | none (receipt, start) |
| `build-equity/p9-m2i-dcov-covv1` | completed, 1.3 s, peak 501 MiB | **0** | the same line | none |
| `build-equity/p9-m2d-cov-risk-container` | completed, 1.6 s, peak 501 MiB | **0** | the same line | none |

With agent 1's S-7 hand check (every manifest `files` entry and the container equal the bytes on disk; manifests =
pins) and `v8-risk-lo3-v10` cited from M2d (`handoff-root-m2d.md:23`, `m2d-logs/cov-diff-legacy.log`: "legacy equal:
1405 sessions ...", exit 0; not re-run, as ruled), **GATE-COVDIFF holds**. Focused check (c): D3-Q1a's and
COMPOSE-1a's own evidence is their runs (step 3); no extra gtest filter was needed.

**Step 2 state: HOLDS.** 0 failed in every suite of record (Release and Debug gtests whole, both ctest labels on both
trees, `scripts/tests` seeds 0 and 1, the stage chain, `atx-engine/tools`, `atx-impl/tools`, `atx-impl/strategies`, the
fields pytests, the mirror guard), named skips only; the two reds seen were the gate tool's TMP root (GATE-TMP), each
shown so; GATE-COVDIFF holds. Known-red set EMPTY.

### Step 3, agent 2: identity (SHA-256 and JSON path only; no statistic read)

**Reference inputs missing on disk (a finding for the owner / PM; `gate-logs/inputs-check.txt`, `tools/inputs_check.py`,
names only):** the fields directories `build-equity/train-2020-2023-lo3-fields-v10/` and `-fields-v13/` no longer
exist (their builders' receipts `-v10-run/`, `-v13-run/` remain; `-v9`, `-v14`, `-v15a` are gone too; `-v11` and `-v15`
remain). `disk-audit.md:135` (2026-10-03) lists v9 / v10 / v13 / v14 / v15 / v15a as one hard-linked family ("v13 is the
P6 pin `e5f7f28c...` that X-5 and D-1 read"); M6b's X-5 runs read v13 on 2026-10-15 ~19:30Z, so the directories went
after that; nothing in this agent's or agent 1's records deletes them, and no ledger line records it. Missing inputs of
gate runs: X-5's NAV reference argv (`p9-c1-s2-x5-nav-run`: `--fields .../fields-v13/manifest.json`), X-5's u / w
(`p9-m6b-x5-{u,w}-post-run`: `--train-fields .../fields-v13`), **R-2's NAV argv** (`mega-nav-v8-r1-std-...-L1.1474-v80-run`:
`--fields .../fields-v10/manifest.json`), and the build argv of the three container stores (`p9-m2i-dcov-v11`,
`-covv1`, `p9-m2d-cov-risk-container`: `--fields .../fields-v10/manifest.json`; the stores themselves are pinned bytes
and read fine: GATE-COVDIFF above; B8A-PROD's step 2 in P9-V's cell re-runs that build argv, so it needs v10). Every
Y-F0 / Y-1 / X-10 input (fields v15) is present. Not recreated (a fields build reads atx-db inputs: A4-DB / OD3-DB).

| item | runs (bounded runner, `--seconds 600 --max-rss-mib 2048`, Release p9-3f exes; `gate-logs/nav-runs.txt`, `d3q1a.txt`) | result | evidence |
|---|---|---|---|
| X-5 NAV flag-absent identity, argv and `--spec` (C3-6 / M2J-2 reference `p9-c1-s2-x5-nav`) | `p9-gate-x5-nav-{argv,spec}` | **not run: refused before any work**, exit 1 `InvalidArgument: nav replay: fields manifest missing/oversized` (fields v13 absent); nothing written | `nav-runs.txt`, `receipt-p9-gate-x5-nav-*.json` |
| Y-F0 NAV flag-absent identity, argv (reference `p9-b0-yf0-nav`: the wave-1 P9-B0's Debug exe `407c34ad...`) | `p9-gate-yf0-nav-argv`, 22 s, peak 583 MiB, no `store` block | **holds**: 26 / 27 files byte-identical; `summary.json` differs only at `/producer/build_type`, `/producer/engine_git_sha` | `nav-identity-yf0.txt` |
| Y-F0 NAV flag-absent identity, `--spec` (NavSpec printed by the Release exe: `yf0.navspec.json` `a793e183...`) | `p9-gate-yf0-nav-spec` | **holds**: the same 26 / 27, the same two `/producer` leaves | `nav-identity-yf0.txt` |
| ALCLOCK-1 opt-in (`--info-clock-print` on Y-F0's argv) | `p9-gate-yf0-nav-clock` | **holds**: same 27 files; only `summary.json` differs, 70 leaves all under `/scenarios/<i>/info_clock` (5 of 5 scenarios), opt-in side only; minus those blocks = the flag-absent document | `nav-identity-yf0.txt`, `tools/clock_optin_cmp.py` |
| Y-F0 fit flag-absent identity (reference `p9-b0-yf0-fit`; the fit receipt's argv, `--output` only changed) | `p9-gate-yf0-fit-py`, 1.6 s | **holds outside the code-identity stamps**: `admission.csv` byte-identical; `admission.json` only `/inputs/script_sha256` (reference only) -> `/inputs/producer_fingerprint` (new only); `composition_weights.json` only `/provenance/{admission_sha256, script_sha256 -> producer_fingerprint, std/module_sha256 -> std/producer_fingerprint, theme_erc/module_sha256 -> theme_erc/producer_fingerprint}`; attributed by merge commit to `61378337` (D2: `d61f73c2` replaced the script SHA stamps, CM-5, as M2d's D2 root item (iii) recorded) | `fit-identity-yf0.txt` |
| D3-Q1a engine-fit identity (D3-Q1, D3-Q1a) | signals index `p9-gate-yf0-sig` (67 candidates, roster order, payloads of the real cache's Debug entries; `signals.json` `410900f5...`), Release `factors` -> `p9-gate-yf0-factors` (1004 decisions x 67, 0 refused; manifest `21b9f198...41e1`), `--engine-fit --engine-factors <it> --admission-exe <Release f53d9277...> --composition-exe <Release 04db0d67...>` -> `p9-gate-yf0-fit-eng` | **DIFFERENT**: `admission.json` = (`2d5abfd7...`) and `admission.csv` = (`c3933116...`) byte-identical; `composition_weights.json` minus `$.provenance.engine_fit` (canonical JSON): Python `f2454589...b876` vs engine `decc2f8c...55a8`: 694 float leaves differ (0 non-float; no key on one side only): `/weights/*` (47), `/provenance/candidates/*/weight`, `/provenance/themes/*`, `/provenance/theme_erc/*` (covariance, ERC shares / risk contributions, sleeve vols / correlations, weights before cap), `/theme_standardise/theme_erc/covariance/matrix/*/*`, `/provenance/blend_in_sample_TRAIN_diagnostic/factor_mean`, `/provenance/weighted_standalone_turnover`; `provenance.engine_fit.status` = "agree", `fit_max_rel_float_diff` **non-zero** (within P12; value not read) | `d3q1a.txt`, `receipt-p9-gate-yf0-*.json`, `tools/d3q1a.py` |

**D3-Q1a outcome (ruled before the run, ledger :726, :898): P9-W LAPSES at 0 trials (PRE-35); P9-F0h is produced
flag-absent (the Python fit) at P9-B0.** The FINDING for the freeze report: on the real Y-F0 TRAIN fit the C++
theme-ERC fit agrees with numpy within ruling P12 but not bit for bit (the risk `wave-review-ic.md` section 4 stated
before the run: `fit.hpp:23-31`, BLAS `np.dot` vs the C++ sums), and `--engine-fit` writes the C++ values, so every
weight moves in its last bits; no re-tuning, no second attempt. The admission screen itself is identical.

Other step-3 items: COMPOSE-1a ACCEPT (above); D3-Q2 / D3-S1 greps 0 / 0 (`gate-logs/d3-greps.txt`); golden at 1 and
4 workers OK on both builds; canaries 15 / 15 on both builds (step 1). **X-5 u / w identity and every other run naming
`--candidate-cache build-equity/mega-candidate-cache-v8-lo3`: not run** (GATE-IDX: `index.sqlite` still present,
`gate-logs/cache-lo3-listing-agent2-start.txt`: 352 files, `fb444f7d316a1a4c`; without that row 351 files =
`155872de50833fca`); X-5's u / w would in any case need the absent fields v13. **Step 3 state: PARTIAL** (Y-F0 identity,
ALCLOCK-1, D3-Q1a, COMPOSE-1a, greps, golden, canaries done; X-5 identity blocked by missing inputs; u / w identity
blocked by GATE-IDX).

### Steps 4-6: not started (GATE-IDX; missing R-2 input)

Immediately before step 4 (13:40:01Z) `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` was STILL present, so
no run naming `--candidate-cache build-equity/mega-candidate-cache-v8-lo3` was started (Y-F0's re-based u / w are P9-B0
runs): **BLOCKED-OWNER** (GATE-IDX). P9-B0 is one run set with its substitution list committed first; it was not
started piecemeal (its R-2 NAV would in any case refuse: fields v10 absent, above). No substitution list was written
(it depends on the PM's ruling on R-2's missing input). Steps 5 (rho) and 6 (re-pin; EXE-FREEZE) follow step 4.

### Step 7, agent 2: the parts that name neither the cache nor a P9-B0 output

| item | result | evidence |
|---|---|---|
| `research_cycle.py scoreboard --timings` | exit 0; ledger `build-equity/trials.jsonl` 133 lines, head `5a3ef9d9...7503`, N 62 (chain verified); lineage `(parent) v8x3b -> y-s v8ysb ACCEPTED`, 0 branches; waves y-s ACCEPTED, y-2 / y-3 / y-5 NOT ACCEPTED; checks 4 / 4 (each ledgered, `s2_net_sr` equal): **reproduces the accepted lineage** (= the wave-1 record); timings table below (wall seconds; v8's waves read through the P9 scoreboard; no P9 wave exists yet) | `gate-logs/scoreboard-gate.txt` (non-statistic fields only; the console and JSON were deleted from the scratchpad unread beyond those fields) |
| PY-S S-6: the Python `ledger_n` vs the eval verb's N, by hand | **equal**: unscored py 63 = `house_v1.n` 63 = `tot.v8.n` 63 (`n_rule` "+ 1"); scored (`--trial c0=0ad7ea4c5122ebe3`) py 62 = 62 = 62 (no "+ 1"); Release eval verb p9-3f `e2b48c46...962c`; synthetic net series (300 placeholder rows), no dsr / sr0 / variance field read | `gate-logs/pys-s6-ledger-n.txt` |
| CRT record (CRT-2) | the two D-COV stores: receipts completed exit 0, Release, exe `ccc73cd1...d811` (p9-2o), manifests = the M2i pins; `ucrtbase.dll` unchanged since their build; never rebuilt | `gate-logs/crt-record.txt` |
| catalog (M2A-4), `verify --pins`, render identity, second catalog, X-5 opt-in cache identity, G-P ticks | **not run**: the catalog walks `build-equity/` (it would index the GATE-IDX file and could not be reproduced after its deletion) and belongs on the post-re-pin tree; the X-5 opt-in cache identity names the cache (GATE-IDX) and needs the absent fields v13 | - |

`scoreboard --timings` (v8 waves, wall seconds by phase; runs), as read at the gate:

| wave | card | fit | monitor | nav | ref | summ | u | w | total |
|---|---|---|---|---|---|---|---|---|---|
| y-2 | 18.2 (1) | 0.8 (1) | 1.6 (1) | 90.9 (2) | - | 34.4 (1) | 12.7 (1) | 42.2 (1) | 200.7 |
| y-3 | 24.5 (1) | 1.9 (1) | 1.6 (1) | 105.5 (2) | - | 37.2 (1) | 12.7 (1) | 52.8 (1) | 236.2 |
| y-5 | 18.8 (1) | 1.0 (1) | 1.6 (1) | 161.6 (2) | - | 37.7 (1) | 12.7 (1) | 62.2 (1) | 295.5 |
| y-s | 24.5 (1) | 1.9 (1) | 1.6 (1) | 115.3 (2) | 61.9 (1) | 40.0 (1) | 12.7 (1) | 52.8 (1) | 310.7 |

Gate walls of this agent beside them (Release p9-3f, our host quiet but for the HOST-6 vol sessions): Y-F0 NAV 21-22 s
(the wave-1 P9-B0 Debug NAV: 79-90 s); Y-F0 `factors` 24.8 s; the fitter 1.6 / 1.9 s (cached WorkStore records,
67 reused).

### Step 3, agent 2: COMPOSE-1a, the store-backed parse check (`gate-logs/compose-1a.txt`, `tools/compose_1a.py`)

Synthetic only (no product code, no real data): the tiny world (`scripts/tests/fixtures/tiny_world.py`, seed 7) built
into `build-equity/p9-gate-compose-tw` (role `2b34283c...6c31`, fields `395f82f7...a1ac`, = the template's locked
pins); the step-1 Release `atx-equity-strategy-risk` (p9-3f `0c110534...586b`) `risk ... --emit-exposures all
--emit-container --bias-families extended --recipe atx-cov-v1` on it: **exit 0**, a store
`build-equity/p9-gate-compose-store` (manifest `5a4b7b9b...1555`, container `model.atxcov` written; 656 sessions x 64
names, 0 fitted / 0 forecast sessions on so short a world). Then the Release NAV exe (p9-3f `255b58ac...3565`) on the
tiny world's NAV argv (`tiny.json` nav block; `--combined` names a placeholder file: every check below comes before the
blend load), one not-yet-existing `--output` each:

| case | flags added | exit | the refusal line | stage reached |
|---|---|---|---|---|
| COMPOSE-1 | `--rate info-clock-rate-v1 --risk-model <tiny store> --risk-model-sha256 5a4b7b9b... --target-rule mv-aim-v1` | 1 | `InvalidArgument: nav replay: fields lack ea_days_since` | v7 parse, the store open (role pin checked), the base parse and the run checks pass; refused at the fields load: the tiny world has no `ea_days_since` (the rate's clock) |
| control: `mv-aim-v1` alone | the store flags + `--target-rule mv-aim-v1` | 1 | `InvalidArgument: target replay: manifest missing/oversized` | past the fields load; stops at the placeholder blend |
| control: the rate alone | `--rate info-clock-rate-v1` | 1 | `InvalidArgument: nav replay: fields lack ea_days_since` | the same stop as COMPOSE-1 |
| control: two stores | COMPOSE-1's flags + a second `--risk-model` | 2 | `InvalidArgument: nav v7: duplicate/missing value: --risk-model` | refused at the v7 parse (the M-1 refusal) |

No output directory was created by any case. **COMPOSE-1a outcome: ACCEPT at the gate's store-backed parse check**
(the two rules together pass the parse and the store open with a real store; the one refusal is the tiny world's
missing clock field, identical with the rate alone, i.e. a data refusal, not a flag conflict). So P9-MV does NOT lapse
under COMPOSE-1 at the gate. Stated limit: `validate_nav_config` / `validate_nav_input` run after the fields and blend
loads (`book/nav_lockstep.cpp:238-250`), which the tiny world cannot reach with the rate on; those checks of the two
rules together are P9-MV's own preflight (COMPOSE-PRE: a refusal there lapses P9-MV at 0 before any read). The tiny
world and its store are deleted by literal name after this record.

---

## Chain agent 3 (RESUME 13, 2026-10-16; `r13-prompts/gate-prompt-3.md`; rulings GATE-3-ORDER, FIELDS-SUB, FIELDS-X5, GATE-MYPY, LAPSE-TABLE, TEST-2)

Started from `handoff-root-gate-2.md`. Order of work = GATE-3-ORDER (ledger :961): every item that names no candidate
cache first, then the GATE-IDX check. Every commit of this agent: explicit pathspecs plus
`:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache`, then `git show --stat HEAD` checked for
`mypy_cache` (0) (GATE-MYPY; the 133 staged editor-side paths are the owner's to unstage, never touched here).

### Step 0 re-verification (agent 3, 2026-10-16T14:04Z)

- HEAD `2e08fa1c` (docs); code head `06dc9e5a`: `git diff --name-only 06dc9e5a..HEAD` outside `.superpowers/` = 0 files.
  `git status --porcelain` (outside the staged `.mypy_cache`): the PM's `progress.md` edit and the five untracked PNGs.
- Trial ledger 133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
  `build-equity/research-store/` absent (SQL3-2).
- GATE-IDX file `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` **still present** (72 KiB) at start; not touched.
- SPDLOG-RELINK re-hash (`tools/exe_sha_end.py`): **27 / 27 Release (p9-3f) and 27 / 27 Debug (p9-3g) `disk = receipt`**
  (`gate-logs/exe-sha-rel-3.txt`, `exe-sha-dbg-3.txt`); next free tag `p9-3h` unused.
- Host: free RAM 3,490 of 16,069 MB; C: 66.6 GB free; no build running (ours or foreign); editor-side python only.

### FIELDS-SUB (1): the witness (`gate-logs/tools/fields_witness.py`, `gate-logs/fields-witness.txt`; commit `e5a2fe68`)

For each of the 70 names of `scripts/specs/v8/lib-v80.json` `fields.list` (R-2's spec, sha256 `306a070b...3a21`;
`fields.manifest_sha256` = v10's pin `a4a060ae...0809`): (a) the v11 manifest (`5826c02a...5f69`) has the entry, file
`<f>.f64`, `reused_from.dir` ending `fields-v10`, `reused_from.manifest_sha256` `a4a060ae...0809` and a `payload_sha256`;
(b) v15's manifest (`26fee5ce...3b09`, re-hashed) `files[<f>.f64].sha256` equals it; (c) the bytes on disk
`train-2020-2023-lo3-fields-v15/<f>.f64` hash to it (every one of the 70 hashed in full). **Result: 70 / 70 hold, no
miss.** Beside it, the two fields R-2's NAV of record says it read (`summary.json` `/financing_fields/fields_used`:
`shares_out` `1cff53f8...05bc`, `si_shares` `78036207...7186`) equal v15's `files` entries and the bytes on disk.

### Step 4, agent 3: the P9-B0 substitution list (written and committed BEFORE any P9-B0 run)

Reading of GATE-3-ORDER's items (1) and (2): the witness (1) runs first; the substitution list (2) is committed before
any P9-B0 run (the brief's step 4 and FIELDS-SUB: "listed FIRST in the substitution list"), so R-2's NAV run (1) starts
only after this list's commit. The runs, their tool and the checks are fixed here; nothing below is decided after a run.

Common to every run below:
- **Exe** (M2c-RED-2 (e), OD5-2: Release is the producer of record): token 0 of every receipt of record (a Debug exe in
  `build-equity/bin`) -> the Release exe of step 1 in `build-equity-rel/bin`, tag p9-3f, source = code head `06dc9e5a`
  (receipt Source `5f48a25d`, a docs commit: `git diff 5f48a25d..HEAD` outside `.superpowers/` empty, EXE-CUR): NAV
  `atx-equity-strategy-targets` `255b58ac...3565`, IC `atx-equity-strategy-ic` `ccd54db6...d705`; re-hashed against the
  receipt before and after each run (SPDLOG-RELINK).
- **Runner** (RUN-1, RUN-2, COVR-1): `scripts/run_bounded_research.py --output build-equity/<out>-run --seconds 600
  --max-rss-mib 2048 --build-type Release --admission-wait-seconds 1800 -- <exe> <argv>`; u / w at the registered phase
  caps of prereg C.14 (u 2,560, w 3,072 MiB; `lib-v8ysb-gm.json:10-28`), free RAM stated; alone on our side of the host
  (GATE-PAR); one per pool.
- **Argv** = the receipt of record's `command` with ONLY the tokens named per run changed (the tool asserts each named
  flag occurs exactly once, that no added flag is already there, and re-hashes every substituted pin from its file).
  `--ann-mean-gross` appended on every NAV reference (PRE-7a, C3-9); `--info-clock-print` appended on Y-F0's NAV only
  (ALCLOCK-1: the P9-B0 names it on the Y-F0 reference; held at agent 2's step 3; LAPSE-TABLE P9-C RUNS). Fit mode
  flag-absent (LAPSE-TABLE; D3-Q1a). No `--candidate-cache` on any NAV argv (checked on each receipt of record before
  its run: none names one), no cache-index flag anywhere (SQL-11, prereg C.13). Timings off: no timing flag on any argv
  (C.5).
- **`--seal-allow`** (SEAL-ALLOW-3): a flag of the wave driver `research_wave.py` (its seal scan over the wave's logs;
  `scripts/wave_seal.py:18`); a hand run through the bounded runner has no seal scan and the exes take no such flag, so
  it is named on no argv here; should any run stop on a seal token, `tools/sealsrc.py` runs first and the stop is
  recorded (none expected).
- **No statistic is read.** Outputs are compared by file SHA-256 and JSON leaf path (`m2g-logs/tools/nav_cmp.py`: CSVs by
  header, row count and the count of differing cells per column; no value printed). Every output file's SHA-256 is
  listed (`gate-logs/b0-files-<out>.txt`). A run's receipt must carry no `store` block (SQL3-2).
- **Tool:** `gate-logs/tools/b0_nav.py <cell>` (NAV runs; `--dry` prints the argv and its token diff, run before this
  commit for all four NAV cells); the u / w runs use the same form (token 0 and `--output` only).

| # | reference replaced | receipt of record (argv source) | tokens changed (besides token 0) | new dir | compared against / expected | why |
|---|---|---|---|---|---|---|
| 1 | **R-2** (A.4; no P9-B0 reference yet) | `mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80-run` (v8 Debug exe `5497c89d`, source `39f87cd6`, 2026-10-02) | `--fields build-equity/train-2020-2023-lo3-fields-v15/manifest.json --fields-sha256 26fee5ce...3b09` (the v10 pair: FIELDS-GONE, FIELDS-SUB, witness above), `--output build-equity/p9-b0-r2-nav`; + `--ann-mean-gross` | `p9-b0-r2-nav` | the v8 NAV of record `mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80` by SHA-256 per file; allowed: `summary.json` `/producer/*` and the leaves `--ann-mean-gross` adds (`/scenarios/<i>/ann_mean_gross`, opt-in side only, the M2j item-4 form); identical outside them = path A (R-2 re-based = `p9-b0-r2-nav`, PRE-22 met by substitution); any other difference, a witness miss or a refusal of the 84-field manifest = **path B** (R-2 NOT re-based; G-B6 / OD-3 lines as FIELDS-SUB words them). Facts stated before the run (not a ruling): that reference was written before P9 by the v8 exe, i.e. before C1's NAV change (M1d's P9-B0 list: L1-L4 / capacity files "may differ (sqrt)" against a v8-era reference); and its `summary.json`, `recipe.json`, `capacity/summary.json`, `capacity/recipe.json` carry the fields manifest's SHA-256 (`/financing_fields/manifest_sha256`, `summary.json` `/source_bindings/research_fields_manifest_sha256`). The verdict is the ruling's letter; every differing path is ALSO classified (fields-manifest leaf / M1d's C1 classes / other) for the PM, deciding nothing | PRE-22; FIELDS-SUB |
| 2 | **X-10** (`p9-b0-x10-nav`, M1d, Debug `407c34ad`) | `p9-b0-x10-nav-run` | `--output build-equity/p9-b0g-x10-nav`; + `--ann-mean-gross` | `p9-b0g-x10-nav` | `p9-b0-x10-nav`: 27 / 27 files, 26 byte-identical; `summary.json` only `/producer/build_type`, `/producer/engine_git_sha` and `/scenarios/<i>/ann_mean_gross` (the form of agent 2's Y-F0 identity on the same exe, `nav-identity-yf0.txt`); anything else = an identity miss, attributed by merge commit (ROOT-1) | B2-13 (the leverage-only pair's reference); GATE-3-ORDER |
| 3 | **Y-1** (`p9-b0-y1-nav`, M1d, Debug `407c34ad`, store `v8-risk-lo3-v10`) | `p9-b0-y1-nav-run` | `--risk-model build-equity/p9-m2i-dcov-v11 --risk-model-sha256 397366473b3a...23c7` (the Release `atx-risk-v1.1` container store by its pin, PRE-29; `handoff-root-m2i.md` "Store pins"), `--output build-equity/p9-b0g-y1-nav`; + `--ann-mean-gross` | `p9-b0g-y1-nav` | no identity is claimed (PRE-29: another store, not byte-comparable with v8's Y-1): every file's SHA-256 recorded; compared with `p9-b0-y1-nav` and, PRE-34's form, with v8's ledgered Y-1 NAV `mega-nav-v8y-vol-target-L2.0`, each reported as "agree / differ at <paths>" by SHA-256 and JSON path only; PRE-34's printed-precision agreement of the figures is a read of figures (B.7: made before the adoption print) and is NOT run here (READ NO STATISTIC). Before the run: COV-9 role check (`m2g-logs/tools/s9_check.py`), COVR-1 | PRE-29, PRE-30, PRE-34 |
| 4 | **Y-F0 u** (`p9-b0-yf0-u`, M1d, Debug IC `393183c0`) | `p9-b0-yf0-u-run` | `--output build-equity/p9-b0g-yf0-u` (the argv keeps `--candidate-cache build-equity/mega-candidate-cache-v8-lo3`, GATE-RELCACHE) | `p9-b0g-yf0-u` | after GATE-IDX only. `p9-b0-yf0-u`: payloads (`orientations.json`, `recipe.json`, `train_daily_ic.csv`) byte-identical; `train_candidates.jsonl` equal outside `/stage_seconds/*`, `/wall_seconds`; `summary.json` differs by design only at timing leaves, the Release VM identity strings and cache-root paths, the cache counters (cold Release pass vs warm Debug reference: M1d's Release IC adoption list, `integration-log.md:304-327`) and the cold-vs-warm field counters `/roles/*/research_fields/{field_loads,loaded_bytes,peak_resident_fields}` (GATE-RELCACHE), each listed by path; anything else = an identity miss (ROOT-1). Cache: GATE-RELCACHE's checks (>= 15 GB free before; after, the 351 original files byte-unchanged, `155872de50833fca`, and the new whole-dir listing recorded with its cause) | PRE-22 |
| 5 | **Y-F0 w** (`p9-b0-yf0-w`, M1d) | `p9-b0-yf0-w-run` | `--output build-equity/p9-b0g-yf0-w` (cache as row 4; `--composition-weights` the v8 fit's, as the receipt) | `p9-b0g-yf0-w` | after GATE-IDX only. `p9-b0-yf0-w`: the 10 payloads byte-identical (`orientations.json`, `recipe.json`, `train_combined.{f64,json}`, `train_combined_{finite.u8,ids.u64,member.u8,sessions.i64}`, `train_daily_ic.csv`, `train_planned_targets.csv`); `summary.json` / `train_candidates.jsonl` as row 4 | PRE-22 |
| 6 | **Y-F0 NAV** (`p9-b0-yf0-nav`, M1d) | `p9-b0-yf0-nav-run` | `--output build-equity/p9-b0g-yf0-nav`; + `--ann-mean-gross --info-clock-print` | `p9-b0g-yf0-nav` | after GATE-IDX (GATE-3-ORDER). `p9-b0-yf0-nav`: 27 / 27, 26 byte-identical; `summary.json` only `/producer/build_type`, `/producer/engine_git_sha`, `/scenarios/<i>/ann_mean_gross` and `/scenarios/<i>/info_clock/**` (opt-in side only; ALCLOCK-1 as held at step 3) | PRE-22, ALCLOCK-1, LAPSE-TABLE |
| - | **X-5** (`p9-c1-s2-x5-nav`; u / w) | - | - | - | **CUT** (FIELDS-X5): "not run at the gate: fields v13 absent (FIELDS-GONE)"; X-5 is no P9 cell's parent; its opt-in cache identity of record is M6b's items 5 / 6 (`handoff-root-m6b.md`), cited, not re-run | FIELDS-X5 |

Not re-based: the Y-F0 fit (the brief's step 4 names u, w and NAV; the NAV references read the v8 w's
`train_combined.json`, the w reads the v8 fit's weights, each stage on its reference's own inputs, the M1d form); the
gate's flag-absent fit identity of record is agent 2's (`fit-identity-yf0.txt`: holds outside D2's code-identity
stamps). The D-COV stores are pinned bytes, never rebuilt (CRT-2). rho (step 5) is taken on the new NAV dirs above
(B2-13; value and the "1 - rho < 1e-6" flag only).
