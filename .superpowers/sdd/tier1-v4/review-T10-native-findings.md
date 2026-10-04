# Independent Task10 native review — 86956a29

Requirements/spec verdict: NEEDS_CHANGES; original Task10 acceptance remains FAIL/unmeasured (0 PASS, 7 FAIL/unmeasured; independent manual source review 0/300). Code-quality verdict: NEEDS_CHANGES. No Critical finding; three Important findings below. The engineering checkpoint accurately disclaims economic/source certification and is not evidence that the original task is complete.

Scope: read the Task10 brief, binding gap-analysis §2 item8, R22, global constraints, author report, review diff and owned native production modules, relevant legacy parser and Task9 admission code, registry/docs and relevant test source. Accepted the author's recorded offline results without rerunning the unchanged suite. Used targeted static discovery because the graph tools were unavailable. No actual data/source/hash/metadata/HTTP/return/job operations, cleanup, worktrees, helpers, product edits or commits were performed. Only this report is written.

## Important 1 — explicit source guidance tables are universally rejected

Locations: `atx-db/src/atx_db/alpha_panel/guidance_native.py:52`; legacy producer `guidance.py:1003` (also heading-derived explicit labels need separate provenance handling).

`qualify()` requires `candidate.period_basis == 'stated'`, but every candidate emitted by the established table parser is stamped `period_basis='table'`, including fully explicit `Q4 FY2026` source labels. Thus independently proved accounting/currency/share/calendar metadata cannot admit any table value. A source-qualified EPS range present in a table becomes NULL with `target_inferred_or_unknown`, depressing the qualified population, revisions and actual surprise. This defeats the required source values and is not merely an unsupported actual bridge.

Narrow pure in-memory repro (5.83s, no fixture files, DuckDB or source accesses): `G.extract_guidance('Business Outlook\nMetric | Q4 FY2026 |\nNon-GAAP diluted EPS | $4.02 +/- $0.20 |', G.ReportedPeriod(None), 2026)` returned `[('table','ADJUSTED',2026,4,3.8199999999999994,4.22)]`; calling native `qualify` with a July2026 original qualified issue and a unique, matching document SHA/independent USD diluted share/calendar/scope proof produced `target_inferred_or_unknown` and `low=None`.

Fix: preserve explicit-versus-inferred target provenance when generating table/heading candidates and admit independently proved explicit labels. Do not simply whitelist all table candidates: the legacy producer currently discards its resolved `_pb`, so inferred table years must remain refused. Add a native qualified explicit-table fixture, an inferred-table refusal fixture and an explicit-heading case.

## Important 2 — a known latest ambiguous issue can disappear from prior selection

Locations: `guidance_native.py:54-56`; `guidance_kernels.py:17-26`.

Qualification rejects ambiguous evidence before attaching period_end/calendar/currency/accounting/share IDs. `prior()` first applies equality on all comparator IDs, so this latest NULL-valued row is excluded before the intended latest-ambiguity check. Older good guidance then supplies a numeric revision/surprise. Even two otherwise identical independent mappings for the same exact target and basis exhibit this behavior. The docs and frozen contract expressly promise that a latest known ambiguous/unqualified comparable issue blocks stale fallback.

Same narrow repro: take a valid old candidate/proof; qualify a later identical target candidate with `[proof, proof]`; select a prior for a still-later same-key actual. Observed later reason `target_currency_share_evidence_missing_or_ambiguous`, but `K.prior(actual,[old,new])` returned old accession with reason `qualified`.

Fix: retain independently established target/basis identity separately from value qualification, and track latest issue blockers before choosing a midpoint. For conflicting mappings, block only affected identities using explicit ambiguity provenance; do not invent a qualified mapping. Add native pipeline assertions that duplicate/conflicting latest evidence leaves revision and EPS surprise NULL, including public replay. Missing-source releases whose exact target is genuinely unknown need a documented separate policy.

## Important 3 — public feature replay does not check the complete feature population

Location: `guidance_native.py:320-328`.

The feature validator builds issuer IDs solely from parsed guidance UNION admitted actuals, then compares feature rows only for each of those issuers. There is no orphan-issuer or total-row check on `published_features`. A producer generation containing an extra feature row for a CIK absent from both inputs can pass semantic regeneration when its legitimate byte/schema/row records describe that generation. File authentication only proves the declared bytes; it does not establish that every published event was regenerated. This falls short of the report's 'exact full event rows; no success-only validation' promise. Static control flow establishes the omission; no corpus/job or checksum mutation was attempted.

Fix: refuse any published feature CIK outside the source union and verify the global feature-row count against the sum of regenerated rows, in addition to the existing per-issuer multiset comparisons. Add a small offline publication fixture with an extra absent-issuer row and updated producer output records; require semantic validation to refuse it.

## Other reviewed requirements and limitations

R22 is correctly frozen as dimensionless EPS actual-minus-prior-midpoint divided by positive same-basis prior-session price. Revenue surprise remains explicit unit-mismatch NULL and native-unit revisions are separate. Exact target/basis/accounting scope/currency/share/calendar keys, strict earlier clocks and session-close/availability checks, equality refusal, no reaction-price fallback, unsupported actual bridges, and Task9 native EPS qualification are visible in the implementation. No evaluation or complete forward-label job is introduced.

Admission checks current Task9 schema/parser/code, exact parsed phase, immutable generation/products and SEC/catalog lineage. Isolated generation publication, retained authenticated release units, last-written receipts, private pending work, OS ownership locks, byte/schema/row authentication, source rechecks, original/amendment population retention and fixed original-release sampling are implemented. Parse bytes and original lineage are retained in feature publications. These are useful engineering safeguards, subject to the replay omission above; hashes and caller-reviewed evidence contracts are not independent economic certification.

The manual gate pins parse/config/source hashes, requires fixed sample identities and independent expected source fields, refuses a bare correct=true assertion, uses denominator300 and cannot PASS a shorter sample. Six independently declared yearly member counts are required and checked before coverage; yearly numerator uses original filing year and qualified native source values. Actual authority/completeness of reviewed evidence, full member populations, source issue clocks, 300 independent original-text judgments, yearly measured shares, real resources and full-corpus behavior remain unverified. The existing report correctly retains `source_quality_certified=false` and `NOT_PRODUCTION_READY`; do not convert engineering fixture results into the seven actual gates.

Verification command for the only new repro: PowerShell set `PYTHONPATH=C:/atx/atx-db/src`, `OPENBLAS_NUM_THREADS=1`, then pipe the short Python program described in findings1/2 to `atx-db/.venv/Scripts/python.exe -`. Exit0, wall5.83s. The program imported G/N/K, constructed only in-memory source-shaped dictionaries, and printed both observed failures. No unchanged pytest suite was rerun.
