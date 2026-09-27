# Independent L1/R3/A4 qualification audit

2026-09-26. Read-only audit of root report `ffa53664d0836d2bfa8a4b71fbffededbe241efe` and source `2a3d5c2b17d5fbe93261d9d1d0202a147146f416`. No reruns, builds, configuration changes, or market payload reads were performed.

**Approved as bounded runtime evidence.** Independently parsed all six XMLs and native receipts under `C:/atx-wt/pool-2/build-equity/`. Every receipt reports that source and exit 0. Every testcase is run/completed, with no failure, error or skipped node. The sets are disjoint: core 14, dataset 5, costed 37, mine 30, search 65, eval 98; **249 distinct passes**. The five new suites contain exactly the expected **28 checks** (9 ledger, 5 execution fitness, 2 actual mine, 5 dataset, 7 costed solver).

The case index `w2-execution-qualification-index.json` hashes to `fbd13874219e0653c285a7b6cef61e7646c0263270d495d095ee59c69d448905`, matching the report. Independently hashed the four current executables in `build-equity/bin`; each matches every corresponding run receipt:

| Executable | SHA256 |
|---|---|
| atx-engine-ic-screen-tests.exe | bf3f718ccbf944682250f3bce7573e4885c1e2ac30b20a6d9ed4c114cf2616c0 |
| atx-engine-w1-eval-tests.exe | 468ed9f0ca50a1e15d7c04f04419e33b9d3e83f0aa511f916639a6e516eb17da |
| atx-engine-w1-risk-tests.exe | 246bb4b93d6efcda71f30bdf429b63cf1069c8aaa273d478431aeab065e71ab0 |
| atx-impl-ic-screen-tests.exe | 517524e050fd6067b3d39dd08662043dee02aae38acc302e771f770c8458a87c |

XML bindings (`w2-execution-<suffix>-qualified.xml`):

| Suffix | SHA256 |
|---|---|
| core | 78ec778a55813e31f298ea827e51fa40c648ed2def8d5c7be5455a3ba603e091 |
| dataset | 44a4cfbdd4a3bd6e73e28de602338313735e255e8b42bfdd89bca9dd11e9a3d9 |
| costed | 6ffe0b516f6af9497e2d86ff78742bac56f56ceb0e30a15725a1606912aaea99 |
| mine | 9863a11df2c69496fd084311eca78eee0fb5c8ff6108da007cdd6bfb035c2ff1 |
| search | cf1a407a42b6c993e51b8091bf7354e196601a16f25385f7d5bac291e3d24b0a |
| eval | 8af6b3cb0be4d781de02515a8800ee3ed6feb1e990939784616b0b916083891d |

The earlier integration pass compared the 15 L1/R3 receipt-bound source blobs and 16 A4 final source/fixture blobs with the compiled root revision; no omission was found. Generated commands and CMake registration include the three new production CPPs and four new test TUs. Normal and focused test target entries are expected separate consumers, not duplicate cases in the above run sets.

Build receipts record successful leaf work (82.0650755 s, Jobs2, eight objects including configure) and the combined target build (359.8567938 s, Jobs3, 75 objects/six links). This is broad API integration evidence, **not an incremental speedup claim**. The prior private-boundary audit is separate and is not repeated here.

Approval does not close full lane acceptance: large RSS/performance, full out-of-core training, real-market coverage, calibrated economic preferences, residual exposure/HAC/half-life objectives and general causality-harness gates remain open. A4 remains programmatic opt-in. These synthetic results establish no tradeable-alpha or profitability claim and must not be added to earlier overlapping qualification totals.
