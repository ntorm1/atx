# Twelve-request LCP harness: source-review checkpoint

The reviewed primitive `17ab5fbf`, following prospective plan `9641ef8d`, is now
connected to a separate offline control runner. The fixed twelve requests have
not run. This checkpoint prepares their execution and receipt validation; it
does not change a production solver, historical pilot or Greek decision.

## Exact scope and source ownership

`lcp_zero_rate_run.py` requires explicit `--mode one_correction`. Its only model
is the original synthetic `zero_rate_closed_form-put-03`: spot/strike100,
maturity1, volatility0.2, rate/yield0, put. Both `dirk_l` and `dirk_third` use
all six unchanged v2 grids, in the original order:

| Label | nx | nt | Domain multiplier |
|---|---:|---:|---:|
| space-coarse | 4097 | 512 | 1 |
| center | 8193 | 512 | 1 |
| space-fine | 16385 | 512 | 1 |
| time-coarse | 8193 | 256 | 1 |
| time-fine | 8193 | 1024 | 1 |
| domain-wide | 16385 | 512 | 2 |

No input/model/grid or numerical-budget option is exposed. The original stage
LCP tolerance, payoff, boundary, graded-time mesh and `2^27` stage-node guard
remain in the unchanged `greek_reference.py`. The unscaled obstacle equations,
policy selection and bounds remain in the reviewed primitive.

`lcp_stage_adapter.py` loads that unchanged Greek source into a fresh private
module namespace and binds only that instance's obstacle call to a caller-owned
collector. It does not register a replacement module, mutate the process-global
imports, use TLS or alter either original numerical source file. The explicit
`original` adapter mode exists for small unit parity checks; it is unavailable
as a twelve-request CLI mode. No cached PDE result supplies an output.

## Evidence and failure accounting

Before executing, the runner requires clean Git source, the exact approved
primitive/default/declaration contents and the three immutable historical v2
receipt hashes. The existing verifier revalidates all historical144 rows and
their source archives without rerunning PDE. The six old successes and six old
refusals for this model remain as baseline data; no analytic value replaces a
failed solve.

A newly created output directory contains source snapshots, historical receipt
copies, streamed and flushed outcome rows, stage work and a separate summary.
Every selected-policy and correction call is counted, including failed attempts;
each completed stage retains its actual complementarity residual. Independent
checks join all stage counts to the PDE aggregates and enforce the original
`n+2` update and two-call bounds. A typed primitive failure retains owned input
arrays and the last finite checked vector in a separate hashed NPZ. Its reader
recomputes that vector's original residual. If no typed work record exists,
the outcome explicitly reports unknown work; it does not invent zero attempts.

Declared model values, actual numeric types, all grids, stages, geometry,
three-node payoff/regime and derivative stencils are checked using the existing
independent reader plus new mode and work checks. Every refusal stays in the
twelve-request denominator. All714 other corpus contracts and six other cold
Greek fields remain uncomputed.

The runner preserves Black-Scholes price/Delta/Gamma diagnostics and the original
v2 refinement/analytic-interval rule. The reported status is always
`not_promoted`, with the prospective rule result in a separate field. A control
pass requires twelve computed requests and both original refinement rules to
pass; it remains an unqualified control result. The separate schema cannot be
read as a v1/v2 pilot by the historical promotion reader. A new complete144 plan
and execution would be needed for new reference decisions.

The final reader requires clean-source completion, before/after hashes, exact
archived source/Git matches, the immutable primitive/default revisions, unchanged
historical bytes and reconstruction of the summary. Merely making a changed
primitive's new Git/hash metadata self-consistent cannot relabel it as17ab.
Exclusive directory/file creation protects prior results; completed outputs are
never updated in place by a later run. Partial or source-mutated runs cannot pass
verification. Elapsed timestamps record provenance, not a performance claim.

## Source-checkpoint controls

Thirty focused tests pass: fifteen adapter/harness/provenance controls and the
fifteen previously approved primitive/original obstacle controls. Reader stubs
first failed at runtime. Later self-consistent wrong-primitive and boolean/grid
type mutants reproduced runtime failures before their guards were fixed.
Synthetic protocol tests deliberately preserve the six historical refusals;
they are not numerical successor outcomes. Small `nx=33, nt=4` adapter controls
check actual private routing, stage counters and original-mode bit parity;
none executes a declared grid above.

The tests also cover dirty-source refusal, existing-output preservation, twelve
flushed outcomes despite a refusal, immutable baseline copies, actual failed
vector/residual consistency, malformed counters, changed grids and source/hash
contradictions. A syntax error discovered during the initial runner compile was
fixed before runtime tests; it is not counted as a runtime-red control.

Evidence: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/`,
`lcp-zero-rate-harness-final-receipt.json` and its pinned test logs/source files.
No C++ change or native build belongs to this component. Python syntax checks
and `git diff --check` are its applicable hygiene checks.

After fresh exact-source approval, execution uses a new directory:

```powershell
python lcp_zero_rate_run.py --mode one_correction --baseline C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/independent-greek-pilot-v2 --output C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/zero-rate-lcp-control-new
python lcp_zero_rate_run.py --verify C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/zero-rate-lcp-control-new
```

These commands are documented for the later approved run. They have not executed
at this checkpoint. No option semigroup, full144, fitting qualification or new
Greek accuracy claim follows from this harness approval.
