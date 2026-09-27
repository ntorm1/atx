# D0 raw VWAP: final owning-target correctness closure

## Outcome

Correctness closure PASSED: the four affected targets have **1,683 passes, six
documented skips and zero failures**. The corrected L9 numerical measurement and
quiet performance comparison remain open; this is not a W0 wave-gate approval.
Implementation preceded tests. No real data was opened by these synthetic gates.

## Frozen source and implementation

Root `feat/aes-codex-integration-20260925` at
`fcbcc9d1a351ccd339687389b118aca65e2812b5`, pool-2, lease
`aes-codex-integ-20260925`. Later root changes are reports and preparation only.

Production imports: `f54b55e5`, `2a7194d5`, `0f74a357`, `c1011024`; the separately
identified capacity resume identity repair is `0037c515`. Tests/fixtures:
`58df3e29`, `c68b78a4`, `1e4ee053`, `efe57fd9`; other benchmark legacy pins
`9c2ffd1f`. Source review `2cbc935a`, final alpha evidence `033b89c2`, owner-scope
audit `f1620990`, integrated include evidence `63d28b5c`.

RawDailyCloseV2 now requires known raw prices, finite positive price/volume and
valid geometry before indexing; it replaces stale VWAP and avoids TRI scaling.
The context interface, move state, CLI, recipes, reports and active discovery
identity carry the rule. Explicit AdjustedTypicalV1 preserves the former numeric
recipe. This is a raw daily-close proxy, not observed intraday VWAP. Adjusted OHLC
and mixed-basis lint remain W2-A3; no tradeable-alpha or complete causal-panel
claim follows from this scoped correction.

## Build and incremental evidence

From pool-2, exit 0:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 build -Preset equity-dev -Jobs 1 atx-engine-data-tests atx-engine-book-tests atx-impl-tests atx-shm-worker
```

Session99434 completed in **991.797s**: 163 compiler actions and six links, plus
the ordinary glob check. This included first-use stable common/data test-PCH
carriers in this tree and changed shared-header consumers. All163 cache calls
were misses; zero preprocessing errors. Do not describe this build as cache hits
or a controlled speedup. Debug/static/PCH-on/precise-FP settings and isolated
`deps/equity-dev` were retained.

Native Ninja argv had `-j 1`. The monitor's non-atomic process scan reached two
clang processes once, which does not establish simultaneous separate Ninja
workers; bounded independent snapshots saw zero/one/one. Sampled owned-tree peak
RSS was 1,607.4375MiB, minimum available physical RAM 1.078415GiB and minimum
commit headroom 3.210178GiB. No pressure termination occurred.

An exact unchanged repeat exited0 in **5.047s**, printed
`ninja: no work to do.`, and created no compiler-cache log. It did not replace
the original build receipt. The separate warm alpha repeat was 3.106138s with
zero cache calls and an unchanged executable hash (`033b89c2`).

Receipts: `pool-2/build-equity/w0-vwap-closure/`, including `build-result.json`,
per-attempt native/cache/resource logs, `compiler-snapshots.json` and the no-op
receipt. Diagnostic dry attempts never compiled: PowerShell consumed the native
`--` separator, Ninja's dry glob edge requested regeneration, and this bundled
Ninja does not support `-d norebuild`. The actual build used the normal wrapper.

## Runtime results

Root runner: `build-equity/w0-vwap-closure/run-tests.ps1`, native exit0. It bound
source/executable/log hashes, cleared external-data/nightly opt-ins, preserved
the prior data-fixture exclusions and wrote GoogleTest JSON for every target.

| Target | Passed | Skipped | Failed | Wall seconds |
|---|---:|---:|---:|---:|
| Alpha (independent pool-5 qualification) | 711 | 0 | 0 | 47.328 |
| Data (same external-fixture exclusions) | 239 | 1 | 0 | 9.348 |
| Book | 128 | 0 | 0 | 7.817 |
| Impl | 605 | 5 | 0 | 252.852 |

Data's existing skip is `DataUniverse.SurvivorshipCaveatDocumentedOrDeferred`:
the optional reference document is absent. Impl's existing skips are
`Alpha101Orats.RankBySharpeRiskParity`, `Alpha101Orats.RankByOptimizerSharpe`,
`AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha`, `FundamentalZoo.RealDataIcReport`
and `SingleAlphaCapacity.SweepAndVerify`. No new skip or acceptance weakening.
The exact external-data filter is in the runner's receipt and `data-exclusions.txt`,
SHA256 `2af3ee16dbacbd4dc414a5dd6c25ff83043f89f87746fce3692a9e209fd64d53`.

| Executable | SHA256 |
|---|---|
| Data | `333899846fe011043cc72d74ac37f9b34071e385fee9a26a63beff55b3e717f6` |
| Book | `0db53c7b07dcff4062b093cadca243a8bf6acdd6eff3555d46ba396368122f06` |
| Impl | `cc0054b5b527a5fa4011c0fa6d5130981839d44680c0d775a35d9561c28fb133` |

The independent 2520x128 legacy fixture oracle also passed1/1, comparing every
field name/order, mask and f64 bit against the frozen pre-D0 recipe. Digest:
`75b4a957ff30e9c3`. Its expected side does not call current V1. Alpha's initial
test index-overload error and four missing synthetic raw-basis failures, with
their fixture-only fixes, remain documented in `lane-vwap-alpha-qualification-codex.md`.
The remaining12 engine groups have no new default-policy runtime owner; the two
parallel fixtures use the explicitly preserved V1 helper. No broad rebuild of
those groups was needed (`f1620990`).

## Scoped include evidence

Root's ordinary successful impl build already compiled config, mining, panel,
discovery and progress-sink production objects with no PCH. Their exact configured
commands/hashes are bound in `impl-no-pch-commands.json`; no duplicate large-TU
compile was performed solely for hygiene.

The retained pool-4 real_panel PCH-off check is reusable: its source and34 tracked
header inputs are unchanged. For context.cpp, the first local check/repeat was
insufficient for complete integrated parity (83/84 inputs matched; VM was older).
Root's attempted own-tree check stopped before compilation because that hygiene
tree did not exist. Pool-4 then imported the already-reviewed root VM verbatim at
`3099437575ae2774f5f62ae79c4c70815c6fbc15` and ran the one-object wrapper check.
Authoritative native exit0,16.473s; **all84 first-party inputs match fcbcc9d1**.
Fresh dependency extraction reports347 valid total inputs. Report `63d28b5c`
preserves the earlier first-run null exit-code capture and parity limitation.
No full-tree hygiene or every-header-alone claim is made.

## Remaining wave gates

Only full L9 needs corrected numerical evidence; the impact contract at `35c68fe6`
reuses all14 unaugmented contexts and unaffected L7/L10/cp21/native replay results.
Final Release builds and L9 are separately scheduled. All81 benchmark cases,
three repetitions, frozen N128 width and the20% regression gate remain required
on a quiet host. The previous contaminated baseline attempt is invalid, not a pass.
