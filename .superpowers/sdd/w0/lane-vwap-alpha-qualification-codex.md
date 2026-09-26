# D0 VWAP alpha qualification and incremental build receipt

The final alpha gate passes: **711/711 tests**, plus the explicitly invoked
independent N128 fixture oracle **1/1**. This qualifies the alpha owner and its
frozen benchmark input recipe. Data/book/impl runtime, remaining production
hygiene, L9 remeasurement and the 81-case performance gate remain separate gates.
Implementation preceded these tests; all input here is synthetic.

## Frozen source and runtime

Pool5 source `90d965df7233bdd7e716ccd8a8ae78c72a68817c` imports the six reviewed D0
commits through d40441ad, the a9b3d913 empty-schema test, 520bc0cc test index fix
and 7be27928 streaming fixture raw-basis declaration. No CMake, preset or PCH
ownership changes were imported. The working tree contained only a report edit
while this source was built; no additional production or test changes.

Final executable:
`C:/atx-wt/pool-5/build-equity/bin/atx-engine-alpha-tests.exe`

SHA256 `bb1e6dabd2a2c529cdaad50fd8f0d09993844138de7f5d8cfb974f5ab381a8e1`.

- Whole alpha: 711 tests from 276 suites, **711 passed**, 47.328 seconds, exit 0.
- Explicit `--gtest_filter=VwapLegacyFixture.*`: **1/1 passed**, 1.472 seconds.
  The independently copied b185d056 recipe and current explicit V1 helper match
  every field name/order, f64 bit and mask cell for 2520 dates x 128 instruments.
  Both contain 19 fields and digest `75b4a957ff30e9c3`.
- Native logs: `build-equity/w0-vwap-alpha-final-all.log` and
  `w0-vwap-alpha-final-oracle.log`; source/hash/exit receipt:
  `w0-vwap-alpha-final-tests.receipt.json`.

The test-only streaming correction appends raw_close after its existing RNG
generation, copying the existing close column including holes. It neither changes
the random sequence/mask nor weakens the default requirement for a known raw basis.

## Warm build and resource evidence

The existing Debug equity-dev cache retained groups `alpha;eval;combine`, PCH ON,
and isolated `C:/atx-wt/pool-5/deps/equity-dev`. Both the project and original
alpha-owned test PCH were reused. Target command for every build:

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = '1'
& C:/atx-wt/pool-5/scripts/atx-build.ps1 build -Preset equity-dev -Jobs 1 atx-engine-alpha-tests atx-shm-worker
```

Configure passed with the same groups/dependency directory. An advisory dry plan
contained 77 compiler commands and three links, with no PCH rebuild. The earlier
worktree checkout had refreshed vm.hpp timestamps, explaining extra unchanged
header consumers beyond the D0-only estimate. The plan used a separate ignored
copy of build.ninja with only the VerifyGlobs force edge suppressed, after the
real generated VerifyGlobs check; the real build graph was not edited and actual
builds used the wrapper. Actual log counts include Ninja's ordinary glob edge.

| Attempt | Native outcome | Wrapper wall | Cache calls | Owned process-tree peak RSS |
| --- | --- | --- | --- | --- |
| Initial | Test compile error | 82.922 s | 35 preprocessed hits, 9 misses, 1 failed compile | 1235.87 MiB |
| Index-type repair | Build passed | 68.859 s | 23 preprocessed hits, 10 misses | 399.69 MiB |
| Streaming fixture repair | One test TU + one link passed | 15.203 s | 1 miss | 358.79 MiB |

The first error was an ambiguous `field_name(3)` test argument; 520bc0cc makes all
four new literal field indices explicit usize values, including the impl cases.
The first complete suite then had 707 passes and four streaming corpus failures
because that synthetic generator omitted raw_close. Both failures and their logs
are retained, not counted as passing evidence. After 7be27928 the final suite is
green. The N128 oracle passed both before and after that fixture-only repair.

Per-attempt artifacts use `w0-vwap-alpha`, `w0-vwap-alpha-retry1` and
`w0-vwap-alpha-retry2` prefixes under build-equity, with `-build.log`,
`-build.receipt.json`, `-ccache.statslog`, `-ccache.log` and `-ccache-own.txt`.
There were zero preprocessing errors in the recorded cache attribution.

The original launch had 3.225 GiB available and 4.771 GiB commit headroom. The
parent then permitted correctness launches at >=2 GiB physical and >=3 GiB commit
headroom with zero other compiler workers and exactly one build worker. All three
attempts observed at most one compiler. The largest individual clang RSS was
1110.18 MiB; across attempts host availability remained >=1.685 GiB and commit
headroom >=3.323 GiB. A verified owned-process watchdog would stop only this build
after five consecutive one-second samples below 0.75/1 GiB respectively. No
pressure stop or other-process termination occurred. These are sampled memory
observations, not a universal compiler memory guarantee or speedup comparison.

## Actual unchanged rebuild

After the final fixture build, the same wrapper/targets completed in **3.106138 s**
with `ninja: no work to do.`, exit 0. The executable SHA256 was unchanged and the
fresh per-command CCACHE_STATSLOG file was never created: no compiler-cache call.
Receipts: `w0-vwap-alpha-noop.log`, `w0-vwap-alpha-noop.receipt.json` and the
advisory `w0-vwap-alpha-noop-plan.log`. No source touch or forced compilation was
used to generate cache statistics. This proves an actual same-tree no-op only;
it does not compare cold versus warm performance.

The Release benchmark tree and its frozen flags were not configured or compiled
during this alpha gate. Its current executable still requires D0 rebuilding before
eligible timings. No benchmark was launched during this qualification.
