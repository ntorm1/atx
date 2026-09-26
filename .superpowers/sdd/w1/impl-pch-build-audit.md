# Application PCH and incremental-build audit

Source-only change: `f956586d8e3778238b7d89598774b9ec541a8244`.
Audited build: pool-2 `2e3981011478085829596143ebbd905d1283697a`;
the original attempted build was `948498c2517e64936a4572736caaa5420682f449`.
No configure, compiler, cache mutation, benchmark, or numerical run was performed
by this audit. Runtime compatibility and speed improvement remain unqualified.

## Observed build, not an inferred cache failure

All artifact paths below are under `C:/atx-wt/pool-2/build-equity/`.
The initial integrated receipt records Jobs=2, 419.7470873 seconds, exit 1.
Its start receipt records 1728 MiB physical memory and 4.401081 GiB commit
headroom. The native log stops on the test-only missing `<fstream>` include in
`provenance_test.cpp`; this was not an engine failure or an attributed ccache
preprocessing failure. There is no isolated cache log for that original build.

The retained dry plan contains 109 objects and nine links, with no PCH creation:

| Target family | Objects |
|---|---:|
| tsdb | 2 |
| engine production | 43 |
| impl production | 29 |
| engine IC / W1 eval / W1 risk / W1 data | 2 / 7 / 7 / 2 |
| impl IC / W1 contracts | 7 / 10 |

After the source include repair, the receipt for the same six requested targets
records native 0, Jobs=3, 55.9271327 seconds. Their subsequent exact no-op records
native 0, 3.5618925 seconds, no cache-log creation, and `ninja: no work to do`.
Unchanged targets rebuilding spuriously was therefore not reproduced.

Stored Ninja dependencies show genuine shared-header fanout. Counts intersect
the 109-object dry plan and overlap; they must not be added together:

| Changed header | Stored dependent objects | In the dry plan |
|---|---:|---:|
| eval/cpcv.hpp | 134 | 64 |
| eval/cpcv_date.hpp | 39 | 38 |
| tsdb/mapping.hpp | 107 | 37 |
| impl/src/config.hpp | 106 | 35 |
| risk/estimator_policy.hpp | 34 | 34 |
| risk/exposures.hpp | 132 | 33 |
| risk/factor_model.hpp | 125 | 31 |
| data/panel_store.hpp | 26 | 26 |
| impl/src/panel_artifact.hpp | 39 | 24 |

These were read from `ninja -t deps` after the resume, against headers changed
between `6debcc10` and `948498c2`; they identify dependency routes rather than
exclusive historical rebuild causation. Root owns configuration-type separation
to reduce this fanout.

The latest `.ninja_log` interval for each planned object totals approximately
256.18 worker-seconds for the 29 impl production objects and 131.79 for its 17
focused test objects. Some entries came from the resumed build. These are neither
wall-time attribution nor a claim that PCH can save all 387.97 worker-seconds.

## Implemented change and command compatibility

All 46 impl objects in the plan lacked `/Yu`, `/Fp` and `/FI`, despite the global
`ATX_USE_PCH=ON` setting. The engine already had warm PCH carriers, which this
change does not alter.

The existing `compile_commands.json`, normalized only by removing source,
`/Fo` and `/Fd` arguments, contains:

| Target | Commands | Distinct environments |
|---|---:|---:|
| atx-impl-core | 35 | 2: 34 ordinary; one stage_discover SHA definition |
| atx-impl-tests | 95 | 1 |
| atx-impl-ic-screen-tests | 7 | 1 |
| atx-impl-w1-contract-tests | 10 | 1 |

The three test environments are identical to each other, including GTest,
miniz, JSON, Threads, warnings and `ATX_IMPL_TESTS_DIR`; their common normalized
SHA256 is `7bf7b9b80321ae0e2f405b4cd03da52896a79a230c9c044add8cec5d1aa67d3f`.
They differ from production, so blindly reusing the engine or production PCH
would not satisfy CMake's matching-options/flags/definitions contract (verified
using installed `cmake --help-command target_precompile_headers`).

`atx-impl-core` now owns a PRIVATE PCH containing Eigen, JSON and stable standard
headers. It contains no engine, pipeline, configuration or provenance headers.
The Git SHA remains source-local to `stage_discover.cpp`; its extra macro is not
used by this payload. No first-party Eigen/JSON/GTest configuration `#define` or
`#undef` was found in the application or public core/engine/tsdb headers.

The ordinary and both focused test targets now use one compile-environment
helper and reuse an EXCLUDE_FROM_ALL object carrier with the same environment.
The test payload adds GTest to the stable production header list. The carrier
has no test registration, scratch initializer or worker dependency. Scratch
objects remain linked into each executable; the existing worker dependency
remains attached only to the ordinary suite. PRE_TEST discovery is unchanged.
Both PCH paths are disabled by the existing `ATX_USE_PCH=OFF` hygiene setting.

No source lists, optimization/debug flags, warnings, runtime library, engine
PCH, launcher, cache options, or global CMake files changed. Root's newer prereg
source/test registrations are outside the patch hunks and must be retained on
import. Validation was source inspection, existing command-class comparison and
`git diff --check`; new generated command compatibility still needs the single
combined parent configure/build.

Initial adoption necessarily creates two PCHs and one trivial carrier object,
and recompiles the requested impl consumers with the new PCH flags. Do not use
that one-time cost as a warm-edit speed measurement. Only requested targets
build: this does not force the ordinary 95-test suite into a focused invocation.

## Deferred duplicate-source optimization

The configured graph has 67 focused compile entries, each with a distinct source;
62 also appear in an ordinary suite. Of those duplicates, 57 have compatible
commands after source/output normalization. Five data tests use different
miniz/PCH environments. None is duplicated among the six targets in the recorded
build, so this is not an explanation for its 109 objects.

Any future shared-object solution should preserve selection at the source level;
linking a whole ordinary group object library into a focused target would force
unrequested test compilation. Root explicitly deferred this separate change
until the PCH/configuration refactor is measured.

## Evidence bindings

| File | SHA256 |
|---|---|
| w1-next-integrated-advisory.log | 86f8f5f2927f98113ae8ff9afaacad021f3c260d69366a60ff9b01c9f36ed8a3 |
| w1-next-integrated-build.log | fea1ec4e878aa302d67a874f594a6b7d9d367592577b9a3881804427931b8ae2 |
| w1-next-integrated-build-receipt.json | e6cb563f36e25d55c64ee361784c0ad2393dda4affefab5412921e6201a983e6 |
| w1-next-integrated-start.json | eae18224958f546a84f43b03d3b7de8c516b8d31b426abf7998acd86c6593777 |
| w1-next-final-build-receipt.json | ba8a1b7e408031454de11b5cc199c8803e44e708a71c07909ab163397c28e00b |
| w1-next-noop-receipt.json | 18513052f64561eac3a4af1d4e466875d5a089ea5460ccbcfd80767ad9ef42ea |
| w1-next-noop-build.log | 985287629e7861d89ea28a8cb0092f5c6a5d42b0b75c955c08f34e6be00a2109 |
| compile_commands.json at audit time | ce49a5958e0604d2644013b800f790a6d3543c68d1e61931416302223d50daf7 |

The command database, Ninja dependency database and `.ninja_log` are live build
metadata; subsequent root configuration/builds will legitimately change them.
