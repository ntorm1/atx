# Task15 original native text/TNIC implementation

Status: conditional implementation in progress toward owned commit and
independent review; actual source qualification and original release gates
remain **FAIL/unmeasured**. No production-ready or Task15 completion claim.

Requirements read: task-15-brief.md first, native-continuation brief,
global-constraints.md, foreign-dirty-files.txt, R21 cutoff ruling,
gap-analysis section2 item13/section3h, original task-TXT-report.md,
ALPHA_PANEL_TEXT.md, legacy filing_text/tnic/text_features source, lane rules,
R24 TNIC method ruling and controller SEC ledger chronology auditv2.
Graph tools were unavailable (`ALL_TOOLS` search returned[]), so targeted `rg`
and source/document reads were used. No subagent, worktree or cleanup.

## Owned implementation

New `filing_text_native.py`: independently frozen source plan, runtime/code
identity, full native monthly member/identity populations, original SEC recent/
historical submissions arrays with matching source receipts, accepted UTC
clocks from authenticated original EDGAR index Accepted proofs (NY wall time,
DST ambiguity rejected, raw submission Z is not presumed true UTC), preserved
annual/amendment inventory and exclusions, original primary
bodies/receipts, original index-typed EX13 bodies, and full original SEC
quarterly-index cohort closure. No download, HTTP or actual source inspection.

Fitting uses the complete original SEC annual cohort, independently of future
member selection. Exact original master-index accession/form/date/owner replay
is required, including co-registrant metadata. A caller completeness flag or
matching normalized rows does not replace this native census. Original linked
10K-filer/member publication denominators remain unchanged. Missing original
index quarters leave fitting missing with explicit reasons.

New `text_features_native.py`: bounded issuer/file units; exact unhashed token
frequency cosine/Jaccard; word/character/sentence counts; legacy heuristic Fog;
explicit sentence-edit proxy with6000sentence limit; log word change; original
document byte/readability metrics; unique prior fiscal-year, same-form-family
matching with both filing/report gaps180..550days; amendment exclusions; missing
clock/body/section/period reasons. Whole-item3 fallback cannot masquerade as
20FItem3.D. Original EX13 classification is authenticated through actual source
index document type, URL, bytes and receipt. Missing bodies/sections are NULL,
not fabricated zero. No licensed sentiment dictionary is used.

Publication writes bounded JSONL checkpoints and four typed Parquet products:
`text/features.parquet`, `classification_tnic/pairs.parquet`, `firms.parquet`,
`years.parquet`. Exact source/code/schema/runtime binding; independent pin
checks before/after; authenticated resumable checkpoints plus semantic replay;
orphan/incomplete phase rejection; deterministic immutable generations;
exclusive lock; atomic generation rename followed by atomic activation pointer.
Interrupted staging remains; retained process-kill lock requires controller
reconciliation, never automatic stale lock deletion.

New `tnic_native.py` implements R24 required reproduced TNIC-style method:
legacy vocabulary/stop/geography rules, binary cosine, prior-year DF strictly
above25%, MIN_WORDS50,20000histogram bins, prior-year SIC3 calibration and
strict score>threshold. No nearest-five or positive-cosine-only substitution.
Exact words remove legacy hash collision risk. Historical SIC comes only from
source-pinned original observations with authenticated UTC receipt/vintage;
future2026snapshots cannot qualify earlier fits. Source fitting/corpus scope
and full original index snapshot clocks, every short/filter input, both filing
clocks and SIC clocks contribute to provenance and availability. Immutable
snapshot vintages preserve earlier published generations. Missing fit leaves
NULL peer counts with reasons, not zero. Official external TNIC is not a new
mandatory source under R24 and is not claimed.

## Public admission and integration

`read_native_generation(source_root, generation, expected_manifest_sha256,
expected_plan_sha256)` checks independent identities, entire published/source
populations and fully replays original-byte parsing/corpus fitting and every
Parquet/JSONL/metadata product byte. It writes only isolated replay scratch,
never changes original sources/published products. Native plan/checkpoint
bytes, all output/code/runtime/source/receipt identities and upstream input
manifest SHA256s are frozen. Orphan keys/products and changed inputs reject.

Returned mapping: generation, manifest_sha256, source_plan_sha256,
status(PASS/FAIL), products(exact four filenames to paths), complete yearly
coverage, dependencies(relative-source pin map), code(absolute pin map).
`iter_native_rows(admission, product, batch_size<=64)` gives bounded rows and
product/manifest checks before/after. All Parquet clocks are TIMESTAMPTZUTC.
Keys: text(cik,accession), pairs(year,cik,peer_cik), firms(year,cik), years(year).
`publish_native` and explicit `python -m ...text_features_native --phase
publish/read` provide the real controller-run path, not fixture-only helpers.

Exact API/bindings/products were sent to Task16/Task17 authors. Their modules
were not edited. Task16 keeps `text_score` NULL until actual native admission,
review/gates and exact golden field mapping qualify. Task17's fixed native
reader prerequisite must retain source-quality/gate failure rather than trusting
generic native manifest status. New dataset docs/registration stubs are owned;
shared registries/dictionaries were not touched.

## Offline evidence and exact commands

All test commands run from `C:/atx/atx-db`, with PowerShell environment:
`$env:PYTHONPATH='C:/atx/atx-db/src'; $env:OPENBLAS_NUM_THREADS='1'`.
DuckDB fixtures128MB,1thread; offline sources created only under pytest temp.

Command template (replace FILE with the exact listed file):
`.venv/Scripts/python.exe -m pytest FILE -n0 -o addopts='' -p no:cacheprovider -q`.

| Exact FILE | Latest frozen outcome | Evidence |
|---|---|---|
| tests/test_text_features_native.py | PASS3tests20.41s | Full original-byte replay, authenticated original index clock, stable typed NULL schema, conditional text values,2/2full member cells2020, missing body denominator preservation, interrupted authenticated resume |
| tests/test_text_features_native_failures.py | PASS4tests16.08s | Source/receipt changes, forged orphan-key product with refreshed output hash, missing scope/clock, amendment exclusion |
| tests/test_tnic_native.py | PASS3tests13.32s | Strict peer conditional8/8filers2019,8directed pairs, full corpus clock, future-SIC exclusion, fallback never counts toward strict gate |
| tests/test_filing_text_native.py | PASS3tests2.16s | Original quarterly census closure, missing-year clock, native orphan accession rejection, original index clock/DST ambiguity |
| tests/test_text_features_native_exhibits.py | 1 passed,7.21s | Authenticated original index-typed EX13 replacement/full byte replay and unlisted source rejection |

Earlier exact same command on the pre-split text file passed7tests44.42s;
other earlier focused results6tests23.94s/35.82s, TNIC2tests11.07s/12.16s,
combined8tests40.89s/45.29s. These are superseded by final frozen split checks.
Initial failures exposed DuckDB child cursor isolation of TEMPtables,
unsupported correlated non-inner join, and mixed aggregate GROUPBYALL; fixed
using disk-backed tables, prefiltered tokens and explicit grouping.
The initial split failure-file invocation had1collection error (1.24s) because
the sibling fixture import omitted the `tests.`package prefix; fixed before
the recorded passing4test invocation.

Two pre-freeze test results did not satisfy the timing protocol:1failed/
6passed64.45s due to an owned code edit during its live replay (correctly
rejected changed-code identity), then7passed75.19s amid host pressure. They
are not promoted as compliant checks. Fixtures were split to keep each
invocation below60s. No code edits while final frozen checks run. Root test/
index holds are respected; fixture lane is serialized with Task16/Task17.

## Original actual gates: independently open

| Original gate | Status and actual measured number | Exact execution status |
|---|---|---|
| Verify complete landing before one fetch loop | FAIL/unqualified; no native full-source coverage measurement | Original guarded `python -m atx_db.alpha_panel.filing_text --phase verify` NOT RUN by this lane; root owns actual jobs/HTTP reconciliation |
| Finish source landing, original bodies/EX13/census/clock proof | FAIL/unmeasured | No fetch/source scan/hash/build; old section landing is not absent, but body replay and full census are not admitted |
| classification_tnic strict peers>=95%linked10Kfilers per year2019+ | FAIL/unmeasured actual denominator/numerator in every required year | Actual guarded native publish/read NOT RUN; offline8/8is not actual gate evidence |
| text>=90%full member cells2020+ | FAIL/unmeasured actual denominator/numerator in every required year | Actual guarded native publish/read NOT RUN; offline2/2is not actual gate evidence |
| Yearly2020..2025member_equity coverage and Task2 universes | FAIL/unmeasured | Complete yearly reporting code exists; optional native univ_all_common/univ_r3000p flags+exact independent yearly counts remain actual dependencies |
| Native source/license/clock/code/schema/complete replay | FAIL actual qualification; PASS conditional fixture behavior only | Independently reviewed actual source plan/manifest identities do not exist for this lane |
| Free-source/terms restriction, no licensed sentiment | PASS implementation policy; actual source-rights review remains open | No new source landed,0HTTPcalls,0licensed dictionary values |

For2019,2020,2021,2022,2023,2024,2025,2026 strict actual linked-filer peers are
unmeasured. For2020..2026full actual member text coverage is unmeasured.
Unmeasured values remain missing, never0or a downloaded/parsed-row denominator.
Published conditional manifests include full source yearly counts, full member
denominators, strict original95%/90%gate statuses and exclusions.

Root's `sec-budget-chronology-audit-v2.json` establishes observed existing
sec_text ledger38,433receipts declaring38,678requests, dated2026-09-29..30.
The older3303request/7%landing checkpoint is stale. That root ledger audit
verifies ledger bytes only, not retained source content/completeness/body proof,
source quality or shared all-attempt budget. It grants no SEC HTTP permission.
This lane did not inspect/hash/verify actual source/metadata/receipts or launch
any actual job. No return calculation/evaluation/forward label was performed.

## Actual source prerequisites for root execution

1. Independently qualify complete native monthly member/identity populations
   (including delisted lines), original upstream manifests/source evidence and
   exact yearly counts; admit additional native Task2 universe populations.
2. Land/retain original SEC submissions census JSON and original full quarterly
   master indexes with exact original transport receipts and actual observed
   source clocks. Derive the entire original SEC fitting census independently
   of future member selection and reconcile original accession/form/date/owner
   records. Unknown/full-quarter scope remains missing.
3. Retain original primary bytes and original index-typed EX13 bytes with
   matching native receipt/body hashes and original index Accepted-clock proofs.
   Existing parsed section rows/document statistics alone cannot replay bodies.
4. Independently admit historical native SIC observation vintages and their
   original publication/observed clocks. Current2026snapshot is insufficient.
5. Freeze the full source plan canonical digest, actual source/code/runtime
   pins, upstream manifests and independent review evidence before any guarded
   actual native producer run. Root reconciles shared HTTP budget/hold first.
6. Guarded publish then independent full read must replay all rows/bytes and
   report original95%/90%gates on full yearly denominators; independent code/
   spec review and golden exact field mapping remain required.

Root-only future command (not executed):
`.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py
--job-gb 0.6 --wait-minutes 60 -- .venv/Scripts/python.exe -m
atx_db.alpha_panel.text_features_native --phase publish --source-root SOURCE
--output-root OUTPUT --plan REVIEWED_PLAN --expected-plan-sha256 INDEPENDENT_DIGEST`.
Reader phase additionally requires immutable generation and independently
expected manifest SHA256. No actual data or disk landing is included in commit.

## Self-review and limitations

Self-review repaired future-member fitting selection, full fit-input/census/SIC
clock dependencies, latest-filing visibility, unknown source zero counts,
unhashed text vectors, fiscal-period matching,20Fscope, current-vs-prior missing
reasons, manifest/checkpoint metadata bytes, mixed UTC/ET mislabeled submission
clock resolution through original index proof, and tzdata runtime binding. Source corpus
scope and observation latency can reduce coverage; no original gate is weakened.

The reproduced vocabulary/readability/edit method remains approximate and
explicitly named. The public schema does not claim official TNIC scores or exact
word minimum-edit distance. Corpus completeness may require sources beyond the
old member-only text landing. Whole source/index and historical SIC prerequisites
are actual open dependencies. Pair computation is bounded and streams to disk,
but actual large-corpus runtime is not measured. Source documents40MiB and
submissions16MiB bounds and retained issuer500000word working-set bounds reject
exceedances rather than allocating unbounded memory. Scratch files are isolated
from serving paths. Final independent review is still pending.

Final frozen checks total14passed across five serialized per-file invocations;
no fixture or actual data process remains live. Commit is restricted to the13
explicit owned paths listed above; the required legacy coauthor trailer is
preserved. Exact commit identity is reported to root after index coordination.

## Whole-task repair round1/5 — findings1–7

Initial commit9ac8af9551d1675f546ef135ba7bec4c97d48e45 received independent
NEEDS CHANGES in review-T15-text-native-findings.md. This is one whole-task
round addressing all seven Important findings; actual gates are unchanged.

1. Complete DF lineage: raw cohort closure/clock/digest and encoded histogram
   closure/clock/digest are separate. Prior raw DF inputs qualify each encoded
   histogram and remain dependencies of later calibration. Missing2018 prevents
   falsely qualified2020; late2018 cannot vanish from2020availability.
2. Original fitting cohort: raw vocabularies below MIN_WORDS50 do not enter
   DF tokens/denominator, and eligible selection occurs before latest-year
   selection as in original tokenize_pass/_load_year; a later short document
   cannot displace an earlier eligible one. Full source census/provenance and published member/
   linked-filer denominators retain short and excluded documents.
3. Interrupted activation: retained immutable intent pins target manifest/plan
   and previous pointer bytes/value. Rename-before-pointer retry independently
   authenticates manifest AND transition, replays source, rejects intervening
   activation and completes only that exact transition. Previous pointer is
   retained as rollback history; process-kill locks still require reconciliation.
4. Public consumption: admission adds source_root/runtime; each bounded batch
   and finally rechecks immutable manifest/plan/product plus full source/code/
   runtime inventory. Explicit close also rejects changed dependencies. Caller
   must close an iterator on early stop; full original replay is unchanged.
5. Accepted identity: original body must name exact accession and filer/archive
   owner CIKs. Both must correspond to original submissions records; absent/
   conflicting semantic or owner-chain evidence leaves the clock unknown.
6. PIT identity: retain actual available_at, strict/dated/name tiers, effective
   intervals and strict monthly22UTC cutoff. One distinct supported CIK must
   agree with supplied member CIK; NULL members resolve through this identity.
   Ambiguous/conflicting/unresolved cells stay in full populations; unresolved
   member identity blocks TNIC gate qualification, rather than shrinking proof.
7. Working sets: streaming word counts reject oversized current/EX13 text before
   token/profile allocation; current+retained profiles checked before publication.
   Original SIC bytes16MiB, receipt1MB and all accession indexes5MB bounds apply
   before reads. TNIC vocabulary input has explicit word/character bounds.

Focused commands from C:/atx/atx-db, PYTHONPATH=C:/atx/atx-db/src and
OPENBLAS_NUM_THREADS=1: `.venv/Scripts/python.exe -m pytest tests/FILE -n0
-o addopts='' -p no:cacheprovider -q`. All synthetic, file-backed128MB/one thread;
serialized root-allocated lane, producer code frozen during each invocation.

| FILE | Result | Findings |
| --- | --- | --- |
| test_tnic_native_lineage.py | First freeze3passed15.20s; final3passed13.60s |1/2; reviewer missing/late2018 repro and forty short raw filings; final adds later short displacement |
| test_text_native_activation.py |1passed6.83s|3; failed pointer, independent transition, intervening pointer, exact recovery/history |
| test_text_native_dependency_reads.py |1passed4.25s|4; original source, helper code, runtime mutation and explicit early close |
| test_text_native_source_admission.py |First freeze4passed5.40s; final4passed3.12s|5/6/7; wrong page/accession/owner, NULL/ambiguous/future/backfill cells, oversized current/SIC/EX13 |
| test_text_native_identity_chain.py |Final2passed8.80s|5/6; missing original owner record and full text coverage of NULL-CIK members |

First nine checks passed; no unchanged broad suite was rerun. Static follow-up
tightened original submission owner correspondence, original eligible-before-
latest selection, and bounded source reads after that first freeze. The final
nine covering tests above passed on the final producer code. Unchanged activation
and public-consumption kernels retain their focused first-freeze evidence; the
final identity publication also exercises full replay/activation. There are11
distinct passing focused tests over seven serialized per-file invocations, all
under60seconds; repeated cases are not new tests. Independent review remains
pending. Existing
fixture Accepted bodies/tier clocks were upgraded to semantic native-shaped
evidence; synthetic evidence remains incapable of qualifying actual sources.
No actual HTTP/source/warehouse/data job occurred in repair. Source/PIT/population,
runtime/code/immutable identities, actual95%/90%yearly gates, field mapping and
independent review remain the original open dependencies.

Final self-review: filter lineage keeps raw DF source qualification separate from
SIC histogram qualification, carries all necessary source clocks, and preserves
full original short/excluded records. Original owner correspondence is enforced
after original issuer census derivation; no caller archive owner alone qualifies.
Bounded consumption rechecks runtime and code as well as source/product bytes;
explicit close is the documented early-termination contract. Activation intent
retains previous pointer identity/value for precise recovery and rollback audit.
No new mandatory source, lower95/90threshold, fallback gate credit or R21 return
evaluation was introduced. Actual corpus performance remains unmeasured.

Repair commit includes only the three native modules, two updated original native
fixtures, five new focused test files, two dataset docs and this report (13paths).
Registry schemas/product keys are unchanged. The separate read-only SEC budget
analysis report is not part of this repair commit. Git diff whitespace check
passed; root explicitly allocated the index lane. Commit identity follows in
the completion message; no actual gate is promoted by code/tests/commit.
