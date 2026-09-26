# W1-D1 initial implementation: dated identity through fundamental alignment

Status: implementation slice, **not D1 acceptance**. No eligible historical
coverage measurement, data acquisition, warehouse read/write, migration execution,
or C++ compilation was performed. Code preceded the synthetic checks.

## Implemented contracts

`atx-db/src/atx_db/security_link.py` provides immutable filing evidence, original
SpiderRock security-line intervals, qualified dated links, dated overrides,
point-in-time resolution, and explicit unresolved/requested-decision coverage.
Issuer CIK comes from the issuer field, never from the accession prefix. Symbol
punctuation is preserved. Distinct filings must bracket the entire finite vendor
interval. The later proof advances link availability; retrospective confirmation
cannot backfill an earlier decision. Unknown interval ends remain unresolved.

Acceptance, independently established publication, revision availability, and
retrieval observation are separate clocks. Acceptance alone, missing vintage,
missing publication, and FSDS instance-prefix candidates do not qualify a link.
Late contradictory issuers and verified overlapping vendor lines add clocked
conflict markers. A future conflict cannot remove an earlier decision. Overrides
require qualified original evidence, reviewer/reason, and a separate adjudication
clock; conflicting active overrides remain unresolved.

The pure `sec_identity_sources.py` adapters consume already sealed in-memory
Insider SUBMISSION, FSDS SUB, or original filing projections. Naive acceptance
timestamps need an explicit timezone; ambiguous local timestamps need an explicit
offset. They perform no acquisition or database work. Current submission ticker
arrays and FSDS `prevrpt` do not establish historical symbol truth.

`atx.security-link/v1` is an exclusive immutable directory with hashed links and
unresolved tables, evidence-ID/source-SHA bindings, bounded verified reads, and a
manifest published last. Evidence hashes refer to original external projections;
raw filing documents and their retrieval receipts are not duplicated in this table.

The exporter defaults to `dated-links-v2`, consumes the link artifact and a
hash-bound pre-2020 Company Facts projection receipt, and emits explicit
`points.interval-v2.tsv`. It does not emit a legacy CSV under the new rule. The
old warehouse bridge requires `--bridge-rule legacy-static-v1`. The new output
binds source/link/context manifests and all output hashes; its manifest is renamed
into place last. Company Facts fiscal arithmetic remains unchanged.

The authorized consumer expansion changes `PitRecord` and `align_pit_records` in
`fundamental_fields.hpp`, adds the pure `fundamental_fields_artifact.hpp` v2 TSV
decoder, and adds `data_security_link_intervals_test.cpp`. Dated records carry
owner, identity interval, identity-availability clock, marker and override priority.
The aligner expires old issuers, independently delays identity and filing clocks,
withholds ambiguous identities even when a competitor has no accounting facts,
and permits an older fiscal period from a valid successor. Explicit legacy records
keep their existing path; mixed versions on one security line are rejected.

## Checks

- Pure security-link/source tests: **8/8**, 0.015 s, native exit 0;
  `C:/atx-wt/pool-3/build-equity/d1-security-link-tests.log`.
- Synthetic interval exporter checks: **4/4**, 0.088 s, native exit 0;
  `C:/atx-wt/pool-3/build-equity/d1-export-tests.log`.
- Existing fundamental exporter arithmetic checks: **9/9**, 0.002 s,
  native exit 0. No production payload or DuckDB fixture was opened.
- Three new C++ checks are source only, **not compiled or executed**. Root owns
  the next combined CMake/build batch. The decoder assumes its input member was
  validated against the export manifest by the file-loading caller.

The synthetic integration creates two issuers sequentially occupying one vendor
line, exports actual synthetic Company Facts snapshots, verifies all output hashes,
and checks link delay, an unavailable gap, successor selection and exclusive expiry.
This verifies machinery, not historical identity coverage or tradeable alpha.

## Remaining D1/D3 integration and evidence

1. Acquire sealed original filing/vendor projections with independently supported
   publication and revision/vintage clocks. Automatic whole-interval bracketing is
   deliberately conservative and may offer little usable coverage before expiry.
2. Active `ticker_history.py` / `security_master` loader behavior is untouched.
   Coordinated source migration and materialization are still needed; no warehouse
   schema or active loader was changed in this slice.
3. The new C++ decoder is callable source. No existing repository stage loaded the
   old standalone fundamental CSV, so a production file-loader/manifest-verification
   seam remains to be selected and wired in a later authorized stage batch.
4. Company Facts uses the existing modeled filed-date policy. D3 must establish
   verified accounting availability; new identity clocks do not upgrade that policy.
5. Recover the original 82 recycled-ticker fixture, or publish a separately named
   bounded regeneration with its own count. The original 82 are **not resolved**.
6. Measure 2013–2019 t1000/t3000 operating-company coverage, exclusions and survivor
   strata, non-survivor gap, and out-of-window violations on eligible evidence.
   Requested-decision coverage is explicitly not that plan gate.

No original DAG element is marked complete by this report.

## Independent-review corrections (after a2f3f373)

The strict identity resolver and new Python/C++ interval paths now require both
link and filing availability to be **strictly earlier** than the decision/session
key. Equality is withheld; the legacy lower-bound alignment is preserved. Strict
exports also verify the JSON body's canonical issuer CIK against the linked CIK
before emitting owner-labelled facts, including when a hash-bound ZIP member has
the wrong issuer body. The synthetic tests include equality and mislabeled-member
regressions: core **8/8**, 0.019 s; export **5/5**, 0.091 s, exit 0. Logs are
`build-equity/d1-clock-identity-{core,export}-tests.log`. Four C++ fixtures remain
uncompiled source.

The automatic whole-interval rule is not a usable broad live master: it waits for
a right-hand proof at/after the last valid calendar day, offering at most a narrow
final-day decision opportunity and commonly none. A separately authorized causal
prospective rule is required; the retrospective rule must not backfill history or
be represented as meeting the original D1 acceptance gate.

## Separate prospective rule: causal corroboration V2

`build_prospective_links` adds the explicit `prospective-two-filings-v2` evidence
rule. Two agreeing, qualified issuer/symbol observations must have distinct
accessions and effective dates inside the same known vendor line. Link availability
is the maximum of the vendor known-at clock and both original public proof clocks;
live eligibility is strictly later. This is prospective corroboration, **not** the
original plan's retrospective whole-interval acceptance proof.

An open vendor line remains economically open and is clipped only by the declared
pre-2020 artifact seal (`end_kind=seal-bound`). A finite vendor endpoint is accepted
only as part of that row's original known-at payload. Later-learned endpoints must
be separate hash-bound `VendorExpiry` events; they must not silently rewrite the
open row. Both effective expiry and its independent verified availability are
required before invalidation. Expiry/conflict markers persist through the open
line's seal, carry no invented accounting values or EDGAR acceptance timestamp,
and survive artifact/export/aligner projection. Contradictory future-effective
observations start their marker at that effective date, preserving earlier state.

`atx.security-link/v2` persists method, end-kind, strict-clock rule, and source
bindings. V1 artifacts remain readable under their original methods. V2 semantics
cannot be mislabeled V1. The original `build_links` remains retrospective audit;
`resolve_link(..., retrospective_audit=True)` is its explicit audit opt-in.
Live resolution and the fundamental exporter exclude its V1 automatic methods;
direct projection rejects audit-only links rather than emitting tradable fields.
Reviewed manual overrides retain their explicit priority and own dated clock.

Postimplementation final checks: core **14/14**, 0.059 s; export **6/6**, 0.161 s,
native exit 0. Logs `build-equity/d1-prospective-final-{core,export}-tests.log`. New cases cover
usable open intervals, strict corroboration timing, independent-observation rules,
future-effective known-early conflicts, late-known expiry, persistent invalidation,
versioned round trips, and actual exporter projection of no-value expiry markers.
Five C++ checks are source only; no C++ compilation or data evaluation was run.

All original acquisition, operating-company coverage, non-survivor, original82,
active-loader and production file-reader/manifest-verification gaps remain open.
