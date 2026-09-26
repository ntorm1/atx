# Independent generated-graph and qualification audit

The pre-refactor qualification and compiler-refactor graph are separate evidence
sets. Source/read-only metadata inspection performed in pool-3; no configure,
compiler, benchmark, cache mutation or numerical job launched by this reviewer.

## Prior qualification, source 2e398101

Index `C:/atx-wt/pool-2/build-equity/w1-next-qualification-index.json` SHA256:
`78518b516950082203630a50ae70b533fc5658de2348614e875f30125b55f1c4`.
Its six XML files were independently parsed, their bytes hashed, and their
adjacent receipt files compared with the copies embedded in the index. Every
receipt records native 0 and source
`2e3981011478085829596143ebbd905d1283697a`.

| XML stem, prefix w1-next- | Passed | Skipped | XML seconds |
|---|---:|---:|---:|
| data-qualified | 86 | 0 | 0.327 |
| eval-qualified | 98 | 0 | 10.787 |
| ic-qualified | 65 | 0 | 14.733 |
| impl-contract-qualified | 97 | 0 | 7.642 |
| impl-search-qualified | 85 | 1 | 56.418 |
| risk-qualified | 34 | 0 | 0.154 |

The union contains **465 passes and one skip, 466 distinct case names**, with
no duplicate names, failures, errors or not-run cases. The opt-in skip is
`AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha`: its real-panel rediscovery
verdict requires `ATX_ALPHA101_PANEL`. This is not a passed alpha gate.
The separately aborted preregistration fixture has no XML and is correctly
excluded from these totals; its later correction/rerun needs separate evidence.

At 2026-09-26 19:06 UTC, current data/eval/IC executables independently matched
their recorded hashes. Those targets were not being relinked by the active
compiler-refactor build. Current risk/impl executable reads were deferred until
that build finishes to avoid contending with its linker. The six historical
receipt hashes remain evidence of the prior run, not qualification of newly
linked binaries.

## Generated graph, source fa5e0b85

Configured clean source `fa5e0b8506a3722cc2d18ea87872f9e131b1cab2`.
`compile_commands.json` SHA256 at inspection:
`da5a75fa21a3a57ff98ab26749dcdacd19c2ce086d1b33530495b51b9173f27c`.
Dry-only `w1-private-advisory.log` SHA256:
`07154250909e48ade337401c0a158822f2e6a6259b3ad8388160420d3ae44a75`.
Both paths are under `C:/atx-wt/pool-2/build-equity/`.

After removing only source/output arguments and normalizing PCH creation `/Yc`
versus use `/Yu`, all **114** application-test/carrier commands have identical
compiler options, definitions and include order, including the shared `/Fp` and
`/FI` paths. These comprise 95 ordinary tests, seven IC tests, ten W1 contract
tests, the carrier source and its PCH creation command. Normalized environment
SHA256: `23b9dc50d317013601e2726835fbdf5973c2ca47af166f5bea1e1b57d7799f0f`.

The production PCH creation command and 34 ordinary application source commands
also have identical environments. `stage_discover.cpp` is the only additional
environment and differs solely by its source-local `ATX_ENGINE_GIT_SHA`; removing
that definition yields an exact match. The stable payload does not use this
macro. **The initial review incorrectly accepted this exception:** clang-cl's
PCH compatibility check rejects the extra definition even when the payload does
not reference it. The actual failed build below supersedes the source-only
compatibility approval for that TU. Production/test PCH paths remain separate,
as their GTest/miniz/fixture environments differ.

The 111-action advisory contains 106 compilation actions and five links:

| Target family | Compilation actions |
|---|---:|
| engine production | 42 |
| impl production, including its PCH | 36 |
| application test carrier, including its PCH | 2 |
| focused risk tests | 9 |
| focused impl IC tests | 7 |
| focused impl W1 contracts | 10 |

There are exactly two new PCH creation actions and one trivial carrier source.
No engine PCH rebuild, ordinary application-suite build, or worker build appears
in the advisory. The focused Ninja order-dependency edges contain neither the
ordinary suite nor worker. The carrier's own order boundary is empty (`.`).
Each newly extracted risk source (`qp_solver.cpp`, `qp_factor_admm.cpp`,
`exposures.cpp`) occurs exactly once in the compile database.

This verifies target selection and matching environments except for the rejected
source-SHA exception; it does not establish runtime or numeric parity. Initial
PCH adoption necessarily changes consumer command lines; only the later warm
no-op/leaf receipts can demonstrate incremental behavior.

## Compiler-refactor timing and current-binary follow-up

The first build at `fa5e0b85`, Jobs=3, failed natively with exit 1 after
515.5435341 seconds. `stage_discover.cpp` was rejected with
`-Werror,-Wclang-cl-pch`: `ATX_ENGINE_GIT_SHA` differs from the production PCH
environment. This failed attempt is preserved, not excluded from build cost:

| Evidence | SHA256 |
|---|---|
| w1-private-build-receipt.json | d85389f207abfb1f92be4e689ae8fe9802126f3f2565164a3cff2906a95d11d3 |
| w1-private-build.log | 5fad5c7b18773c0095ba377b87171f7268c19d84ee11ef81b61843971b353923 |

Root fix `56df1587e0b90a4b64c90547d3774ad095d1fad8` separates the changing
provenance into a tiny generated source file and stable declaration. Independent
source review approves this fix: the returned string_view borrows a static
literal, the source include path resolves the declaration, exactly one owning
definition is registered, and only this tiny TU skips PCH. Existing configure-time
unknown/dirty resolution is unchanged. `configure_file(... @ONLY)` writes only
when its output content changes. Discovery's assignment preserves the same
textual metadata without a command-line macro.

The clean `56df1587` generated graph was then independently checked. All 36 core
PCH creation/use commands now share exactly one environment; the only non-PCH
core command is `generated/build_provenance.cpp`. No command in the database
contains `ATX_ENGINE_GIT_SHA`. The 114 test/carrier commands retain the same
normalized environment hash as above. Removing the rejected macro and the
launcher/showIncludes wrappers from the prior logged failed command yields the
current core environment exactly: no unrelated PCH-path or flag change.
Current database SHA256:
`468fd6cae89686f43ac30df7db01e109b8bee6f0e21263e76447a2f44085c088`.
Generated provenance source SHA256:
`aa03428a6ccce80785539f4b4aa86766aaef98abf358e35591df2ae0f79c0fb8`;
its literal is the clean `56df1587` full SHA.

The first resume at `56df1587`, Jobs=2, also failed: native exit 1 after
73.0093538 seconds. `trial_ledger.cpp` deliberately defines
`_CRT_SECURE_NO_WARNINGS` before its includes to retain its exclusive-create
`fopen` use. Forced PCH parsing moved the CRT declarations before that definition,
causing `-Werror,-Wdeprecated-declarations`. The file's existing comment explicitly
warned about adding PCH. This review's initial macro scan was too narrow
(Eigen/JSON/GTest) and missed that warning.

| Evidence | SHA256 |
|---|---|
| w1-private-resume-build-receipt.json | aadef787c820c78510b5206a3fce7686e50aa7cde6b4b872b22c74216480e270 |
| w1-private-resume-build.log | 2001e9a77d12718c4bed6181119d436ca1d0f239395394213c28258337d24b3e |

The full application macro inventory identifies six existing local CRT-switch
sources: production `trial_ledger.cpp`, and fixtures `discover_test.cpp`,
`alpha101_orats_test.cpp`, `fundamental_zoo_test.cpp`, `seed_parse_test.cpp`,
`single_alpha_capacity_test.cpp`. Root applied source-scoped PCH exceptions in
`d59f4abda191d4c66ef10459e6a95d250c0e7e38`, preserving their original
include/warning semantics and lock algorithm. Other local defines are
command-line-covered fixture-path fallbacks and `NOMINMAX` in the already-compiled
data-provenance source.

Independent inspection of the clean `d59f4abd` graph confirms exactly the intended
exceptions:

| Target | PCH commands, including creation where applicable | Non-PCH commands |
|---|---:|---|
| impl core | 35 | ledger and generated provenance |
| test carrier | 2 | none |
| ordinary application tests | 90 | five CRT-switch fixtures above |
| focused IC tests | 6 | discover fixture |
| focused W1 contracts | 10 | none |

The PCH command environments remain identical to the previous configure:
core hash `762622a39a8fec729c828dda0b087a63a3c5cfbfe0507eb19aad12e8a1eb29e8`,
test hash `23b9dc50d317013601e2726835fbdf5973c2ca47af166f5bea1e1b57d7799f0f`.
No SHA macro remains. Compile database SHA256:
`25d0474f0e5d6fe4939d30a6dfecbb78f1ad4fe2b2259fa615e05c8ec7423cf1`.
Generated source SHA256:
`2ebbf4a319006bfac085680c3023aa4b4278b560b71291ca1b537287ad65e37b`,
containing the clean `d59f4abd` full SHA.

The final resume at `d59f4abd`, Jobs=2, completed natively with exit 0 in
177.8167963 seconds, including automatic CMake regeneration. Its log contains
35 compilation actions and five links, including the first completed test PCH
creation; previous successful objects were retained. The three adoption attempts
total **766.3696842 seconds**, including both failures. There is **no cold-build
or overall speedup claim**; different requested work/resource conditions are not
a controlled before/after comparison.

The exact repeat of risk/impl-contract/impl-IC targets completed with native 0
in **3.8043152 seconds**, Jobs=3. Its log contains only a glob check and
`ninja: no work to do`; the receipt records no cache-log creation. No compile or
link action was hidden inside this no-op.

| Evidence | SHA256 |
|---|---|
| w1-private-final-build-receipt.json | 248c14df870146f3f2464a7950b95760f3656f538a93c7ca3584dd24d3c4f735 |
| w1-private-final-build.log | 7e564d0f168cfcc36b4aff15ec85b58d01e3f999f9aa71f0c77a8ebe7bf823e3 |
| w1-private-noop-receipt.json | 3ef7515e41e26c10af11161aeefe0b256dbcb1e297347e181a084a5c8fcc1f0a |
| w1-private-noop-build.log | 57f56d16ae002e8eeb189c74da18d15923afd9f133d1a936cd6217450ae0006d |

After that build, current data/eval/IC binaries still match their prior receipt
hashes. The three rebuilt binary hashes are below; they deliberately do **not**
inherit the historical 465-pass count:

| Target | Current binary SHA256 after final resume |
|---|---|
| atx-engine-w1-risk-tests | 83dc120de06614b5858ad91baf70d25ac53d9749f44b48827388a9595e007dee |
| atx-impl-w1-contract-tests | 0fdf0b207431091cb3ea039e1b6121cc3c899d59669bed611d55f78ba62a28a0 |
| atx-impl-ic-screen-tests | 4a98a06dead121198151a6dcea40659a74de4e405c17f671027198e6a52a6440 |

## Actual implementation edit and runtime attribution

Commit `817bd5843a43936a224940cc6e89de8422979fa5` changes only the compiled QP
implementation: `solve` gives its private implementation a named const lifetime
instead of invoking the same method on a temporary. Configuration and dispatch
are unchanged. The edit changes preprocessed source, so this is an actual
compiler-cache miss rather than a timestamp touch or comment-only proxy.

The parent-owned build completed with native 0, Jobs=3, in **21.7589912 seconds**.
The independently inspected log has exactly **one C++ compile (`qp_solver.cpp`)
and four links**: engine archive, focused risk, impl-IC and impl-contract binaries.
No caller, dependency or PCH was recompiled. Its per-build ccache statistics
contain exactly one cache miss, direct miss and preprocessed miss, with no hit or
uncacheable compiler invocation. This demonstrates the local implementation-edit
path on this host/build state; it is not a general compile-time guarantee.

No configure was needed for that edit. The receipt explicitly distinguishes
actual source `817bd584` from configure-time provenance `d59f4abd`; the generated
provenance string does not claim to have refreshed automatically on every build.

| Leaf evidence | SHA256 |
|---|---|
| w1-private-leaf-change.json | 2f60d7594a291027b5f7fa4e886dc226dc76705e2464aecb7ec78f55c696d685 |
| w1-private-leaf-build-receipt.json | f0adfc81722dad755ab9c15783d8f371216ccc1f734d6434cde80c3eff361500 |
| w1-private-leaf-build.log | 31c5e0089309b73e503ff069a6d8a207716ad711c9b11a63c8d0b27baae01e80 |
| w1-private-leaf-cache.stats | 43e8b95e9b6eb609cd90969a8030c4fde9d45b0a76d2e0db82a816bd631a10f8 |
| w1-private-leaf-cache.log | ca1e897b1998afb5449115bb51f7284a34b652075881e7f98f392289b109695a |

The following four XML/receipt pairs were independently parsed. Every receipt
records native 0; all cases completed without failures/errors/skips:

| Source | XML stem, prefix w1-private- | Passed | XML seconds | XML SHA256 |
|---|---|---:|---:|---|
| d59f4abd | risk-qualified | 67 | 21.967 | f009490377fbaa3f3d7f5dc0e1e357d91077a88cfea233d91546726e2e058b09 |
| d59f4abd | prereg-qualified | 3 | 22.604 | df6471417e903a08bf9321bfaaca1265be04bc915adf79b75fd7326467871ee2 |
| d59f4abd | provenance-qualified | 12 | 10.764 | ce1af2d51b0e3fd3b89389c6aca4f6e118b4ab3e785f68e1c115c9d1cc4c5704 |
| 817bd584 | leaf-risk-qualified | 2 | 0.017 | dc4b4dbcb53041d93ca061d44a5f882c5d6a1f2c5e5fe35f9fd37708aa016350 |

These are 84 successful test executions over **82 distinct case names**; the
two leaf QP analytic/byte-identity checks repeat cases from the earlier risk run.
The three preregistration cases close the earlier aborted fixture qualification
on their explicitly recorded corrected source. They must not be represented as
having passed in the original six-XML run.

The pre-leaf receipt hashes match all three executable hashes independently
captured above before the leaf edit. After the edit/relinks they are historical
receipts; current executable bytes no longer match them. The leaf risk executable
was independently hashed and matches its own receipt:
`5c89b814b98c22ed78a0611a62708737a9de52ccfe21c751fb21112f057f5cc8`.
Current relinked impl-contract hash is
`13fc83d0245489f541db446ece68988bfd97d85174d70d7b5de002cf5005ba89`;
impl-IC is `480089691b43d72e3d4e17381faf9bf6c3a409050a91defff14107dc6db511fa`.
No additional test runs are inferred for those two relinked application binaries.

Final audit conclusion: generated environments/target selection, successful
corrected build, exact no-op, one actual local implementation edit, and bounded
runtime evidence are verified within the attribution above. Initial adoption
was costly and required two repairs. No new alpha result, full W1 wave acceptance,
large-scale performance gate or cold-build improvement is established here.
