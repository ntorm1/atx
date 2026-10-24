# Four saved-stage observation: loader, archive and reader plan

Prospective runner on `b422fe45da3beae6eba31872ff9da06f595b7b64`. The earlier
`lcp-four-stage-observation-plan-v1.md` remains the numerical and work contract.
The approved observer is unchanged (canonical LF SHA256
`897c67981f508655074113bb1903dd738ebb165cb3256b17ff65d75a71bd7e30`).
No saved-stage observation has run. This plan is frozen before runner TDD;
independent review of the final runner is required before the four-stage replay.

## Entry points and fixed inputs

Add `lcp_observe_run.py` with explicit `run --inputs DIR --output NEWDIR` and
`verify --directory DIR --source-commit FULL_SHA` commands. There is no case,
PDE grid, tolerance, update-count, solver-mode or historical-decision switch.
The run uses only the four files, stages and byte hashes in the predecessor
plan, original binary64 tolerance `1e-10`, and `max_updates=128`. Small tests use
private loader/archive helpers with an explicit synthetic contract, never an
alternate public experiment or a real saved-stage fixture.

The loader pins the original manifest and outcomes bytes before parsing them.
It independently joins each of exactly four failure records to scheme, grid,
stage, original `one_correction` diagnostics, file hash and 16,383-row shape.
It rejects extra or missing `*-failure.npz` inputs. The historical directory may
retain its unrelated original receipts. NPZ loading uses `allow_pickle=False`;
ZIP names, duplicates, member sizes, NPY headers/dtypes/shapes and total declared
uncompressed bytes are checked before allocating arrays. Each input has exactly
the six original operands plus `last_checked_values`, all finite binary64.
The old final vector is archived unchanged and is never used as the initial state.

## Provenance and transaction boundary

The run requires a clean Git worktree and records its full commit, Python,
NumPy, SciPy and platform versions. All relevant runner, observer, primitive,
reader, tests and plans are copied as source evidence and compared against that
commit, allowing only CRLF/LF normalization for Git text. The original observer
and two numerical source hashes are additionally fixed constants. No archived
source is executed by the reader. A verifier requires the expected source commit
as an explicit argument and binds the copies to that Git content, rather than
trusting a commit chosen inside the archive. Numerical thread environment is set
to one before importing NumPy/SciPy; this is not a latency qualification.

Create the output directory exclusively. Write an initial incomplete manifest,
then process each declared stage once in the declared order. Preserve all partial
files on exception. Copy and hash the original manifest, outcomes and four input
files; compare original source/input bytes again after observation. Publish a
complete manifest only after all four observations and independent validation
succeed. Never overwrite an existing output directory or silently retry a stage.

## Per-stage export and bounded exact audit

Use uncompressed NPZ for checked values and Boolean policies. Original inputs
remain separately copied byte-for-byte. JSON stores the observer's scalar work,
phase, state, recurrence and terminal-presence metadata; the terminal vector is
the checked final row and its equality is validated before serialization. On read,
reconstruct it only when the explicit terminal-presence flag agrees with the
number of retained states. JSON rejects duplicate keys, nonfinite literals and
unexpected keys. Complete archive inventories reject extra files and symlinks.

Invoke the approved independent `verify_observation` over every reconstructed
state. Its full binary64 equality and phase checks stay authoritative. Add an
independent exact row audit for the last state and the earliest confirmed repeat's
closing state, deduplicated and sorted. For each selected state reconstruct every
continuation, gap and preceding-policy selected equation twice: exact `Fraction`
from the actual binary64 operands, and `Decimal` at precision100. Use neither
exported residuals nor a linear solve. Initial states have no preceding policy.

Record row counts, exact rational maxima and violation row IDs, Decimal100 maxima,
and deterministic digests of every reconstructed row. The reader recomputes all
of them. Digests compact the export; they never replace full-state equality or
the reader's independent arithmetic. Decimal100 is an independent finite-precision
diagnostic, not a rigorous error bound. Exact rows classify tolerance against the
exact binary64 `1e-10`, not a rounded decimal substitute. The original terminal
comparison uses full value bits and is diagnostic, not a convergence assertion.

At most two states per stage and 131,064 distinct rows receive each exact audit
pass. Export computes these once and the independent reader recomputes them once
before publication: at most262,128 row evaluations across the two automatic
passes. A separately invoked reader has the same per-pass limit. Each
row has only three products and bounded binary64 operands; process rows
sequentially. Summaries include at most three lists of row IDs per state and
fixed extrema/digests; cap each stage's JSON at1MiB. Do not export huge per-row
rational strings. Report checked-state bytes, original-input bytes and fixed
caps separately; no claim about whole Python/SciPy process memory.

## Admission and resource limits

Preflight declared ZIP/NPY sizes and shapes before allocation. Checked-state
storage remains at most19,020,663 bytes per stage. Original input copy plus
trajectory and scalar JSON must fit24MiB per stage. Complete output including
historical receipts, sources and manifest must fit128MiB. A bounded writer
refuses an over-cap file before extending it beyond the cap; partial output
remains incomplete. The reader enforces actual file sizes, inventories, digests
and per-stage/total caps, not only declared counters. Reject unexpected dtypes,
Fortran-order arrays, extra/duplicate members and oversized NPY declarations.

The final summary retains all four IDs/statuses, attempted/completed work,
observation-cap versus original numerical refusal, recurrence category and exact
row findings. It makes no PDE/Greek reclassification and no speed claim. Failure
to see a repeat in128 updates stays inconclusive. Original historical files and
results are immutable.

## Focused runtime controls before review

Use tiny synthetic systems only. Establish runtime RED then green for strict
input/provenance joins; archive round trip; duplicate/missing/extra identities;
malformed ZIP/header/dtype/shape and bounded writes; overwrite and incomplete
archives; source/hash changes; independent policy/residual/terminal reconstruction;
all previously found reason/phase laundering mutants; exact rational/Decimal row
checks; and recurrence-selected audit states. Rehashing a mutated trajectory or
scalar file must not bypass semantic validation. Verify under normal Python and
`python -O`. Run the existing16 observer tests and focused primitive controls.

Freeze the source, tests, receipts and exact SHA for fresh independent review.
Only a separately authorized/reviewed execution may then run the four immutable
saved stages. No incidental test or import loads them into the solver.
