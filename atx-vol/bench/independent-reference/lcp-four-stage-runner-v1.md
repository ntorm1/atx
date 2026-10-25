# Four-stage observation runner component

The complete runner is implemented for the prospective plan frozen at
`02ad39e0`. The four saved stages **have not been observed**. This is a small
fixture and reader validation result, pending fresh exact-source peer review
before the four-stage experiment. No PDE request, historical decision,
production fitting code or numerical source changed.

The parent approved a budget clarification before execution: the selection has
at most131,064 distinct rows, evaluated once by export and once by the independent
reader. The automatic run therefore performs at most262,128 exact/Decimal100 row
evaluations across exactly two passes. The manifest reports both counts. The
original128-update/256-linear-call per-stage caps remain unchanged.

## Implementation boundary

- `lcp_observe_run.py` has fixed `run` and `verify` commands. Its public experiment
  has no case, mode, tolerance or work-budget override. It joins the original
  manifest/outcomes and exactly four pinned failure files before observation.
- `lcp_observe_archive.py` preflights ZIP member lists and NPY headers before
  allocation, stores uncompressed trajectories, enforces bounded writes, and
  reconstructs all state evidence through the approved observer reader.
- The independent exact audit uses Fraction operands from the actual binary64
  values and a separately fixed Decimal100 round-to-nearest context. It checks
  original complementarity and preceding-policy selected equations over the
  last state and earliest confirmed two-lap closing state, deduplicated.
- The reader binds archived source to an explicitly supplied reviewed Git SHA;
  it also pins the unchanged observer and original numerical files. It checks
  actual inventory bytes, typed summaries, work phases, exact audit results and
  full-state recurrence. Hash updates cannot bypass those numerical joins.
- Only a fully validated archive replaces the initial incomplete manifest.
  Failed observation or final validation preserves incomplete evidence, with
  no retry, truncation or resume. Stage and whole-output caps remain24/128MiB.
  Checked-state reservation, retained state bytes and original input array bytes
  are explicit; unchanged solver/Python temporary memory is not bounded here.

The reader does not replay linear solves. Consequently it validates recorded
state consistency, work, source binding and repeated-state equality; unsigned
receipts are not an independent proof that every transition was produced by an
untampered process. No cycle is claimed until the reviewed real observation
produces the required full-bit two-lap evidence.

## Focused evidence

Evidence directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-observe-runner-v1/`.
`receipt.json` binds the final source and retained logs. The earlier first RED
log is preserved separately as `repair-iv/lcp-runner-red.log`.

The first11-test runtime RED had10 missing-implementation errors and7 errors
from a synthetic matrix with a zero dominance margin. The fixture diagonal
was corrected from2 to3 before the green run; no numerical solver check was
relaxed. The final runner suite has17 tests. Together with16 approved observer
and15 primitive/obstacle controls, **48 pass**. The runner and observer suites
also pass all33 controls under `python -O`.

Commands from `atx-vol/bench/independent-reference`:

```text
python -m unittest -v test_lcp_observe_run test_lcp_observe test_lcp_correction test_pde_reference.ObstacleTests
python -O -m unittest -v test_lcp_observe_run test_lcp_observe
```

Controls include rehashed state/policy/residual/exact-summary corruption,
wrong source with a refreshed inventory, missing/extra/duplicate inputs,
zero-solve reason laundering, an erased finite initial state, terminal-presence
contradictions, malformed/oversized NPY headers, noncanonical Boolean policy
bytes, compressed output trajectories, capped writes, and failed final readers
that must leave an incomplete marker. Exact row controls include a cancellation
where binary64 gives zero but rational/Decimal100 gives one, mixed preceding
policies, and independence from the caller's ambient Decimal context.

A read-only loader preflight joins all four original files, each16,383 rows,
to their fixed bytes and historical failed-stage records. It invokes no solver.
The real observer, correction primitive and original PDE file remain identical
to the approved predecessor. Python syntax and `git diff --check` pass.

## Future execution gate

After exact-source peer approval, run once from a clean reviewed checkout into
a fresh evidence directory, then retain its complete or incomplete outcome:

```text
python lcp_observe_run.py run --inputs <original-zero-rate-control-directory> --output <fresh-observation-directory>
python lcp_observe_run.py verify --directory <observation-directory> --source-commit <reviewed-full-commit>
```

The run itself includes one independent reader pass before publication. The
second command is an optional separate read-only verification, with its own
per-pass row count; it is not needed to establish the automatic run's two-pass
budget. No twelve-request or full144 rerun is authorized by this component.
