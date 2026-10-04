# Independent readiness v6 review

Spec verdict: **PASS for this evidence-only refresh.**
Quality verdict: **PASS with two Minor auditability corrections.**
Critical0; Important0. No actual acceptance gate is promoted.

Reviewed refresh-readiness-v6.py, readiness-matrix-v5/v6.json and the six named
new evidence reports, plus their three small execution receipts and host guard
receipt. Task15 engineering review is an external prerequisite here, not a
self-review of the author's product. No product changes, source/warehouse reads,
hashes, HTTP, actual jobs, fixture runs, staging or commits. Only this report is
written; root owns matrix/generator and subsequent checkpoint publication.

## Preservation and generator equivalence

Independent JSON comparison: exactly46requirements,46unique IDs, same order.
Removing only newly appended observations makes every v6row exactly equal to
v5, including all original contracts, thresholds, denominators, commands,
statuses and prior observations in sequence. All29new observations have
checkpoint_version6 and gate_promotion=false. Top-level changes are exactly
schema, version, checkpoint and requirements; client_date is already2026-10-04
in v5, so remains equal. All other top-level release/workflow/authority contracts
are unchanged, including NOT_PRODUCTION_READY.

A separate read-only AST/literal reconstruction of the generator's six entries
and checkpoint exactly equals the complete v6JSON. The generator deep-copies
v5, checks existing artifact paths, asserts contract equality and creates v6
exclusively; it performs no gate measurements/source admission. It was read,
not executed (which would write a matrix).

## Evidence qualification

- t17-financial-progress-audit-v2.json names only batch0, four products and
  missing_batches1..84. Native192.3seconds/583569408peak bytes are directly
  recorded. Its qualification explicitly separates independent typed financial
  unit admission from original source-economic gates/all85final publication.
  Checkpoint admits exactly1unit/independently_checked[0], keeps financial final
  publication false and counts no additional unit from continuation94538.
  Extract85and universe2018..2022are unchanged v5observations; no inference
  that one financial unit completes the release.
- task11-submissions-progress-v4.json directly records99partitions,
  985667main members,5251history traversed,278228unique CIK-accession keys,
  zero duplicate CIK-accession rows and terminal exhaustion of the independently
  enumerated main axis. Exceptions19617and1665650remain open; directory history
  count5374differs from traversed5251. complete_source_qualified and
  availability_clocks_qualified are both false. V6preserves body, clock,
  history/exception, economic/cohort and ownership exclusions; no completed
  acquisition/source qualification follows from traversal.
- review-T15-text-native-r1-findings.md explicitly PASS/PASS for scoped
  repairdab239f5/all7findings; actual95%strict peers/90%text/full yearly populations
  remain open. V6distinguishes this from Task15 completion/source qualification.
- host-headroom-window-v4.json records31samples/300seconds, sampled minima
  commit3.5228GiB/physical2.2586GiB, condition met. The separate continuous guard
  receipt records mincommit2.9519GiB; neither is source acceptance. Pilot guard
  is0.8GiB/heavy/disk min40GiB, completed without cap. V6does not weaken GC4.
- sec-command-metadata-audit-v1.json supports1551entries/1402173bytes including
  recheck, two pytest/ruff executions, retained text loop without complete
  execution intervals, complete_invocation_inventory=false, budget_upper_bound
  null and remaining_budget_granted0. SEC HTTP hold remains explicit.
- R26 binds eight delivery plus four catalog findings to existing Task17
  repair2/5 and preserves full family/consumer/statistical gates. Helper75efb231
  exists (`git show --no-patch`) but receives no independent engineering approval
  from this review or v6; combined integration remains pending. No actual golden
  panel/labels, predictive measurements or atx-impl acceptance is claimed.

## Minor corrections

1. **Checkpoint key should name the exact counted key.**
   refresh-readiness-v6.py checkpoint calls278228
   `submissions_inventory_unique_accessions`, while the cited audit proves
   `unique_cik_accessions`. The appended observation correctly says distinct
   CIK-accessions. Rename the checkpoint key to
   `submissions_inventory_unique_cik_accessions`; retain value278228. No audit
   establishes globally distinct accession strings separately. This is a
   precise label correction, not a demonstrated false value or gate promotion.

2. **Timing/minimum claims need their direct receipt citations.**
   Financial audit26.0seconds/151269376bytes, inventory audit16.0seconds/
   84922368bytes and continuous guard2.9519GiB are supported, respectively, by
   `logs/t17-financial-progress-audit-v2.receipt.json`,
   `logs/task11-submissions-progress-v4.receipt.json`, and
   `logs/host-headroom-observe-v4.receipt.json`. They are not fields of the
   selected observation artifact itself. Add these existing receipt paths to
   the relevant observations (or remove only those ancillary timing/minimum
   details). No source rescan or changed contract/status is needed.

## Exact verification actions

Targeted Get-Content/Select-Object on named generator/reports; targeted rg on
named evidence and log filenames. Two read-only Python here-strings piped to
`atx-db/.venv/Scripts/python.exe -` loaded only these small report/config JSONs:
one asserted full row/prior-observation equality and printed46contracts/
29new observations; another parsed generator with ast.literal_eval and asserted
full payload equality, printing EXACT_GENERATOR_PAYLOAD_MATCH6entries. Small
receipt summary reads independently confirmed cited timing/memory/minimum
values. `git show --no-patch --format='%h %s' 75efb231 dab239f5` confirmed named
commit metadata only. All checks completed normally; an initial guessed host
receipt filename was absent and was corrected via restricted log filename
discovery. No generator or actual audit script was executed; no suites rerun.

This is checkpoint review, not a fresh current warehouse/source identity audit
or real-time continuation monitor. Any later units need their own independent
admission evidence and a subsequent readiness checkpoint. Root's unchanged
actual/source/economic/statistical/consumer requirements remain binding.

## Narrow correction rereview â€” 2026-10-04

**Spec PASS; quality PASS. Both Minor findings addressed.** Reviewed only the
corrected count-key/supporting-artifact additions and exact generator-to-matrix
match, as requested. Checkpoint key is now
submissions_inventory_unique_cik_accessions=278228; the old ambiguous key is
absent. Financial audit, inventory audit and host observations carry the exact
three previously verified guard receipt paths in supporting_artifacts, with
generator existence checks and identical emitted matrix values.

Read-only AST/literal reconstruction (same Python stdin command mechanism,
now including the supporting dictionary) exactly equals the corrected full
v6JSON; printed NARROW_CORRECTIONS_PASS/EXACT_GENERATOR_PAYLOAD_MATCH and
3supporting receipt mappings verified. No generator execution, broader/source
review, jobs, tests, product edits or index writes. Original review scope and
no-gate-promotion conclusion are unchanged; this supersedes its two open Minor
corrections. Root may checkpoint the reviewed draft within its existing scope.

## Narrow terminal-evidence rereview â€” 2026-10-04

**Spec PASS; quality PASS for the updated terminal checkpoint.** Scope only
the new interruption facts and exact generator/matrix equality. Direct
`logs/t17-runtime-b0001-b0004-v1.receipt.json` records stopped_low_commit,
return137,573.5seconds,617504768peak bytes, cap_hit=false. The inspectionv4
report explicitly records retained admission files[0,1,2], previously independently
verified[0], final_manifest_exists=false/current_pointer_exists=false and owner
lock10544preserved. Its qualification calls additional native admissions
provisional pending normal full source/typed replay. Inspection guard completed
2.0seconds/63860736bytes/exit0/no cap. Both direct receipt paths are attached to
the five added gate_promotion=false observations.

Updated checkpoint retains financial_batches_admitted1 and independently
checked[0], separately lists retained files[0,1,2]/pending replay[1,2], and records
the terminal host stop/no root job/no additional unit counted. Neither batches1/2
nor retained partial attempts receive current independent-admission or final
release credit. Prior Minor corrections remain applied. All original contract/
status records come unchanged from v5; this terminal draft now appends34
observations across seven entries, without promoting any gate.

Read-only Python AST/literal reconstruction including the updated checkpoint,
seven entries and supporting receipt dictionary exactly equals the entire
updated v6matrix; explicit assertions confirmed the1/[0]/[0,1,2]/[1,2]count
distinction. Output: TERMINAL_DRAFT_EXACT_GENERATOR_MATCH; five new nonpromoting
observations; only batch0counted. No actual source/product reads, jobs, tests,
generator execution, broader review or index writes. This checkpoint verdict
supersedes the earlier in-progress job-state description only; actual gates,
R26review dependency and original acceptance requirements remain open.
