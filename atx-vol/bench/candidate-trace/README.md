# Complete monetary-candidate capture

This is an offline diagnostic for the six original Screen/Fast proposal and
scalar Reference proof phases. It changes no proposal, price proof, fallback or
fit policy. The default-off `ATX_VOL_CAPTURE_PRICE_CANDIDATES` library build is
required, as are one fit worker, one repetition, timing detail and cold reset.
Other audits/replays are mutually exclusive. All three output files must be new.

```powershell
$env:ATX_VOL_FIT_WORKERS = '1'
atx-vol-quote-bench.exe --input quotes-SPY.csv --output predictions.csv `
  --report native.json --policy canonical-policy.json --timing-detail `
  --cold-reset --candidate-capture capture.json
python capture_audit.py --capture capture.json --report native.json `
  --predictions predictions.csv --out summary.json --spy
```

The executable reserves 4096 call records and 4096 cache lifetimes before fitting.
Setup has its own diagnostic clock; the observed fit includes collector and phase
clock overhead. JSON serialization occurs after fitting. Neither those clocks nor
the process clock from this instrumented execution qualifies latency. Native
reports mark this explicitly, and the SPY promotion reader rejects capture-marked
or explicitly nonqualifying clocks even if their summary claims sub-second work.

The capture owns ordered requests, raw options, decisions and six phase records.
Every double carries both JSON numeric value (null for nonfinite) and exact
binary64 bits. Cache constructor/destructor identities, before/after model keys,
counts and closing snapshots preserve warm floors and address reuse. No vendor
reference, target adjustment or discarded refusal is introduced.

The independent reader reconstructs the short-circuit graph and Screen/Fast
option schemes, joins proposal/proof seed bits, validates cache lifetimes and
monotonic work, and checks actual phase/whole-call solve and floor-count sums.
Every call is joined to the existing per-slice candidate/Screen/Fast counters.
It rejects unknown/missing IDs, incomplete phases, fabricated work and changed
denominators. `--spy` additionally pins original input and canonical LF policy
hashes plus all700 attempts,684 acceptances and16 refusals. Those are fixed
expected counts, not a numerical model theorem or a license to drop other calls.

Unit controls retain five distinct refusal/retry/invalid/allocation paths and
21 malformed mutations, plus an inter-call cache-key mutation. Prepared keys must
remain unchanged when preparations do not advance, including before/after each
call, between calls and at owner close. The optional cache may decline and leave
its state untouched while the original cold scalar floor fallback accepts; that
valid path is counted separately. An actual fail-once native control demonstrates
the accepted, bit-identical candidate with an empty cache and unchanged solve work.
Native CLI controls refuse incompatible scopes before
reading input or creating files. The separate 13-gate scalar actual-API audit
and prediction-byte parity remain required before using captured SPY work for
an architecture decision. No whole-SPY capture has yet been established by this
source component; no timing or numerical improvement is claimed.
