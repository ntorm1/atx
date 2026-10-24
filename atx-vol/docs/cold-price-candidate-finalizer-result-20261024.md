# Deferred candidate proofs: complete SPY correctness result

The explicit enabled experiment at `7037c0e194ed84587e73852916d8eb0c95f5ddbc`
passes all 13 scalar actual-API gates. Its complete prediction CSV is byte-identical
to the original candidate-state-machine capture. All 33 slices and 12,866 eligible
rows are served; 12,851 rows are inside their original boxes (99.8834136484%).
There are 12,850 identified rows and the same 16 genuine PriceOnly rows.

Root and calendar independently approved the exact implementation before this
single complete-input run. The [implementation review](cold-price-candidate-finalizer-20261024.md)
and [original 700-call capture](cold-price-candidate-capture-review-20261024.md)
define its scope. Both empirical options remain default OFF. This is a
workload-qualified empirical target experiment, not a generic interval proof or
a production-default change.

## Complete work and outcome comparison

The comparison sums each `graded_stats.slices[].timing` leaf once. All original
nonclock counters remain identical except the four entries explicitly shown below.
No candidate, direct seed, refusal, retry or final row is removed.

| Work / outcome | Original | Deferred experiment |
|---|---:|---:|
| Candidate attempts / accepted roots | 700 / 684 | 700 / 684 |
| Screen attempts / accepted stops | 700 / 684 | 700 / 684 |
| Remaining Fast continuations / accepted roots | 16 / 0 | 16 / 0 |
| Direct seed attempts / accepted roots | 70 / 61 | 70 / 61 |
| Candidate-stage IV iterations | 1,437 | 1,437 |
| Candidate-stage boundary solves | 1,158 | 1,185 |
| Direct-seed-stage boundary solves | 123 | 124 |
| All counted AL price calls | 5,779 | 5,807 |
| Certify-bucket AL calls | 1,820 | 1,848 |

The enabled finalizer makes 14 component calls for all 700 pending candidates.
Its 684 Screen seeds yield 333 empirical vector admissions and 351 scalar
admissions, matching the separate frozen proof replay. Ninety vector packs
contain 360 lanes; 27 attempted vector lanes require scalar patching. The same
16 candidates without accepted Screen seeds continue through the original Fast
and typed Search path. No allocation refusal, setup fallback or complete scalar
fallback occurs in this run.

The component proof owner records 263 floor preparations and 421 hits; the Fast
continuation owner records zero of each. The separate publication owner records
two preparations and 59 hits for accepted direct seeds. The deliberate owner
reordering adds one direct-seed boundary solve in chain `7550940946639438750`
(`T=0.25411561131477356`, 40 direct seeds, all accepted). This work is charged.
The 27 extra candidate boundary solves are the vector attempts subsequently
patched by the scalar route. Prepared-seed work remains 496 requests, 480 vector
roots and 16 scalar roots, with the same 11 preparations and 485 floor hits.

The outer four-buffer allocation fallback remains source-reviewed, rather than
fault-injected. Separate component setup, optional-cache and synthetic cold-floor
exception controls retain their previous passing receipts. The complete SPY run
does not exercise those exceptional paths.

## Actual API and accuracy

The scalar Reference exporter is unchanged. Every eligible prediction and its
status, interval, convention and carry metadata matches the actual API. Shape,
call-put, calendar, terminal-floor and between-node checks pass. Every identified
target clears its independent zero-volatility band and scalar `P(sigma=0.005)`.

The maximum cold scalar Reference price residual remains
`0.0005012966094284366` dollars per share, within the explicitly approved
`min(0.001, 0.01 * original bid-ask spread)` contract. Original boxes are unchanged.
The same two previously reviewed floor transitions remain explicit in the
identification contract. Strict `1e-7` IV diagnostics still show 757 strict
straddles and 12,093 failures; these are preserved, not relabeled as passing.
No Greek accuracy follows from this price result.

## Reproducibility and clock limits

Evidence directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/candidate-finalizer-v1/`.

| Artifact | SHA256 |
|---|---|
| Archived executable | `e3798becf74950dc7b74b2b6441eec544a3f424b0bb9047845d197be3fe4986c` |
| Canonical LF policy | `781ddc09f7f26cfbfeb6c32edae1e3591b725a8f8d8d5b85d18eeda00b23f34a` |
| Native actual-API report | `4e59e20c01173a0545fda3112c0898083ddee4a7456a1550a3be71a460dd6f49` |
| Complete predictions | `e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f` |
| Independent verification | `84582f1d6983d132a2431b9eb9a8ab3716ae372738a91636d0ffbdf78c6cd830` |
| All counter comparisons | `fe519c0e87b94f3fd366628ccce4bc5a68bdcb4ea0b042ed4622963a4283efd7` |
| Eleven-artifact evidence manifest | `bc1337e980c23886898b4b894d4e23ff358efcae7dd72d5c7c301248420d5ddb` |

`archive.json` records clean source, the archived executable and the copied build
cache before launch. The build is Release AVX2 without IPO, with both empirical
experiments enabled. Candidate capture is compiled in but no capture flag or
collector is activated. `full-audit/receipt.json` records the exact command,
single fit worker, input/policy/binary hashes and successful exit.

The diagnostic component clock is 135.0105 ms inclusive: 34.6768 ms proposal,
100.2002 ms proof, 0.0025 ms continuation and 0.0532 ms setup. Inside the proof
clock, vector/scalar/floor values are 36.8964/42.4985/20.6297 ms. Finalization is
172.4835 ms, including prefix/direct-seed work, outer storage, typed Search and
destruction. These clocks overlap; they must not be added as independent stages.
They are neither quiet-host nor ThinLTO measurements. The earlier baseline's
different wall times establish no speedup.

Still required for a performance conclusion: archive the enabled ThinLTO binary,
repeat its own scalar actual-API gate, then compare complete fits against an
archived control under valid physical-core and whole-host quiet gates. No
subsecond result or qualified throughput improvement is established here.

## Exact ThinLTO archive follow-up

The enabled Release AVX2 ThinLTO build also passes all 13 scalar actual-API
gates. Its complete prediction bytes and all 164 nonclock timing-counter fields
exactly match the development result above. Both capture switches are OFF;
both empirical switches are ON; the research QP library setting is empty.

Before timing, source comparison caught that this research branch predated the
reviewed strict CPU-counter interval guard `5ea67439`. The first ThinLTO archive
`candidate-finalizer-lto-v1` passes its correctness audit but remains unsuitable
for timing admission. Its executable and receipts are retained unchanged. No
timing was attempted with it.

The guard was cherry-picked as `e209692bd0ef3e0c94bf08371d749fffcdab3fca`, with
only an append-only ledger conflict. Clock source/header/test blobs match the
reviewed guard exactly; all 20 owning clock tests pass. Pricing and finalizer
numerics remain `7037c0e1`. The replacement executable was archived before its
own full audit, rather than inferring correctness from the earlier binary.

Evidence directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/candidate-finalizer-lto-v2/`.

| Artifact | SHA256 |
|---|---|
| Guarded ThinLTO executable | `7c44a1b2a206b6f9a920bd7e8d19e445dcaf062e3c53bc990a32f48442ca69c6` |
| Native actual-API report | `2d7303e9a1d19352676ba68ddd3deced0ef2ba73316747be1906d6623c84be9a` |
| Fourteen-artifact evidence manifest | `9d57bbe302a8fd4e71cadb3bd403ba11570192591256f0d75fb3a6b370814939` |

The manifest retains the copied build cache, build log, 20-test XML/log, exact
policy, command/exit receipt, complete output and independent verification.
The canonical policy and prediction hashes remain those above. This closes
the exact-release-binary correctness step only. A qualified quiet-host
comparison and any subsecond or speedup claim remain outstanding.

## Matching control and blocked paired attempt

A matching ThinLTO control at `6f94b783` disables both empirical switches.
Its source differs from the enabled `e209692b` archive only in two documentation
files; its entire saved CMake cache differs only in those two switches. Captures
remain OFF, the QP research setting remains empty, and the strict CPU guard is
identical. Its own 13 actual-API gates pass with the same complete `e086` output.
All 209 timing/work leaves, including every differing route counter, are retained.

The control executable is
`candidate-finalizer-lto-control-v1/bin/atx-vol-quote-bench.exe`, SHA256
`479faf0e37b31ae7bf40def20f1f65cd5694f1507e4355d60fd2243abd352407`.
Its 14-artifact manifest is
`0e7cafb35ae740348241762c25027122a1fc9c73a242990c2954901efca3f656`.
Both directories are under the acceptance evidence root above.

The separately reviewed paired plan fixes six fresh processes in
OFF/ON/ON/OFF/OFF/ON order, worker 1 on P-core logical CPU 6, no warmup or
replacement, strict sibling load below 5% and whole-system load below 10%.
It requires three valid one-second host-load samples plus zero detected
native/build jobs before any fit is launched. Failed trials remain retained;
detected competing jobs stop further launches.

The single coordinated attempt was blocked by its process check before CPU
sampling: `atx-vol-state-store` PID 4852, `clang-cl` PID 1848, `cmake` PID 7856
and `ninja` PID 12504. Zero CPU intervals and zero fitting trials ran. All lane
holds were released; no process was modified and no retry was substituted.
`candidate-paired-clock-v1/manifest.json` has SHA256
`f81ea988eaf130d6cf3e39aa20bfd40603b22b9ee5ee1137ffba3f5871861b8c`;
its separate blocked receipt is
`bd2dc4c3bb474c2456d8766ae020268e4aea6dbe799ff0be2207e38ccc35c745`.

The wrapper's terminal summary displayed null fields because PowerShell
`Select-Object` did not expose the ordered dictionary's keys. The saved JSON
correctly retained the refusal and empty trial list. An unexecuted future
plan-v2 changes only that display cast, with a reproducing control; the frozen
plan-v1 and blocked result remain unchanged. No performance comparison follows
from this blocked attempt.
