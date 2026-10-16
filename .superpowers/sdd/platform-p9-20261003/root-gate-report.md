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

## Status by step

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
