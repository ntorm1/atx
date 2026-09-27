# I1 runtime equity-IC preregistration slice

Status: source and postimplementation fixtures frozen; no C++ compilation, test execution,
market-data access, warehouse publication or registry writes performed by this lane.
This does not complete I1, E2, cluster-N DSR, shared all-stage manifests or the sprint gate.

## Source and ownership

- Production `bcfa5f65`: new `atx-impl/src/prereg.hpp/.cpp`, actual opt-in
  `stage_equity_ic.cpp` consumer. No baseline-view changes needed.
- Follow-up `5c5eb08a`: explicit runtime union bootstrap metadata and direct includes.
- Postimplementation fixtures `0e39693a`: new `tests/prereg_test.cpp` (six checks),
  two additions to existing `tests/stage_equity_ic_test.cpp`.
- Final namespace guard reserves checkpoints >=1000; historical checkpoints remain isolated
  from runtime accounting because legacy attempt IDs divide checkpoint N by their frozen count.
- Root-owned config/parser dependencies `83f670c3`/`4427e3f4` were cherry-picked locally as
  `1cb0c2ff`/`916c388d`. The contextual D6 panel-storage hunk was omitted during conflict
  resolution; these commits are dependency-only and must not be re-imported to root.
- Root must register production `atx-impl/src/prereg.cpp` and the new owning test TU;
  existing stage test target already owns the two consumer checks.

## Runtime interface and exact scope

```
equity-ic ... --ic-prereg-file family-prereg.json --ic-prereg-sha256 <exact-file-sha256>
```

Both flags are required together, only for equity-ic. The direct stage uses the same
`validate_ic_prereg_flags` guard as CLI parsing. Empty fields retain the explicit legacy
checkpoint22 recipe, defaults, serialized recipe/ledger fields and statistical loop order.
No new runtime fields are added to legacy output. Existing legacy fixture remains the
owning byte-determinism check; it has not been rerun for this slice.

Example declaration (synthetic expression, not an alpha or tradeability claim):

```json
{
  "schema": "atx-equity-ic-prereg-v1",
  "epoch": "synthetic-runtime-v1",
  "checkpoint": 1001,
  "declared_n": 8,
  "forward_variants": ["DropMissingForward", "IncludeAuditedTerminalV1"],
  "restrictions": ["full", "_ex34"],
  "families": [{
    "id": "reversal_5",
    "name": "runtime_reversal_5",
    "dsl": "close / delay(close, 5) - 1",
    "sign": -1,
    "theme": "short-term-reversal",
    "horizons": [5, 21],
    "lineage": {"kind": "new"}
  }]
}
```

The two forward variants and restrictions are explicit and fixed in this schema; changing
those choices needs a future version. Each family has its own horizon list, not an implicit
cross-product with every other family's horizons. Three existing reference signals are
remeasured on the union and are not new family trials. The common sample uses the union's
longest label plus the configured execution delay. Bootstrap horizon indices are local to
each signal list and that scope is recorded. Fixed block lengths derive from each actual
horizon in the engine; union lengths are ledger/recipe metadata only.

DSL text is parsed and printed by the existing alpha AST printer before canonical SHA256.
The exact original file-byte SHA256 is independently verified and retained. Keys are sorted
by the canonical JSON implementation; family array order remains meaningful (bootstrap
signal indices). Identity binds family ID/name, canonical DSL, sign, theme, horizon list,
forward variants and restrictions. Duplicate overlapping canonical numerical configurations
are rejected even under different names. This is structural identity, not a proof of all
algebraic equivalences. Signs multiply evaluated finite signals exactly once.

Caps are 1 MiB input, 64 families, 8 union horizons, horizons 1..4096, 4096 DSL bytes and
256 lexer tokens per DSL (bounds recursive parser work), 12 JSON nesting levels. IDs/names
are restricted ASCII alphanumeric/underscore/hyphen. Unknown/duplicate keys, invalid or
nonfinite folded DSL, ambiguous counts, changed retained configurations and unsupported
variant/restriction lists fail before candidate VM work or registration. Runtime family/ancillary VM
allocation budgets deduct already-accounted resident estimates before evaluation; this is
not a measured process-RSS bound or a new global CSV-output streaming guarantee.

## Registration, lineage and failure behavior

`declared_n` must equal four times the horizon count of families declared new. A retained
lineage replaces `{"kind":"new"}` with kind=retained, prior prereg SHA256, exact unchanged
configuration SHA256 and prior trial ID. This is explicitly declared/unverified until E2
reconciles actual prior artifacts/trials; it is not accepted evidence that a prior trial ran.

The existing ledger conservatively charges **every measured family configuration**, including
unverified retained declarations. Thus an all-retained declaration can have declared_new_n=0
while ledger N remains positive. No non-trial purpose/schema exemption is fabricated.
Both counts, full canonical declaration and retained status are bound into the recipe,
request and manifest; ledger parents bind exact file and canonical hashes, with the full
canonical declaration in notes. Adding DSL families within the existing supported context
field interface needs no executable rebuild; arbitrary new source-field ingestion is out
of scope.

The runtime pre-registration line is appended after baseline/reference binding but BEFORE
family VM evaluation. The family preparation runs inside the existing guarded terminal
phase, so lookback/field/VM failures retain both pre-registration and failed terminal
receipts. Runtime attempt IDs bind canonical prereg hash, verified checkpoint line ordinal
and an exclusive output-directory hash suffix; there is no division by declared N. Legacy
family evaluation/registration order is preserved. Pure parser validation may run before
registration because it produces no candidate statistics.

## Pending qualification

Suggested focused filter: `EquityIcPrereg.*:StageEquityIc.Runtime*`, plus the existing
`StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput` legacy check.
Fixtures include canonical versus byte hashes, strict keys/shape/counts, duplicate canonical
configuration overlap, retained exact-configuration binding, changed-file rejection, shared
CLI/direct guards, synthetic runtime additions, opposite signs, heterogeneous21/63 horizons,
all-retained zero declared-new N, and a post-registration lookback failure with digest-bound
failed receipt. No build, runtime, hygiene, performance, empirical alpha or tradeability pass
is claimed. Runtime file/ledger tests must use only owned temporary synthetic artifacts.

Remaining original I1 work includes E2 historical reconciliation (including missing cp22
ledger), proven cumulative/cluster counts, all-stage run manifests, config-file reachability
and shared universal guards. Historical sidecar imports and production data remain untouched.


## First runtime receipt and test-only repair

Root qualification at source `2e3981011478085829596143ebbd905d1283697a` retained
`build-equity/w1-next-impl-prereg-qualified.log` and its receipt. The legacy
TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput check passed in 13.537 s;
the following runtime preregistration case aborted with native -1073740791 before
XML publication. Receipt wall time is 14.2643559 s and binary SHA256 is
`44a02e7a4a2230f33c9e7ea910d2e7924f95a22c55fab09dfe360499e3ea169f`.
This failed attempt is retained and is not counted as a passed runtime gate.

Source inspection found the exact invalid access in the test's CSV classifier:
a positive row moves fields into positives, then a separate if immediately reads
fields[2] from the moved-from vector. Test-only correction
`b464a5b85e6a3bd196dafa562918dc4c9b57d380` makes the negative branch else-if.
No production change was made. Per-family result construction/emission uses each
family's declared horizon span consistently; the engine sizes results from that
same span. Root owns the targeted rebuild/rerun. No postrepair pass is claimed here.
