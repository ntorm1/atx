# Private implementation boundaries and bounded qualification

Compiler source: `008fcdb9` (QP), `7ee0b04b` (small config headers), `6ea53ffd` (application/test PCH; source `f956586d`), `d340caa0` (exposure CPP; source `2058f3a5`), `492ed134` (legacy factor CPP and registration). Source approvals `6defe305` and `58bedfe7`; PCH audit `40f82415`. First combined configured identity `fa5e0b8506a3722cc2d18ea87872f9e131b1cab2`. All production changes preceded qualification; no TDD.

## Preserved numerical implementation

The QP private class retains 1,334 body lines exactly after two class-name substitutions; normalized SHA256 `980c9a9319fad83d1d6d90a4cb93c690ef31000d5553bb08cafeb57edf68a939`. The factor extraction retains 941 body lines after removing inline/default arguments; SHA256 `fc1cfadaaa046900ee14e807f61a7f9a5302659407cdeeef655d372cb26205ad`. Exposure extraction preserves 18 non-template functions. Public value/configuration types, arithmetic and reduction ordering remain intact. The extraction itself contains no R3 objective changes.

Private PCHs contain stable standard/Eigen/JSON (plus GTest for tests) headers, excluding engine/application/configuration headers. Core and test environments use separate PCHs. Compiler options, precise floating point, Ninja, ccache, LLD and existing engine PCHs remain unchanged. After the first-build failure below, configure-time Git provenance is isolated in a tiny generated CPP; no command-line SHA macro affects the PCH or discovery consumer. Root alone runs compilation, with measured RAM and multiple workers.

## Earlier integrated bounded evidence

Clean source `2e3981011478085829596143ebbd905d1283697a` qualifies D6, CPCV caller and R2 changes before the private refactors. Six XMLs contain **465 distinct passes, one existing opt-in skip, no duplicate names and no failures**. This overlaps earlier reports; do not add historical counts. These XMLs do not qualify the later compiler refactors.

| XML | Passed | Skipped | Native seconds | Binary SHA256 | XML SHA256 |
|---|---:|---:|---:|---|---|
| `w1-next-data-qualified.xml` | 86 | 0 | 0.327 | `354106d1ce6d9761b4077d33f23957e6da594128b038ddaeaa8b22c711d038fc` | `9be232b887edd73e83254d3811eda6ee7c0ad20efed43f4bcfcfcf5dcb6a5b25` |
| `w1-next-eval-qualified.xml` | 98 | 0 | 10.787 | `fae55a06fa1dd0137b326ba7d80d6fbc60cbaab9a3575785a8ff7b8c00f86f16` | `552bce31105bedd94cc6dbcdf0ed210e77ab764ff07f756b74e946e9b0ca2b11` |
| `w1-next-ic-qualified.xml` | 65 | 0 | 14.733 | `9f186adb0fb74138e330b3a51bd52f4e3cb5922816c73eb40ba93b6a89a6ea71` | `4f2a365bac23de927eef535150627c3f1a8fe6189a8587a8309166c664388114` |
| `w1-next-impl-contract-qualified.xml` | 97 | 0 | 7.642 | `44a02e7a4a2230f33c9e7ea910d2e7924f95a22c55fab09dfe360499e3ea169f` | `a35e5eddc1a0b660257549bb34cca1d19fc1e8d54b71cefa33f6ad40f7229ba0` |
| `w1-next-impl-search-qualified.xml` | 85 | 1 | 56.418 | `84dcb4cde533b56dd4eb61b06650fb40f98d0bb2f13c0c3bb6afc57ad4c5172b` | `4c202d614cc0930e86adcb89480c4564e6960aa449280174a75f4124b5d79650` |
| `w1-next-risk-qualified.xml` | 34 | 0 | 0.154 | `e0f29500d112a2a349fb8130713d64a3e3dfba93aa82fe743005d046354378c4` | `2b59873e25382efbafa3b949ba29302792cf507ddc2532026da64fcac644133c` |

Files live under pool2/build-equity. Index `w1-next-qualification-index.json` binds source/filter/exit/time receipts. Search skip is `AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha`, an existing opt-in panel case. Eval excludes the timed cached/uncached benchmark; risk runs 34 selected estimator/model cases. Large simulations, actual 2020+ payloads, warehouse writes and alpha promotion did not run.

I1 runtime preregistration initially aborted with no XML (`w1-next-impl-prereg-qualified`, native exit -1073740791, 14.264s wall). The fixture moved a CSV row into positives then indexed the moved vector. Test-only `70bfef58` changes the second conditional to else-if; production unchanged. The incomplete run is excluded from the six-XML count; its rerun is recorded below when complete.

## Build measurements

Prior broad header batch: `w1-next-integrated-build-receipt.json`, source948498c2, Jobs2,419.747s, initial missing-fstream fixture compile failure. The graph included109 objects and9 links, no PCH/dependency/worker rebuild. Final narrow resume at2e398101: Jobs3,55.927s,22 actions plus glob check. Exact unchanged repeat `w1-next-noop-receipt.json`: Jobs3,3.562s,no work and zero compiler/cache calls. These establish that broad header fan-out, rather than unchanged-tree recompilation, caused the long batch.

Initial refactor build atfa5e0b85: Jobs3,515.5435341s,exit1. The numerical CPPs compiled, but clang-cl rejected stage_discover's source-local SHA macro against the common PCH under /WX. The initial graph audit had incorrectly accepted that exception; the real compiler exposed it. No cold-build speedup is claimed. Fix `56df1587` replaces that macro with a stable declaration and a tiny configure_file-generated CPP that skips PCH. Unknown/dirty provenance semantics are preserved; only the generated file changes on a subsequent SHA bake. Configure at clean56df1587 passed25.813s; warm resume uses Jobs2 at about1.5GiB free, retaining successful objects.

The first resume at56df1587 took73.009s and stopped on trial_ledger.cpp: its deliberate source-local CRT declaration switch arrived after the forced PCH. Fix `d59f4abd` applies SKIP_PRECOMPILE_HEADERS to that production TU and the five existing CRT-policy fixtures (discover, alpha101_orats, fundamental_zoo, seed_parse, single_alpha_capacity). Lock and fixture behavior and all common compiler flags/PCH contents stay unchanged. The follow-on build uses Jobs2 and normal Ninja automatic CMake regeneration. The generated provenance object took0.489s in the Ninja log; this is one compile action, excluding configure/link time.

Final resume at clean `d59f4abda191d4c66ef10459e6a95d250c0e7e38`: native0, Jobs2,177.8167963s including normal automatic configure; 35 compile actions plus5 links. All three adoption attempts total766.3696842s. This is setup cost, not evidence that a broad cold build became faster. Successful objects survived both source/configuration corrections.

Exact unchanged three-target repeat `w1-private-noop-receipt.json`: native0,Jobs3,**3.8043152s**, no work, no ccache stats/log file and zero compiler calls.

Actual localized edit `817bd5843a43936a224940cc6e89de8422979fa5` gives the QP private solver a named stack instance for debugger inspection; configuration/dispatch/math unchanged. `w1-private-leaf-build-receipt.json`: native0,Jobs3,**21.7589912s**. Exactly one changed CPP (qp_solver.cpp), engine archive and three executable links; no caller/PCH/dependency compilation. Dedicated ccache stats show one direct miss, one preprocessed miss and one compilation miss, so this is not a touch-only cache-hit measurement. The configure-time provenance remains d59f4abd; ordinary CPP edits do not require configure. The entire numerical implementation remains compiled in that one TU; this is not a minimal toy TU benchmark.

## Refactor runtime qualification

At d59f4abd, **82 distinct focused checks passed**: QP/factor solver, exposure/PIT, discretization and R2 estimator67; actual I1 runtime preregistration3; actual provenance/configuration12. The I1 moved-row abort is closed by the test-only repair. After817bd584, the analytic optimum and byte-determinism checks pass again (2 repeated cases, not two additional distinct checks). No long simulation or market-data run was introduced.

| Artifact prefix | Cases | Native seconds | Binary SHA256 | XML SHA256 |
|---|---:|---:|---|---|
| `w1-private-risk-qualified` | 67 | 21.967 | `83dc120de06614b5858ad91baf70d25ac53d9749f44b48827388a9595e007dee` | `f009490377fbaa3f3d7f5dc0e1e357d91077a88cfea233d91546726e2e058b09` |
| `w1-private-prereg-qualified` | 3 | 22.604 | `0fdf0b207431091cb3ea039e1b6121cc3c899d59669bed611d55f78ba62a28a0` | `df6471417e903a08bf9321bfaaca1265be04bc915adf79b75fd7326467871ee2` |
| `w1-private-provenance-qualified` | 12 | 10.764 | `4a98a06dead121198151a6dcea40659a74de4e405c17f671027198e6a52a6440` | `ce1af2d51b0e3fd3b89389c6aca4f6e118b4ab3e785f68e1c115c9d1cc4c5704` |
| `w1-private-leaf-risk-qualified` | 2 | 0.017 | `5c89b814b98c22ed78a0611a62708737a9de52ccfe21c751fb21112f057f5cc8` | `dc4b4dbcb53041d93ca061d44a5f882c5d6a1f2c5e5fe35f9fd37708aa016350` |

All paths are under pool2/build-equity; `w1-private-qualification-index.json` binds full source/filter/wall/native receipts. Historical2e398101 XMLs and d59f4abd runtime are separate binary attributions; later relinks are not passed off as reruns of every historical case.
 One-time PCH creation and moving code out of public headers invalidated prior consumer objects. The measured steady-state edit changes preprocessed source and produces a cache miss. Shared-host timings with different graphs are not a controlled speedup claim.

## Remaining scope

L1 and R3 source freezes are separate and initially not imported into this compile-boundary batch. W1-W5 wave/large-data/empirical gates remain open. W0 long comparison is owner-deferred, not passed. The IC fast screen exists; practical noisy-null rejection and general true-alpha recall remain unqualified. No tradeable alpha was promoted.
