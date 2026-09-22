# SEC submissions verified resume

Status: implemented; all 54 focused cases passed, independent review completed,
and its two Important repairs accepted after focused regressions passed.
Root owns production/database work and the final commit decision. The source and
activation locks were explicitly granted before implementation.

## Evidence and constraint

`source-prepass2-stop-inspection.json` records archive SHA256
`702fbcd8b4335bc649e9e4eab3a202f3effc314b43421664bfecb59365767165` and
dataset attempt `04cf947d-53bb-49b7-a276-b3c74a2a52c8` retaining 8,504,213 rows
across 37,700 CIKs through `0000933249`. The archive directory inventory has
985,667 main CIK members. The stop was a host-headroom guard stop, not a proven
worker OOM. These counts and the maximum CIK do not prove prefix completeness.

Keep all main members in scope, forms unrestricted, history enabled, batches of
50 nonempty CIKs, and the existing bounded reconnect/progress behavior. No new
dependencies, schema migration, larger memory setting, or parallel workload.

## Implemented first continuation

Added an explicit prior dataset-run UUID option, with no automatic resume. Before
writing any resumed filing rows:

1. Require the prior attempt to be terminal/failed, its source to be the bulk
   submissions loader, and its saved dataset parameters to match unrestricted
   CIKs/forms, history enabled, batch size, and archive path. Follow a prior
   resume UUID in those parameters to support a later interrupted continuation;
   reject cycles or incompatible/missing lineage.
2. Hash the archive using the existing streaming SHA256 helper. Require a
   matching recorded source receipt predating the first attempt. A maximum CIK
   or operator-provided hash alone does not authorize skipping.
3. Query retained rows for the verified lineage, using bounded fetches ordered
   by CIK/accession under the caller's existing memory/thread limits. Require a
   positive number of nonempty CIKs divisible by the batch size. Their maximum
   CIK is only a proposed boundary.
4. Read every sorted main archive member through that proposed boundary,
   including empty members and referenced history. Build the same first-wins
   accession set per CIK as bulk loading, and compare exact accession, resolved
   security ID, form, and source member/path to the retained rows and their run
   lineage. Fail closed on missing/extra rows, CIK holes, changed mappings,
   partial history, unexpected sources, or an incomplete batch boundary. Do
   not deserialize/normalize the entire prefix at once.
5. Only after successful verification skip that prefix. Resume the existing
   loader at the next main member. Keep ancestry visible in result/source
   metadata. `rows_loaded` remains rows written by this new dataset attempt;
   separate counters report verified prior rows and total covered rows. Main
   progress reports the full archive denominator and the verified prefix plus
   new work. Completion is claimed only after the remaining full scope finishes.

This deliberately avoids inventing a retroactive transactional checkpoint for
the old run. A future small transactional checkpoint can replace the prefix
revalidation cost after this first continuation, but is not required to prove
this old attempt. Sidecar JSON alone is not authoritative because file writes
cannot commit atomically with the warehouse batch.

## Files and focused verification

- `atx-db/src/atx_db/sec_submissions.py`: option and integration with the
  existing streaming loader and bounded verification helpers.
- `atx-db/tests/test_sec_submissions_resume.py`: new tests, avoiding the locked
  lifecycle test file. Cover verified prefix + empty member + history, full
  scope completion, incomplete/missing prefix rows, partial final batch,
  changed archive or params, missing/late receipt, and repeated interruption
  lineage. Verification failures must leave submission rows unchanged.
- `atx-db/src/atx_db/activation.py`: explicit
  `--submissions-resume-from-run-id DATASET_UUID` CLI/config wiring and refusal
  to complete the activation stage when referenced history is missing. Direct
  and CLI preflight reject resume plans that include the archive download stage
  before side effects can overwrite the required pre-attempt receipt.

Root runs imports/tests/probes and arranges one independent review. No
implementer execution of those runtime checks. Commit only exact approved paths after
root verification, with the required Claude Fable 5.1 coauthor trailer.

Root verification: new resume tests (25 cases) plus existing bulk tests (10
cases) passed, exit 0, measured process peak 0.674938 GiB; separately all 11
production-activation tests passed, exit 0, peak 0.678215 GiB. Root then allowed
implementer static Ruff on touched files: passed after binding the source-key
helper's target explicitly and marking a test regex raw. No implementer imports,
database calls, tests, probes, or production jobs were run.

All eight review-repair regressions then passed, exit 0, peak 0.651371 GiB.
They cover truthful full-scope metadata and receipt-preserving activation
preflight. See the accompanying implementation report and review for details.

Production cost is still unmeasured. Verification adds a complete archive hash,
one ordered retained-row query (spill allowed under the existing 1 GB budget),
and parsing of every candidate-prefix JSON member/history. It avoids pandas
date normalization and any prefix rewrite. Python holds one CIK's deduplicated
keys plus a fetch batch of at most 4,096 retained rows, in addition to the same
ZIP directory structures the existing loader already needs. No claim that
verification is faster than replay is made without measurement.
