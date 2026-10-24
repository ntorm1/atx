# Joint row census, Stage A only

Frozen implementation contract on c2008138/b422; no reduced solver is included.
The option is `ATX_VOL_JOINT_ROW_CENSUS_RESEARCH`, default OFF. Its macros apply
only to the owning projection/caller/control translation units. Ordinary solver
dispatch compiles away and uses the existing arithmetic, allocation and clocks.

The private observer has two synchronous callbacks. `built` receives the full
unchanged Problem through borrowed spans and a bounded row accessor, the actual
initial `x0` immediately after `solve` clamps the selected target, slice metadata,
and a caller raw-midpoint accessor. `published` receives the certified node
vectors and the final unclamped free vector, after all existing certificates and
before caller-owned writes. Accessors and every borrowed view expire on return.
No observer output is a solver input. Stored row indices are copied exactly,
including slots whose merged coefficient is zero.

The private function gains optional observer/raw-source pointers; ordinary
callers pass null. In an enabled tool build, a one-use research scope can bind
the observer to the calling thread. The raw-draft caller supplies a stack-lived
accessor over its drafts and sorted order. This requires no raw-source array or
allocation and preserves selected feasible-box targets separately from raw mids.

The recorder borrows disjoint, caller-owned, pre-sized storage: 64 slices,
7,000 nodes, 14,000 original coordinates, 14,000 free variables and 60,000 rows
for the explicit SPY tool. Counts are checked before any copy. The command-line
owner allocates/resizes once before the fit; hooks never allocate. Row storage
contains all four index/coefficient slots and the exact count/RHS. Coordinate
records retain fixed substitution; variable records retain bounds, selected
target, actual x0, unclamped final iterate and actual published price. Node
records retain strike, selected/raw call+put, terminal prepaid and publication.
Receipt capacities are declared, not inferred from successful lengths.

An observer has exactly one attempted projection, one built event and one
publication event. Missing/duplicate phases, capacity exhaustion, bad metadata,
callback exception, reentrant callback, nested/reused scope, or a projection on
a foreign thread invalidate capture only. Exception containment is in the
observer wrapper. Reentrant hooks suppress callbacks rather than recurse.
Foreign events touch process-lifetime atomics only, never another thread's
owner. All worker activity must finish before scope exit; ownership is never
transferred to a worker. Failed numerical projections retain incomplete capture
and the unchanged numerical result. The CLI refuses capture-marked timing as
qualification, retains the native fit output, and returns a failed receipt.

Tests first establish runtime RED against a no-dispatch solver. Following the
user's subsequent acceptance instruction, null/profile/capture output bit
differences are diagnostic, not a cross-solver acceptance gate; controls require
the meaningful price/constraint contract. Exact copied row/input bits and
nonclock counter parity still check observation integrity (clock equality is
not expected). Controls also cover allocation counts, row/target/substitution data,
both callback lifetimes, callback exceptions/reentrancy, capacity and owner
errors. Independent reader controls corrupt rows, coordinates, targets,
publication, phase/capacity counts and graph edges. The graph uses exactly the
stored lower-triangle pairs emitted by NewtonSystem, including zero-valued
stored coefficients; no algebraic edge pruning is substituted.

Fresh source review and owning PCH-off checks precede the single full-SPY census
and its separate thirteen-gate scalar API audit. No SPY run is authorized by a
unit-test pass, and no Stage B implementation is part of this contract.

The observed owning-TU row arithmetic is ordered binary64 FMA under the declared
build, not separate Python multiply/add. Native initial slacks call the original
Row::value. The independent reader must round each exact product-plus-addend,
including signed-zero rules, and reject mismatched arithmetic evidence. This
describes this executable's observations, not a requirement that another compiler
or improved solver produce identical prices. Historical exact floor-target pins
are not a new optimization gate under the user's current economic contract.

## Component evidence

The first three native controls failed at runtime against the no-dispatch stub;
all 14 final native controls pass. Five owning translation units compile with
PCH disabled. The independent reader first failed its five stub controls, then
passes 10 controls plus two native CLI checks. The first CLI attempt used an
incorrect worker environment name in its test and refused before fitting; that
failed attempt is retained, then corrected to `ATX_VOL_FIT_WORKERS`.

The five small native fixtures cover ordinary crossing, fixed coordinates,
already-feasible early return, raw-draft terminal inputs, and three distinct
strike grids with non-dividend constraints. Fixture JSON is compacted only for
repository storage; parsed exact binary64 strings equal the native exports.
The independent builder reconstructs every retained shape, parity,
non-dividend, calendar and bound row before checking native initial slacks.
Published clamp/coordinate joins and all final row slacks are reported
separately from observation integrity. No public API audit is replaced by this
row scan. The raw-draft production caller has no private projection profile;
its census reports `profile: null` rather than enabling new solver clocks.

The observed enabled object's row accessor contains ordered scalar FMA at
offsets 0x92, 0x9d, 0xaf and 0xc1, with the scalar remainder at 0xfa. The ordinary
solver's row loop also remains ordered FMA. The arithmetic evidence manifest
binds the executable, owning object, source, cache and disassembly. Its hash
reader verifies those reviewed artifacts; it is not a general disassembler or
proof of arbitrary machine code. Artifact-free fixture summaries explicitly
report `arithmetic_evidence_verified: false`. A tool capture requires the
reviewed arithmetic manifest:

```text
python census_audit.py capture.json --arithmetic-evidence arithmetic-evidence.json --output fresh-summary.json
python -m unittest -v test_census_audit
ATX_QUOTE_BENCH_BINARY=<enabled-binary> python -m unittest -v test_census_cli
```

Evidence is retained in `acceptance/joint-row-census-v1` under the r7 quote
benchmark evidence directory. Neither a full-SPY census nor a reduced solve has
run at component freeze. Full census and actual-API validation follow fresh
source review; Stage B still requires its own implementation review.
