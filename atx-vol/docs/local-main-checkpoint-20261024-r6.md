# Local main candidate-proof and reference checkpoint, 2026-10-24

This user-requested checkpoint advances local main from `5df44c59` with reviewed
candidate-proof batching, final-price wiring, independent cash-reference work
and the offline LCP correction primitive. The final tested combined source is
`92547728`; this document is the subsequent checkpoint record. No remote push
or production-default promotion is included.

The candidate implementation shares the original Screen/Fast proposal sequence
and typed Search continuation, batches only pending cold price proofs, retains
the native and direct-seed routes, and charges setup, floors, retries and storage
destruction. Proof-stage allocation failure is contained per lane. Both
`ATX_VOL_EMPIRICAL_VECTOR_SEEDS` and `ATX_VOL_EMPIRICAL_CANDIDATE_PROOFS` remain
default OFF. Capture and independent reference targets also remain opt-in.

The enabled component at `7037c0e1` was independently reviewed before its complete
SPY run. It preserves all 700 candidate attempts, 684 acceptances, 16 refusals,
and the preceding 70 direct-seed attempts/61 acceptances. The 333 vector and
351 scalar proof admissions match the earlier replay. Extra scalar patches and
independent floor-owner scheduling add 28 counted boundary calls; these are
retained, not hidden. See `cold-price-candidate-finalizer-result-20261024.md`.
The outer four-buffer allocation fallback is source-reviewed, not specifically
fault-injected; the component's separate allocation controls are exercised.

The final combined default build separately passes all 13 scalar actual-API
SPY gates. It serves all 33 valid expiries and all 12,866 eligible rows, with
12,851 inside their original boxes (99.8834136484%), 12,850 identified IVs and
16 truthful PriceOnly rows. Every prediction and its metadata match the actual
API. The complete prediction hash remains
`e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f`.
The maximum cold Reference price residual remains $0.0005012966094284366/share,
within `min($0.001/share, 1% of original bid-ask spread)`. Shape, parity,
calendar, terminal-floor, containment and identification requirements are intact.

The independent cash work retains its earlier negative studies. Its local-density
cubic-weight successor now computes all 24 requests and passes all 12 unchanged
precision checks and 168 independent quadrature comparisons. Eight outcomes use
the local series; sixteen preserve the original-route outputs exactly. This
resolves three adjacent-cell component failures, not complete American cash
accuracy. The representation/full-tail Stage A and later option study remain
open. See `cash-cubic-local-weight-review-20261024.md`.

The offline one-correction LCP primitive retains the original solver as default,
the original complementarity tolerance and bounded policy iteration. The captured
failing stage converges in four solves with residual 1.99e-12 at tolerance 1e-10.
The twelve-grid follow-up and any new complete Greek study remain separate;
historical reference decisions are unchanged.

Combined validation uses Release `vol-rel`, without a latency claim:

| Closure | Passed | Skipped | Failed |
|---|---:|---:|---:|
| American IV pricing | 102 | 7 | 2 existing |
| Allocation controls | 6 | 1 | 0 |
| Complete quote-slice executable | 360 | 6 | 0 |
| Cash Gaussian components | 45 | 0 | 0 |
| Python research/readers/native CLI | 122 | 0 | 0 |

All three assertions in the two pricing failures reproduce exactly against a
fresh build of prior-main C++ source: `PricePolishDefaultOffIsBitForBitControl`
has the same one/two-ULP golden mismatch, and
`PricePolishKeepsCollapsedVegaProtection` still observes zero reverted bolts.
No pin or tolerance was changed. Instrumentation/capture and external-fixture
skips are retained. Reviewed component owning translation units pass PCH-off
checks; final numerical Git blobs match those reviewed sources.

Integration exposed one checkout defect: Windows CRLF conversion changed the
raw bytes of the hash-pinned original cubic reader. A nested `.gitattributes`
rule forces LF for that file only. A fresh `core.autocrlf=true` checkout matches
the original Git blob exactly, and all fourteen cubic reader tests then pass.
The failed attempts and original pin are preserved. Only append-only ledger
entries conflicted during integration; both histories were retained.

The complete receipt is
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/integrated-research-r6-receipt.json`,
SHA256 `d7fbd67511d2266a85f9a8ff190e64e0ff266d0afbd1841687bcff48005c7575`.
It pins 71 artifacts, final executables/build cache, all native XMLs, Python
logs, prior-main failure evidence and the complete actual-API audit. The initial
missing-policy launch refused before fitting; the accepted audit uses the
archived canonical policy and records exact input/policy/binary hashes.

This is a local integration checkpoint, not full release qualification. Existing
umbrella/BEV issues remain open; this checkpoint does not rerun the full Python
package or every release lane. Quiet-host ThinLTO timing, 100 fresh processes,
subsecond SPY qualification and general pricing/Greek/HFT leadership remain open.
The centrality correction experiment and further reference work continue on
their separate research branches.
