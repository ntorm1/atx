# W0 benchmark preflight and incremental-build evidence

Status: both original binaries built; first baseline kernel timing completed but
was invalidated for host contention. No eligible performance result exists.
Final-current snapshot awaits the narrow D0 VWAP correction. This report supersedes
the initial preparation estimates. Root owns the quiet-window release.

## Frozen comparable builds

Baseline is O1 `3ccf012c40ef42c49ed21aaec96476455e06e049` in leased pool6.
Current production is `b185d056440704e7ebcfe2b9395601d7e5264269`, compiled from
docs-only child `766bac4a991b6101d0de4209f79157b859dcbd62` in leased pool5. Later
review-report commits do not alter that executable. Root's later ASan opt-in and
impl summary-disclosure changes leave the selected benchmark production paths
unchanged; these benchmark binaries are frozen and do not import incremental-build
patches or new compiler flags.

Both equity-bench builds passed with isolated deps/equity-bench, Release
`/O2 /Ob2 /DNDEBUG`, PCH ON, static libraries, no global AVX2/IPO, and the same
`atx-engine-bench` plus sibling `atx-shm-worker` targets. The normalized 161
production/benchmark compile commands and relevant cache settings match exactly.
The planned 235-action graph contains 224 compiler calls. Build log spans were
approximately 19.8 minutes baseline and 22.8 minutes current; host/source differences
make these unsuitable for a compiler speedup claim.

| Binary | SHA256 |
|---|---|
| Baseline benchmark | `8bd72411065d71edbdd62eb37ffc2230aee0bf050681aa2096d85b5f04eea966` |
| Current benchmark | `d7b2490a1d099e1de37732a3d38bb831ac2148ccbea5b816331416f05ec4904d` |
| Baseline worker | `1fde832f0b998e67f615ab2f1b37700728115a315077a72d841dae5e89e23e33` |
| Current worker | `a148e0844c642c1575c56d0b42f9ce9fbc44afbe0cdc3ef0ba009f9959258e20` |

Both native registry listings passed with identical 81 names/order: 33 kernels,
20 WQ101, 15 SearchThroughput, nine optimizer and four scalar/multiobjective search.
Receipts are each pool's `build-equity-bench/w0-{baseline,current}-registry.json`,
pool5's `w0-registry-pair.json` and `w0-build-comparability.json`.

## Actual incremental and cache proof

The current build's own ccache statslog records 224 cacheable calls, 84 direct hits
(37.5%), 140 misses, and zero preprocessing or compilation failures. The machine's
older cumulative preprocessing-error count is not attributed to this build.
Independent G0 inspection classified the misses: 64 changed PCH sizes, 51 changed
source headers, 25 absent direct manifests. This is a new worktree/source snapshot,
not evidence of broken reuse of an unchanged same-tree build.

The actual same-tree wrapper repeat of benchmark+worker returned exit 0 in
3.4759664 seconds with `ninja: no work to do`, zero compile/link actions, unchanged
executable SHA and unchanged ccache statslog SHA. The ordinary generated
VerifyGlobs check did not change its stamp, CMake cache, Ninja file or clean source.
See pool5 `w0-current-noop.receipt.json`, `w0-current-noop.log`,
`w0-current-noop-verify-receipt.json`, `w0-current-ccache-own.{json,txt}` and the
per-build `w0-current-ccache.{statslog,log}`. No source-touch cache probe was used.

## Memory model and workload choice before timing

The agent-selected original WQ500 setting was not a W0 plan floor. The authoritative
swarm plan's wave gate requires the benchmark comparison with no regression above
20%; its host rules require equity-bench on a quiet host. Separate later tasks own
3000-name production/large synthetic scale gates. The user explicitly authorized
RAM workarounds. Root approved WQ128, with all 2520 dates, 70 formulas, worker
variants, 81 cases and three repetitions preserved. Search remains 756×500 with
six generations; optimizer sizes remain 1000/3000/5000.

Source-only accounting mirrors this battery's numeric folding, scale default,
pow-to-square rewrite, opcode alias/CSE identity, emitted child refcounts, slot
retirement, ASAP node levels, ALAP leaves, last-consumer release, direct root
outputs and scratch free-list retention. It gives 617 live union nodes, 549
cacheable nodes, 52 serial union slots, at most nine slots per individual alpha,
16 DAG levels and 277 retained whole-panel scratch allocations. The parser, DAG,
bytecode and global-DAG sources are unchanged from O1 to the current production
freeze. This independent source model is not engine-reported metadata or measured
RSS; it is specific to the frozen battery and has not been generalized as a parser.

For N instruments, let B = 2520*N*8 bytes. Panel storage is 19*B plus 2520*N mask
bytes; 70 roots require 70*B; the scratch free list keeps 277*B allocated after
last-use retirement. Cache occupancy is min(549, floor(2 GiB/B))*B. One publishing
copy can coexist before eviction. Thus dominant cold warmup storage is
`(19 + 70 + 277 + min(549, floor(2 GiB/B)) + 1)*B + 2520*N`.

These terms are not aliases: the panel owns source fields, roots own output
vectors, the DAG owns reusable intermediates, and cache publication copies them.
Root outputs write directly into their first output vectors and are excluded
from the 277 scratch count, avoiding double counting. The first cold pass has no
cache-hit shared_ptr pins; later warm passes find all roots and prune their cones.
Cache and scratch coexist until the cold global-DAG call returns. In StrategyA,
16 workers can transiently retain old eight-slot pools while allocating new
nine-slot pools: at most 16*17*B for pools plus outputs/panel. This is below the
cold cached-DAG payload at the proposed reduced sizes.

| WQ names | Cache payload GiB | Dominant warmup GiB |
|---:|---:|---:|
| 500 | 1.9996 | 5.4461 |
| 192 | 1.9791 | 3.3025 |
| 128 | 1.3194 | 2.2017 |
| 96 | 0.9895 | 1.6513 |
| 64 | 0.6597 | 1.1008 |

For WQ500, 4–5 GiB free is insufficient; a practical launch policy would require
6.5 GiB with monitoring. WQ128 uses a 3 GiB launch floor. Root approved deterministic
fallback WQ96 at 2.25 GiB, selected once before ANY timing if WQ128 cannot safely
launch. N64
was rejected as a coverage choice: `IndClass.subindustry=j%71` makes all groups
singletons and alpha #48's within-subindustry neutralization degenerates. N96
retains 25 paired subindustries; N128 retains 57. Neither is a production-width
or worker-saturation claim. Both reduced sizes retain every modeled cacheable
node; 500-name cache-eviction pressure remains explicitly unmeasured.

DLL/runtime metadata, kernel scratch, thread stacks and allocator behavior are
additional to the table, so it is not a hard RSS cap. Raw kernel live arrays use
about 35 MiB; search fixtures are smaller than WQ and use no 2 GiB subtree cache.
The optimizer is a sparse 64-factor augmented solve, not a dense M² covariance.
Five separate family processes prevent fixture/allocator retention from an earlier
family accumulating into later families. Native memory monitoring remains required.
Source model/helper receipts are pool5 `build-equity-bench/w0-static-memory.{py,json}`;
the JSON records source hashes and per-alpha counts. No engine helper was compiled
and no numerical benchmark was run to produce the model.

## Final protocol pending quiet release

N128 was selected and frozen before the first timing at a fresh 3.902 GiB available
with no compiler/test/engine process active. It remains fixed even though the first
batch was invalidated; the unused N96 fallback is no longer a selection option.
Receipt: pool5 `build-equity-bench/w0-protocol-freeze.json`. For families `kernels`, `wq`,
`throughput`, `optimizer`, `search`, run all five baseline families, then all five
final-current families serially, three
repetitions each, P-core affinity 0xFF verified through Windows processor topology.
Root approved this sequencing before timings so baseline work can proceed during
the current-only VWAP repair's source work. No compilation or test process may
overlap either snapshot's measurements; current requires a fresh quiet check after
its final build. This is a sequential pair, not an interleaved measurement design.
The native filters are disjoint and their union is the exact original 81-case
registry. The runner removes inherited ATX variables and pins identical dimensions.
At one-second intervals it records process peak working set/private memory,
page faults and host minimum available memory. Two successive samples below
256 MiB available abort the owned process and mark failed resource evidence.

Exact command form (repeat per family and snapshot, no source/build mutation):

```powershell
python C:/atx-wt/pool-5/build-equity-bench/w0-run-bench.py C:/atx-wt/pool-6 w0-baseline-wqN-FAMILY --registry C:/atx-wt/pool-6/build-equity-bench/w0-baseline-registry.json --compiled-source-head 3ccf012c40ef42c49ed21aaec96476455e06e049 --wq-instruments N --family FAMILY --execute-after-quiet-release
python C:/atx-wt/pool-5/build-equity-bench/w0-run-bench.py C:/atx-wt/pool-5 w0-current-wqN-FAMILY --registry C:/atx-wt/pool-5/build-equity-bench/w0-current-registry.json --compiled-source-head 766bac4a991b6101d0de4209f79157b859dcbd62 --wq-instruments N --family FAMILY --execute-after-quiet-release
```

Keep every native JSON, context, stdout/stderr and receipt. `w0-merge-bench.py`
concatenates unchanged rows after checking five families, exits and common source/
dimensions/affinity. `w0-validate-bench.py` checks the exact 81-case union, three
iteration indices per case, positive finite times, no native errors/skips and each
median independently; paired dimensions must match. Only then run the existing
`scripts/bench-gate.ps1 -Threshold 0.20` without AllowMissing or Update.

Budget approximately 25–45 minutes for the pair; SearchThroughput and optimizer
setup remain unchanged and can dominate. This is a scheduling estimate, not timing
evidence. No performance gate or production-scale acceptance is claimed yet.

## First timing invalidated; window released for D0 qualification

The baseline kernel child PID16448 ran 2026-09-26 00:34:16.902–00:36:43.609 UTC,
native exit 0 in 146.701 seconds. Peak working set was only 50,921,472 bytes,
but minimum host available memory fell to 428,990,464 bytes and the process
reported 505,741 page faults. A mid-run nominal two-second process sample showed
many competing Python/PowerShell/Claude processes consuming CPU. Native repetition
variability increased (e.g. order-stat batch /52/250 real-time CV43.94%). These
timings are ineligible even though native execution returned success.

No process was killed: the owned benchmark had already exited before the verified
stop attempt. Other process ownership was not established; root disclosed a
metadata-only Git-SHA Python loop, which may account for one sampled Python.
The explicit `w0-baseline-wq128-kernels.invalidity.json` marker accompanies the
native JSON/stdout/stderr/receipt in pool6's build-equity-bench. It records sampled
PIDs and CPU deltas; pool5's `w0-baseline-contamination-process-ownership.json`
contains surviving PID/parent/executable metadata. The merge helper rejects any
native batch with an invalidity marker. No automatic timing retry is authorized;
the quiet window was released for D0 implementation/qualification. N128, 81 cases,
three repetitions and the 20% threshold remain unchanged.
