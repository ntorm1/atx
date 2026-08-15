# US Equity Fundamentals Provider Design

Status: implemented foundation, migration 0297, measured 2026-08-15. This document is
the design contract for turning ATX into a revision-complete US equity fundamentals
provider rather than a single latest-value SEC extract.

## Product contract

The service is organized around explicit dataset and schema identifiers, stable security
IDs, point-in-time symbol mappings, reproducible request metadata, and versioned record
schemas. This follows the useful parts of Databento's model: a schema is a documented
field contract; stream/file metadata is sufficient to reproduce a request; instrument
identity is separate from display symbols; and historical symbol mappings preserve the
symbols that were valid at the time. Databento documents these principles in its
[schema model](https://databento.com/docs/schemas-and-data-formats/whats-a-schema),
[DBN metadata](https://databento.com/docs/standards-and-conventions/databento-binary-encoding),
and [point-in-time symbology](https://databento.com/docs/standards-and-conventions/symbology).

For fundamentals, the equivalent record clock is not one timestamp. Every published
fact keeps the economic period, filing/acceptance time, warehouse load time, and the
`available_at` time at which a point-in-time consumer may use it. Original and restated
revisions remain addressable. A symbol is a dated mapping to a security, never the
security's primary key.

Large deliveries use the same reproducibility rule as the interactive API. Job acceptance
pins a normalized query hash (including `as_of`) and a SHA-256 of the complete public field
contract. Streaming generation computes one logical Arrow-record digest independent of
Parquet, Arrow, CSV, or JSONL encoding, then separately hashes the delivered bytes and the
JSON manifest. The manifest embeds the exact request, schema definition, counts, billing
bytes, and all three identities. Data-artifact expiry does not erase the account-scoped
audit manifest. This follows Databento's useful convention that file metadata contains the
parameters needed to request the same data, while addressing the extra backfill and
restatement identity required by fundamentals.

The public layers are deliberately separate:

| Layer | Contract | Representative surfaces |
| --- | --- | --- |
| Source evidence | Lossless SEC provenance and filing structure | `sec_submissions`, `sec_company_facts`, `xbrl_filing_contexts`, `xbrl_filing_dimensions`, `xbrl_filing_facts` |
| Canonical facts | Provider-neutral items, units, signs, periods, revisions | `fundamental_item`, `fundamental_standardization_rule`, `fundamental_standardized` |
| Industry statements | Same items routed through issuer-type statement templates | `entity_industry_template_history`, `fundamental_industry_standardized` |
| Validation | Accounting identities with exact inputs and filing-context evidence | `fundamental_reconciliation_rule`, `fundamental_reconciliation_result`, `fundamental_reconciliation_serving` |
| Delivery control | Reproducible builds, schemas, coverage, entitlements, and quotas | build manifests, API schema catalog, provider coverage, SaaS control-plane tables |

## Filing source model

SEC CompanyFacts is useful for broad incremental collection but is not the authoritative
substitute for a filing instance: it can omit historical concepts, dimensions, issuer
extensions, and presentation relationships needed to explain a value. The warehouse
therefore retains both paths and prefers filing-instance evidence when validating an
identity.

Modern filings are parsed from inline XBRL in the primary HTML document. Historical
filings are parsed from the XML instance attachment. The SEC states that EDGAR archive
directories expose `index.json`, alongside HTML and XML directory indexes, for automated
crawling in [Accessing EDGAR Data](https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data).
Legacy discovery reads the accession directory's `index.json`, accepts the single
issuer/date-shaped XML instance, rejects calculation/definition/label/presentation
linkbases and generated report XML, and fails closed when the candidate is ambiguous.
The implementation is consistent with the SEC's current
[EDGAR XBRL Guide](https://www.sec.gov/files/edgar/filer-information/specifications/xbrl-guide-2026-06-29.pdf),
which treats inline and xBRL-XML as formats for facts supported by contexts, units, and
taxonomy metadata.

Each context, dimension, and fact records:

- `primary_document`: the filing's primary HTML document, used for filing joins;
- `filing_primary_document`: an explicit lineage copy checked against the filing;
- `instance_document`: the exact HTML or XML document from which XBRL was parsed;
- `instance_format`: `inline_xbrl` or `xbrl_xml`;
- accession, CIK, security, form, filing and acceptance timestamps, source URL, run ID,
  source-load time, context/unit references, namespace-qualified concept, and source
  ordinal/line.

Quality gates reject missing or contradictory document lineage, orphan contexts/facts,
bad instance formats, and duplicate instance-scoped keys.

## Standardization policy

The canonical registry currently contains 235 distinct provider-neutral items, 455
versioned standardization rules, and six statement templates (`ALL`, bank, insurer,
REIT, utility, and broker-dealer). A rule is a governed mapping, not a heuristic learned
from the value being reconciled. It defines taxonomy/concept precedence, period type,
industry applicability, sign, unit, validity interval, and derivation behavior.

Standard taxonomy concepts with stable semantics are standardized globally. Issuer
extensions are never promoted globally merely because they make an equation balance;
they require a scoped mapping keyed by issuer, extension concept, canonical item,
effective dates, and approval evidence. This prevents one issuer's custom label from
silently contaminating every other issuer.

Temporary equity illustrates the policy. Item 1224 now recognizes the governed standard
US-GAAP alternatives used across filing generations, including temporary-equity
redemption value and redeemable noncontrolling-interest carrying amount. The AEP custom
extension remains issuer-scoped rather than entering the global concept list.

## Reconciliation policy

Nineteen versioned accounting rules run across every eligible point-in-time revision.
The central rule is simple: an accounting difference is a hard provider failure only
when the compared facts are proven to belong to the same filing, context, period,
dimensions, and compatible units. Missing filing evidence, mixed accessions, or an
unaligned context is diagnostic coverage debt, not proof that the issuer's accounts do
not balance.

Every result retains weighted input rows, chosen concepts, item IDs, accession numbers,
context IDs, units, tolerances, rule version, classification history, and extension-map
evidence. This aligns the balance-sheet identity with XBRL US DQC_0004, documented as
assets equaling liabilities plus shareholders' equity in the
[approved DQC rules](https://xbrl.us/home/priorities/data-quality/rules-guidance/).
Tolerance handling is context-aware because XBRL calculations must account for reported
accuracy and duplicates; XBRL International's
[Calculations 1.1 Recommendation](https://www.xbrl.org/Specification/calculation-1.1/REC-2023-02-22%2Bcorrected-errata-2024-02-14/calculation-1.1-REC-2023-02-22%2Bcorrected-errata-2024-02-14.html)
formalizes those concerns.

The API reads an indexed serving table rather than recomputing the reconciliation graph
per request. Publication uses deterministic staging and a transactionally atomic upsert
plus stale-row prune. Every build manifest records whether it was full or scoped, the
published row count, maximum availability time, order-independent content hash, and
maximum upstream source-load watermark. Critical parity and freshness checks detect
tampering, incomplete publication, or a serving table older than its inputs.

## Reconciliation-driven coverage loop

`filing_context_backfill_queue` converts the latest unresolved `single_filing` /
`context_not_loaded` results into one operational row per security and accession. It
retains the affected rule/period/reconciliation counts, maximum observed difference,
submission metadata, direct SEC directory/index/document URLs, inline-versus-XML format
hint, estimated request count, and whether any prior context rows exist. Ranking is
deterministic: actionable filings precede blocked metadata gaps, then P0 mismatches, P1
diagnostic differences, P2 error-rule verification gaps, and P3 residual work.

Queue publication is linked to the exact reconciliation build ID and content hash. Its
own manifest records row/readiness/block counts, affected reconciliation count, maximum
availability time, and a whole-table checksum. Critical checks enforce row shape,
accession uniqueness, manifest parity, and freshness against the newest reconciliation
publication. A warning reports blocked rows rather than silently dropping them.

`filing_context_backfill_attempts` is the append-only execution ledger. A bounded worker
claims one accession at a time in priority order, records the exact queue build and
attempt sequence, and commits success or failure independently. Successful rows retain
context/dimension/fact counts and estimated/actual request counts; failed rows retain
exception type, message, retry eligibility, and a finite attempt budget. Automatic replay
is limited to transport failures and retryable HTTP statuses; deterministic parser/schema
failures require intervention and the explicit `retry_nonretryable` operator override.
Expired `running` leases are recovered before new claims. Critical checks reject malformed
state or duplicate attempt sequences, while warnings expose stale workers and exhausted
retries.

Submission discovery accepts explicit CIK targets in addition to display tickers. This
is required for historical continuity: a current ticker directory can point at a
successor entity even while older filing facts and accessions belong to the predecessor
CIK. The executor itself selects by accession with no ticker filter.

Inline filings require one document request; legacy filings require the accession
`index.json` plus the discovered XML instance. Execution must use an identified user
agent, cache source artifacts, honor HTTP caching/backoff responses, and stay below the
SEC's aggregate fair-access ceiling. The SEC currently documents a maximum of 10
requests per second in its
[EDGAR rate-control notice](https://www.sec.gov/filergroup/announcements-old/new-rate-control-limits);
ATX's shared SEC client uses process-wide 110 ms request spacing, respects `Retry-After`,
and applies bounded exponential retries to 429 and transient 5xx responses.

## Measured proof slice

The disposable eight-issuer slice spans technology, energy, utilities, insurance,
asset management, REITs, and banking: AAPL, MSFT, XOM, AEP, AIG, AMP, AMT, and JPM.
No operation in this work was applied to the live warehouse.

At the migration 0285 baseline the slice contained 103,208 standardized rows with zero standardization
exceptions and 9,003 published reconciliation revisions. A full reconciliation publish
takes roughly 10–17 seconds on this local slice; indexed filtered reads take roughly
13–19 ms, while the full reconciliation quality sweep fell from about 165 seconds over
the dynamic view to about 0.08 seconds over the serving table.

Six AIG legacy filings were the decisive historical test. SEC accession discovery loaded
3,343 contexts, 5,566 dimension members, and 13,877 facts from their official XML
instances. The remaining temporary-equity residuals exposed two standard US-GAAP
concepts absent from CompanyFacts. After governing those alternatives, all six periods
moved from `context_not_loaded` to `verified_same_context` and reconciled with zero
residual. The outcome is important: the fix came from source evidence and a reusable
semantic mapping, not an issuer-specific balancing guess.

The first queue-driven execution selected AIG's 2024 10-K accession
`0000005272-24-000023`. Its official inline instance added 3,780 contexts, 8,559
dimension members, and 12,478 facts. After rebuilding the downstream chain, all 11
affected reconciliation revisions left `context_not_loaded`: five became
`verified_same_context` and six became the more specific `context_not_aligned`; no hard
failure was created. The queue contracted from 455 filings / 1,472 affected revisions to
454 / 1,461, proving that its impact accounting closes exactly over a completed load.

The SEC metadata recovery then loaded the current 10,396-symbol company-ticker snapshot
and complete submission histories for all eight issuers. It exposed a real identity
transition: current XOM maps to successor CIK `0002115436`, while the historical queue
belongs to CIK `0000034088`. Explicit-CIK ingestion recovered 3,554 predecessor filings.
The queue freshness gate failed before each republish exactly as designed, and the final
metadata refresh made all 454 filings actionable: 185 inline XBRL and 269 legacy XML,
with zero blocked rows.

The durable executor's first real bounded batch processed the top five P1 filings with
zero failures. Five SEC requests added 9,002 contexts, 20,273 dimension members, and
31,149 facts. The downstream rebuild produced 39,417 filing metrics, 103,208 standardized
rows with zero exceptions, and 9,003 reconciliation revisions. Across the five loaded
accessions, 22 results became `verified_same_context`, 23 became
`context_not_aligned`, and one pre-existing result remained `mixed_filing_vintage`; no
hard mismatch was introduced. All 45 queue-attributed gaps closed, reducing the plan to
449 filings / 1,416 affected revisions. All five queue checks and all four attempt-ledger
checks passed.

The next ten priority filings also completed without a failed attempt, adding 25,152
contexts, 56,953 dimensions, and 79,620 facts. Their 69 queue-attributed gaps closed
exactly after the same downstream rebuild, leaving 439 actionable filings / 1,347
affected revisions and all nine operational checks passing. This validates repeatable
batch progression rather than a one-off filing proof.

The first rate-limited tranche processed another 18 unique filings and closed 94 gaps.
A truncated 2013 SEC response succeeded on attempt two, proving transport retry state.
One 2023 AEP 8-K failed deterministically because its designated 93 KB primary document
was only an inline cover page: 46 facts referenced contexts stored in a 24 MB companion
document. The bounded accession-index fallback selected that matching base document on
attempt three and loaded 4,448 contexts, 10,649 dimensions, and 13,731 facts using three
requests with exact primary/instance lineage. After rebuilding, the slice contains
39,419 filing metrics, 103,210 standardized rows with zero exceptions, and 9,004
reconciliation revisions. The queue now holds 421 actionable filings / 1,253 affected
revisions, a cumulative closure of 33 filings / 208 gaps from the fully actionable
baseline; all operational checks pass.

A subsequent bounded tranche processed 20/20 filings successfully using 30 requests and
loaded 40,031 contexts, 82,391 dimensions, and 133,204 facts. The downstream rebuild
contains 39,437 filing metrics, 103,227 standardized rows with zero exceptions, and 9,013
reconciliation revisions. The priority queue fell to 401 actionable filings / 1,153
affected revisions, closing another 20 filings / 100 gaps and bringing cumulative closure
from the fully actionable baseline to 53 filings / 308 gaps. All nine queue and attempt
ledger quality checks pass.

The next XML-heavy tranche processed 20/20 filings with zero failures using 36 requests:
16 historical AEP XML instances from 2010-2016 plus legacy-CIK XOM and AIG inline filings.
It loaded 22,756 contexts, 46,377 dimensions, and 83,812 facts. The downstream rebuild
contains 39,449 filing metrics, 103,239 standardized rows with zero exceptions, and 9,019
reconciliation revisions. The queue now contains 381 actionable filings / 1,057 affected
revisions (241 XML / 590 gaps and 140 inline / 467 gaps), bringing cumulative closure from
the fully actionable baseline to 73 filings / 404 gaps. All nine operational checks pass.

Migration 0286 adds immutable content-addressed source caching to this execution path.
Every EDGAR directory index, inline document, companion document, and legacy XML instance
is stored under its SHA-256 digest; `raw_source_files` retains the verified path, digest,
byte count, URL, and filing metadata. Attempt rows persist total source artifacts, actual
network requests, and cache hits, with a critical equality invariant across those counts.
A real 2021 XOM filing proof cached a 1,057,478-byte document on its first request and then
replayed the same 134 contexts, 156 dimensions, and 614 facts with zero SEC requests and one
verified cache hit. Corrupt cache bytes are rejected and atomically repaired on refetch.

The first cache-enabled queue tranche processed 20/20 filings with zero failures, persisting
36 source artifacts from 36 network requests alongside 18,639 contexts, 35,574 dimensions,
and 75,306 facts. It closed another 20 filings / 80 attributed gaps, leaving 361 filings /
977 affected revisions (225 XML / 526 gaps and 136 inline / 451 gaps). The cache then held
37 verified payloads totaling 186,215,851 bytes, including the independent XOM replay proof.
The attempt-ledger equality and all nine queue/executor gates pass.

SEC guidance treats all inline attachments in a filing's primary Inline XBRL Document Set
as one target instance. The AEP cover-page fallback now therefore reparses and merges the
cover plus resource-bearing companion under one logical instance/context identity while
retaining each fact's exact source-document URL. The real 2023 AEP proof grew from 13,731
facts in the companion alone to 13,777 across the IXDS, recovering all 46 cover-only facts;
the next replay produced the identical 4,448 contexts, 10,649 dimensions, and 13,777 facts
from three cache hits and zero SEC requests. All ten context/dimension/fact integrity gates
pass. This implements the observed cover/companion pattern; general attachment-type-aware
IXDS discovery remains part of the full Arelle integration work.

The following cache/IXDS tranche completed 20 unique filings. Nineteen succeeded on their
first attempts; one 2009 AEP XML response ended prematurely and was correctly classified as
a transient transport failure. Its second attempt reused the already cached directory index
and fetched only the missing instance, satisfying two artifacts with one cache hit plus one
network request. The tranche loaded 11,004 contexts and 44,377 facts in total. Downstream
state reached 39,600 filing metrics, 103,276 standardized rows with zero exceptions, and
9,022 reconciliation revisions. The queue fell to 341 filings / 910 affected revisions
(210 XML / 474 gaps and 131 inline / 436 gaps), for cumulative closure of 113 filings / 551
gaps from baseline. All nine operational gates pass.

Migration 0287 introduces an optional, durable Arelle validation sidecar rather than
conflating the warehouse's deliberately bounded SQL-DQC rules with full processor
semantics. `xbrl_processor_runs` versions every processor/profile execution and records
its security, accession, IXDS entrypoint, exact command, connectivity/cache mode, exit
state, counts, PIT timestamps, and latest revision. `xbrl_processor_findings` retains
structured severity/code/message attributes and source references. Four quality gates
enforce valid run rows, one latest revision per processor profile, valid finding rows, and
no orphan findings.

The optional `xbrl` dependency resolved Arelle 2.44.2. The first real, strictly offline
AEP 2023 IXDS smoke completed under the truthful
`xbrl21_calc11_round_to_nearest` profile and persisted three structured `IOerror`
findings for entrypoints absent from Arelle's URL cache. This proved bounded CLI
execution, current `ixds` construction, XML-log parsing, durable findings, and revision
supersession without issuing SEC traffic; it did not claim semantic validation while the
DTS was absent. Offline remains the default and direct resolution requires explicit
`--online` operator choice.

The filing-package layer then projected verified content-addressed objects into
traversal-safe URL-shaped paths and assembled multi-document IXDSes into deterministic
ZIPs, avoiding Arelle's Windows surrogate-path ambiguity. The real AEP package capture
used five paced SEC requests for the extension schema and four linkbases plus three
verified cache hits for the directory index and inline documents. Re-parsing remained
identical at 4,448 contexts, 10,649 dimensions, and 13,777 facts. Its immutable archive
contains seven members and has manifest SHA-256
`8cb8640ab788140629f00b6518be2b4fd050300a6d4897b121cd40cdbe107399`.

Official 2022 FASB US-GAAP and SRT taxonomy packages plus the SEC 2022 package resolved
the standard DTS fully. Arelle then emitted 543 structured findings: 372
`ix11.11.1.2:invalidTransformation` errors requiring SEC's EDGAR transformation plugin
and 171 `calc11e:inconsistentCalculationUsingRounding` inconsistencies; the prior 24,000+
missing-definition/dimension cascade disappeared and no `IOerror` remained. Migrations
0288-0290 persist each taxonomy ZIP's SHA-256/size, the filing archive manifest/member
count, `dts_resolution_status`, and a separate `validation_outcome`. The measured run is
therefore correctly labeled `resolved` plus `validation_errors`, and all four processor
lineage/quality gates pass. Migration 0290 also makes each finding's latest-revision flag
follow its parent processor run, satisfying the warehouse-wide PIT contract. The complete
repository gate passes 1,500 tests with five expected skips. Official package references are the
[SEC standard-taxonomy catalog](https://www.sec.gov/data-research/standard-taxonomies/security-based-swap-data-repositories),
[FASB 2022 taxonomy directory](https://xbrl.fasb.org/us-gaap/2022/), and
[Arelle package CLI](https://arelle.readthedocs.io/en/latest/command_line.html).

Migration 0291 turns that manual package proof into a governed offline dependency catalog.
`xbrl_standard_taxonomy_package_revisions` keeps content-addressed official releases with
authority, taxonomy family/version, source URL, verified SHA-256/size, cache and
materialized paths, availability, and immutable revision lineage.
`xbrl_filing_taxonomy_packages` maps every recognized extension-schema import to the exact
package revision used for the filing. The catalog is deliberately separate from the
existing relationship-extraction table because a validation artifact revision and a
parsed taxonomy relationship build have different grains and lifecycles.

The real AEP capture parsed ten imports, recognized five FASB/SEC dependencies, and created
five filing edges backed by three packages: FASB SRT 2022 (182,036 bytes), FASB US-GAAP
2022 (6,485,888 bytes), and SEC 2022 (576,613 bytes). Their digests match the prior manual
proof. Immediate replay verified all three cached objects, issued zero network requests,
and inserted no duplicate revision or edge. A subsequent strictly offline Arelle run with
no explicit package paths auto-loaded the filing's catalog edges, resolved the DTS, and
reproduced the same 543 findings.

Migrations 0292-0294 make the catalog processor-ready and broaden the dependency grain.
Official archives that already carry `META-INF/taxonomyPackage.xml` and an OASIS catalog
remain byte-identical processor inputs. Archives without that metadata receive a
deterministic OASIS wrapper whose catalog rewrites the publisher's HTTP and HTTPS URL
space into the archive, while the original source URL, publisher ZIP, SHA-256, and byte
count remain separately governed. Filing edges now originate from every captured
extension schema and calculation, definition, label, or presentation linkbase; absolute
`schemaLocation` and `xlink:href` references are retained with their exact source
document. Five package/edge integrity gates cover row validity, one latest package and
edge revision, and referential integrity.

The 20-filing cross-era proof discovered 517 filing-package references, recognized 380,
and linked them to ten packages (four native OASIS and six deterministically normalized).
It added 306 edges and fetched exactly one new artifact: the official XBRL US 2009 US-GAAP
distribution. Replay used ten checksum-verified cache hits, issued zero network requests,
and inserted no duplicate package revision or edge. A legacy Ameriprise filing that had
141 incomplete-DTS findings then resolved fully offline with only two Calculation 1.1
inconsistencies and no errors; representative 2024 and 2025 filings also resolve without
taxonomy IO errors. This behavior follows the
[XBRL Taxonomy Packages 1.0 specification](https://www.xbrl.org/Specification/taxonomy-package/PR-2015-12-09/taxonomy-package-PR-2015-12-09.html)
and the [official XBRL US 2009 release](https://xbrl.us/xbrl-taxonomy/2009-us-gaap/).

The same tranche loaded 15,165 contexts, 28,358 dimension members, and 56,894 facts from
20/20 filings with zero failures. Its downstream rebuild produced 39,606 filing-derived
metrics, 103,278 standardized rows with zero exceptions, and 9,022 reconciliation rows.
All 50 queue-attributed gaps closed, reducing the queue from 341 filings / 910 gaps to
321 / 860 (202 XML filings / 450 gaps and 119 inline filings / 410 gaps). Cumulative
closure from the 454-filing / 1,461-gap baseline is now 133 filings / 601 gaps. All five
queue and four executor gates pass.

The next priority tranche completed another 20/20 filings with zero failures and added
17,179 contexts, 32,694 dimension members, and 60,990 facts from 140 SEC artifacts. Its
2019-2024 filings exposed two historical SEC distribution contracts: the documented
2019-2020 family archives use `{family}-{year}.zip`, while STPR 2018 publishes the complete
official XSD/XML directory without a ZIP. The resolver now follows the dated pre-2019 and
year-only 2019-2020 conventions and assembles directory-only releases into a deterministic,
same-origin package. The capture discovered 548 references, mapped 381 to 21 packages,
added 381 edges, and replayed with 21 cache hits and zero network requests. Representative
2019, 2020, and 2021 filings all resolve fully offline; the 2021 STPR-dependent filing has
no IO or undefined-member findings. The conventions are documented in the official
[SEC 2018 taxonomy release notes](https://xbrl.sec.gov/doc/releasenotes-2018.pdf) and
[SEC 2020 taxonomy release notes](https://xbrl.sec.gov/doc/releasenotes-2020.pdf).

The tranche rebuild produced 39,614 filing-derived metrics, 103,285 standardized rows with
zero exceptions, and 9,028 reconciliation rows. All 40 queue-attributed gaps closed,
reducing the queue from 321 filings / 860 gaps to 301 / 820 (202 XML filings / 450 gaps and
99 inline filings / 370 gaps). Cumulative closure from baseline is now 153 filings / 641
gaps. The governed catalog contains 31 current package revisions and 766 current dependency
edges across 41 filings and 200 source documents; all 18 queue, executor, processor,
package, and edge gates pass.

Priority tranche 9 completed 20/20 legacy XML filings from 2015-2019 with zero failures,
adding 12,507 contexts, 24,119 dimension members, and 44,880 facts from 160 artifacts. An
audit of all 2018 SEC family directories added the remaining documented exceptions: DEI
uses `dei-2018.zip`, RR retains its dated ZIP, and EXCH/STPR are directory-only releases.
The tranche discovered 510 references, mapped 320 to 22 packages, added 320 dependency
edges, and replayed with 22 cache hits and zero network requests. Offline Arelle smokes
resolved representative 2015, 2017, and 2018 filings; the 2018 filing was valid with zero
findings, while the other two reported only calculation inconsistencies and no DTS errors.

The tranche rebuild produced 39,616 filing-derived metrics, 103,303 standardized rows with
zero exceptions, and 9,030 reconciliation rows. All 40 queue-attributed gaps closed,
reducing the queue from 301 filings / 820 gaps to 281 / 780 (182 XML filings / 410 gaps and
99 inline filings / 370 gaps). Cumulative closure from baseline is 173 filings / 681 gaps.
The governed catalog now contains 50 current package revisions and 1,086 current edges
across 61 filings and 300 source documents; all 18 operational, processor, package, and
edge gates pass.

Priority tranche 10 processed 20 legacy XML filings from 2012-2014. Nineteen succeeded
initially; one response ended prematurely and was isolated as a retryable transport
failure. Its replay reused six verified artifacts and made only two SEC requests, bringing
the tranche to 20/20 and 17,316 contexts, 33,632 dimension members, and 61,841 facts. The
taxonomy pass discovered 546 references, mapped 357 to 21 packages, added 357 edges, and
replayed with 21 cache hits and zero network requests. Representative 2012, 2013, and 2014
filings all resolved offline; the 2012 filing was valid with zero findings and the other
two contained calculation inconsistencies only.

The tranche rebuild produced 39,634 filing-derived metrics, 103,319 standardized rows with
zero exceptions, and 9,038 reconciliation rows. All 40 queue-attributed gaps closed,
reducing the queue from 281 filings / 780 gaps to 261 / 740 (162 XML filings / 370 gaps and
99 inline filings / 370 gaps). Cumulative closure from baseline is 193 filings / 721 gaps.
The catalog now contains 57 current package revisions and 1,443 current dependency edges
across 81 filings and 400 source documents; all 18 operational, processor, package, and
edge gates pass.

Priority tranche 11 completed 20/20 filings from 2010-2012 despite an interrupted client
wait; the durable executor continued to completion and recorded 10,102 contexts, 17,710
dimension members, and 36,640 facts from 160 artifacts. Eighteen filings depend only on
seven already governed packages, producing 207 new edges and a zero-network, seven-cache-hit
replay. Two filings additionally require official SEC Currency 2011 and Invest 2011 ZIPs;
those exact dependencies remain explicit because the managed execution environment blocks
new outbound sockets. Representative 2010, 2011, and 2012 filings with complete packages
all resolved offline with zero errors and calculation inconsistencies only.

The tranche rebuild produced 39,656 filing-derived metrics, 103,327 standardized rows with
zero exceptions, and 9,044 reconciliation rows. All 40 queue-attributed gaps closed,
reducing the queue from 261 filings / 740 gaps to 241 / 700 (142 XML filings / 330 gaps and
99 inline filings / 370 gaps). Cumulative closure from baseline is 213 filings / 761 gaps.
The catalog has 57 current package revisions and 1,650 current edges across 99 filings and
490 source documents. All 213 latest filing attempts are successful, and all 18 operational,
processor, package, and edge gates pass.

Migration 0295 removes batch-level failure coupling from taxonomy capture.
`xbrl_taxonomy_package_capture_attempts` is an append-only, latest-revision ledger at one
row per run and package key. It records the exact authority/family/version/source, archive
or directory source kind, fetch/materialize/normalize stage, cache and request counts,
publisher and processor digests/sizes, completion/availability time, and bounded error.
The default capture continues after transport, filesystem, or package-validation failures;
`--fail-fast` is an explicit diagnostic override. Three gates enforce attempt validity,
one latest attempt per package, and visible warning state for unresolved latest failures.

The full tranche-11 batch was replayed under blocked outbound sockets to prove isolation.
Seven cached packages succeeded, two publisher fetches failed, 242 of 254 recognized
references resolved, and 35 additional filing edges committed. Both failures are durable
latest attempts (`SEC:currency:2011` and `SEC:invest:2011`) with exact `ConnectionError`
messages; all structural gates pass and only the expected count-two warning remains. Arelle
labels an affected filing `incomplete_dts` with two IO errors and one missing import, while
filings whose package sets are complete remain resolved. This prevents partial dependency
coverage from being reported as filing validity without sacrificing successful work.

Migrations 0296-0297 close the first customer-delivery reproducibility gap. Public schema
metadata now carries a deterministic contract SHA-256, and every durable batch row pins the
accepted schema version/hash plus the normalized query hash before entering the worker
queue. The worker rejects a queued job if that contract changes, computes an
encoding-independent logical-content digest while streaming, and persists byte-level
artifact and manifest hashes with traversal-safe URIs. `batch.get_manifest` returns the
exact checksummed JSON to the owning account; unlike the paid data artifact, this audit
record remains retrievable after delivery expiry. A four-encoding proof produced one
logical digest for identical rows while retaining distinct format-specific artifacts.
This implements the request-reconstruction property documented by
[Databento DBN metadata](https://databento.com/docs/standards-and-conventions/databento-binary-encoding)
and makes the stronger point-in-time contract explicit for the snapshot use cases described
by [S&P Compustat](https://www.spglobal.com/market-intelligence/en/solutions/products/fundamental-data).

EFM is a separate, explicit profile. The Arelle PyPI `EFM` extra installs dependencies but
does not ship the SEC-maintained EDGAR plugin. `--efm-plugin-path` must point to a
provisioned official EDGAR plugin checkout before the command adds `--efm` and records
`xbrl21_efm_calc11_round_to_nearest`. The Arelle and EDGAR versions must be compatibility
pinned. As measured on 2026-08-14, the
[official EDGAR repository](https://github.com/Arelle/EDGAR) identifies Arelle 2.39.8 and
XULE 30052, so it is not loaded into the proven Arelle 2.44.2 core environment without an
isolated compatible processor bundle.

The minimum standardization-template coverage check still remains below its
full-provider threshold on one sparse historical proof period. That is an explicit
coverage failure, not a parser failure or a reason to weaken the gate.

## Live warehouse promotion

On 2026-08-15 the governed migration path promoted the live warehouse from
migration 0266 to 0297 with a verified pre-flight backup
(SHA-256 `0a91b7963681740a80fcd59b1888fdaaabbab8b6e8ae4a537d6c10afa5d6a1b3`,
33,079,701,504 bytes) and post-apply schema and checksum verification. The first
full-warehouse standardized build (`62a0adbe`, rule set
`2ae542452f2969104c63e1dae7cec9e611b97c6197710bfb020c4826a1025e98`) consumed
6,089,615 input rows and published 7,340,932 revision-complete standardized rows
with zero standardization exceptions: 897,693 annual, 2,790,992 quarterly,
2,302,418 instant, and 1,349,829 TTM. The published surface spans 1,598
securities and 46 canonical items with economic periods from 1967 through 2026.

That breadth measurement is also the honest gap statement: `sec_company_facts`
currently holds 2,720 CIKs against the 10,433-symbol company-ticker snapshot,
and 46 published items sit well below the 235-item registry. Universe expansion
runs from the cached official bulk archives (`companyfacts.zip` and
`submissions.zip`, both 2026-08-09) through the existing bulk loaders; item
breadth is the statement-template and rule-governance work tracked below.

Migration 0298 added the `restatements` public schema:
`v_fundamental_restatement_events` publishes one immutable event per
standardized revision that changed a previously published value, keyed by
`(revision_group_id, revision_sequence)` so point-in-time vintage selection
never collapses distinct events. Each event retains the restating and
superseded accessions, first-reported baseline, per-event and cumulative
deltas, and the availability timestamps of both vintages.

The first full-universe reconciliation publish exposed a real capacity
contract: materializing the contextual reconciliation view for every revision
at once exceeded an 8 GB DuckDB memory limit on a 16 GB host while a parallel
test session ran. The refresh is being re-run isolated with an 11 GB limit and
reduced threads; symbol-sharded scoped publishes are the documented fallback.

## Remaining parity work

The foundation is provider-shaped but the product is not yet a complete FactSet or
Compustat replacement. The next priorities are:

1. Execute the remaining prioritized filing-instance queue across the proof universe,
   rebuilding metrics, standardization, reconciliation, and coverage after bounded
   batches; promote the same loop to the full US issuer universe.
2. Expand the governed SEC/FASB/XBRL US taxonomy-package catalog across all filing years and
   compatibility-pin an isolated official Arelle 2.39.8 / SEC EDGAR / XULE 30052 bundle;
   then execute EFM, renderer, transformations, and DQC/XULE profiles over the
   now-complete filing DTSes.
3. Increase canonical statement breadth, especially insurer/bank/REIT disclosures,
   footnotes, per-share bases, and acquired/discontinued operations.
4. Extend the proven per-delivery query/schema/logical/artifact/manifest identity into
   scheduled full-universe publications and restatement diffs; prove delisted-company
   continuity plus correction/deletion handling.
5. Complete contractual availability SLOs, schema-version negotiation, longer-lived
   publication catalogs, and benchmark suites against licensed vendor samples when available.

Coverage is an observed product metric, not a claim. Until these gates reach their
targets, the schema catalog must report `degraded` or `pending` rather than presenting
partial history as provider parity.
