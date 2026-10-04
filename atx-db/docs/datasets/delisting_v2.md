M&A terminal events v2 (engineering preview)
===========================================

Status: initial engineering checkpoint ready for independent review. Native financial/price/economic readers, conditional exact returns, immutable publication and public full native replay are implemented. Actual original economic proofs and actual gated publication remain open. No certified actual v2 stage has been published; legacy v1 and foreign labels are preserved.

`mna_native_v2.py` consumes original bytes, not parsed rows. An independently
reviewed immutable caller contract must pin the complete object inventory,
source SHA256, accession/CIK, expected code SHA256 for all native readers and producer modules,
UTC retrieval and availability clocks. The public validator verifies the full
schema, unique keys and every output value by native replay. Native SEC full
submission identity and acceptance headers are matched exactly; a submission
with multiple documents is unsupported pending a pinned selection contract.
HTML-only originals get retrieval floors unless joined to an independently
pinned original SEC submissions recent entry with exact native issuer/accession
and matching native SGML acceptance. An ISO offset or Z alone does not establish
the SEC bulk field's timezone semantics. Catalog assertions never supply clocks.
Archived arrays without native issuer identity are unsupported. These checks establish document semantics only, not
financial completion, party mapping or terminal returns.

Deal vintage keys: `(accession, source_id, available_at)`; rows carry their own
native document clock. Later terms or statuses never overwrite earlier rows.
The parser retains unparseable/ambiguous candidates; it never treats Item 2.01,
Form 25, issuer primary-line metadata or a caller boolean as financial proof.

Terminal keys: `(security_id, last_session, candidate_available_at)` preserve original candidate vintages; `available_at` is the
maximum of event, terms, native completion, target and acquirer price clocks.
Daily PIT cutoff requires the previous independently admitted exchange session
at 22:00 UTC, strictly less; no calendar-day substitution. Monthly consumers
use their stamped session's 22:00 UTC cutoff and prices no later than that close.
Staleness: event vintage, no arbitrary carryforward of status or terms.

| Column | Type/unit | Definition/source |
|---|---|---|
| security_id | integer | Independently qualified target line, not issuer primary line |
| last_session | date | Last target close session in complete candidate inventory |
| candidate_available_at | UTC timestamp | Original admitted candidate vintage clock |
| available_at | UTC timestamp | Latest admitted input availability |
| legacy_continued | boolean | Original heuristic, retained even when source conversion resolves it |
| deal_id | text nullable | Original-source deal identity |
| proceeds_per_share | double, currency/target share nullable | Cash or acquirer terminal close times conversion ratio plus cash; conditional qualification only |
| currency | text nullable | Explicit native currency; dollar symbol alone is ambiguous |
| dlret | double nullable | Qualified proceeds / target last close minus one, policy-qualified individual source value; evaluation/forward labels pre2023 only |
| dlret_exact | boolean | True only after original native financial/class/price/basis replay under frozen pre2023 policy |
| dlret_imputed | boolean | False; no invented zero or imputed exact return |
| reason | string array | Every unresolved provenance/class/unit/clock/value condition |
| source_ids | string array | Retained original source identities |
| settlement_date | date nullable | Native dated settlement, not filing/item date |

Cash/stock/mixed terms need explicit share class, units, currency and signed
price basis. Adjustment factors and FX must be independently native-qualified;
this core never guesses them. CVRs, elections, proration or unresolved fractional
treatment remain NULL. A terminal return does not certify residual-month cash,
conversion treatment, intermediate prices/actions or a complete forward label.
Evaluation observations and complete forward windows end before 2023-01-01; source
and value coverage continue all2020+. The standalone formula is offline-fixture
only, not a builder certification API.

Root metadata reconciliation preserves original2267 raw M&A candidates:
1669 legacy continued=false plus598 continued=true. Legacy continuation is
unqualified and cannot remove candidates. Source-proven terminal/conversion
qualification is still needed for those598; all missing/failed cases remain in
the numeric gate denominator. No gate has been relaxed.

| Actual gate | Status | Measured |
|---|---|---|
| Original candidate reconciliation | PASS (root metadata only) | 2267=1669+598 |
| 200-deal handcheck >=95% | FAIL | Unmeasured; no admitted review corpus |
| Consideration parsed >=80% | FAIL | Unmeasured; no actual native replay run |
| Exact DLRET >=80% of2267 | FAIL | Unmeasured; financial admission/returns not run |
| Per-year/universe2020-2025 coverage | FAIL | Unmeasured |

Known limits: strict named-party monetary/date sentence grammar, UTF8 originals, common/rawUSD unchanged conversion basis, conservatively rejected ambiguity. Real source economic documentation is absent. Full actual resources are unmeasured; large original vendor files are streamed/hashed per replay, so controller must measure bounded throughput before a full price job.
No actual source acquisition, HTTP, source/hash scans or data builds were done
by this implementation lane.


Native financial and price contract
-----------------------------------
`mna_financial_v2` retains literal announcement/completion/termination/settlement dates separately, named common-stock target/acquirer parties, cashUSD or stock ratios and all original consideration clauses. Filing-item codes never certify status. Unambiguous announcement terms can be extracted while terminal completion is unresolved; later documents retain later clocks. Avon-style ADS/ordinary-share elections, fractional cash, preferred cash that was not payable, adjustable or multiple share clauses remain keyed unsupported with NULL terms.

`mna_price_native_v2` consumes original vendor Parquet with complete schema/SHA pins and native securityID/tradingDate/ticker_tk/close. It streams32768-row batches and refuses duplicate SID/date or ambiguous ticker/date identities. It uses raw close; adjusted/prior/return columns never supply the target mark. Original identity is checked before and after streaming. Price availability is at least session22:00UTC plus all evidence clocks.

The separate original authoritative economic evidence must supply exact issuer/common-class/ticker, explicit USD-per-share, raw unadjusted traded-close definition, unchanged conversion terms covering split/FX, and exact dated validity quotations. Every quote and typed field is checked against the independently pinned original. Economic evidence uses a conservative pinned retrieval floor. Only common-share rawUSD unchanged-conversion basis is supported. Other FX/split/ADS cases stay unsupported; source footers, primary flags and code comments never supply these proofs. The root-audited TickerHistory3 lacks currency/class/unit fields; actual economic proof sources remain absent.

`mna_exact_v2.replay_exact` reconstructs terms/prices from original sources and complete independent expected pins; caller-created Terms/Price/Evidence objects are not proof. A frozen value-only policy calculates qualified proceeds without dividing by target close. A frozen pre2023-native-qualified policy conditionally computes proceeds/target-last-close minus1, only when all dated source/class/unit/currency/basis checks pass. A source-proven completed common-share conversion can resolve the legacy continuation heuristic without deleting its candidate or original flag. Residual-month cash/conversion and complete forward labels remain separate unimplemented consumer proof, consistent with R18.

`mna_publish_v2` requires an independently reviewed immutable contract pinned by expected SHA. It verifies the full original candidate Parquet (all raw M&A2020+, including continued), full source/receipt inventories (exact SEC URL, source SHA/bytes/retrieval clock), complete original input-manifest bytes, all reader/code identities before/after native compute and candidate/source links. Optional per-key native economic/price admissions bind their original sources and frozen return policy. Public validation reconstructs every full-schema row from originals, checks nonnull unique keys and rejects altered rows even if their hashes are updated.

Publication uses128MB/one-thread DuckDB, atomic Parquet and publish-last manifest under <output-root>/<contract-SHA>. Existing builds are validated/reused, never overwritten. Complete pending builds resume after full native replay; incomplete evidence remains retained. The root must run real jobs under the guard.

The independent manual review contract pins original review bytes, reviewer, frozen candidate keys and every expected native financial field/source SHA. Boolean correct flags or declared success counts are rejected. Missing/failed cases remain in the frozen denominator. Numeric gates retain200 handchecks>=95%, qualified consideration>=80% and exactDLRET>=80% of2267. Source-only parsing is diagnostic. All actual coverage by year/universe remains unmeasured.

Historical metadata feasibility: the earlier blanket construction cutoff implied1003/2267=44.24%. R21 supersedes that construction restriction; this is no longer a release constraint and was never measured coverage. The full2267 denominator and80% gate remain unchanged/unmeasured.


Fix round1 and R21 refreeze
--------------------------
Semantic contract: mna-native-common-raw-usd-v2-fix1-r21. Conditional cash requires the supported amount/share clauses to consume the entire consideration tail, with if/provided-that/unless/subject-to conditions rejected. Economic basis uses narrow affirmative full statements and rejects negative, alternative and contradictory statements; identities never override economic meaning.

The frozen producer and per-event exact contract now bind complete auxiliary clock original bytes, matching SGML typed pins and original receipt inventories. Missing/extra/replaced auxiliary inputs reject even when supplied with new self-matching hashes. Public admission validates exact physical Arrow/Parquet field types, decimals, UTC timestamp units/timezones and arrays, including empty products, plus whole-row/key replay. Product and manifest identities are rechecked across admission.

Runtime expectation includes the actual loaded Python reader code objects, PyArrow/Parquet/DuckDB entrypoints and compiled binaries/shared libraries, Python runtime binary and exact America/New_York TZif data. The clock reader constructs ZoneInfo from those pinned bytes; all runtime identities are checked before/after compute and public replay. Changed or abbreviated runtime expectations reject.

R21 adds a separately frozen source-only-native-dlret-r21 policy: independently qualified individual source DLRET may be constructed2023+; source rows explicitly carry dlret_policy and is_forward_label=false. The separate pre2023-native-qualified policy and complete forward-window boundary remain strict. This produces no residual-month treatment, tradable label, post2022 evaluation or statistical assessment. Old construction ceilings/receipts are superseded, not promoted toPASS. No actual new measurement occurred; actual gates remainFAIL/unmeasured.


Fixround2 remaining I2: semantic contract `mna-native-common-raw-usd-v2-fix2-r21`
requires the complete original economic block to consist of the five affirmative
statements listed in `mna_v2.native-contract.json`, in that order, with case and
whitespace normalization only. All selected quotes must also match those full
statements. Additional wrappers, quotations, negation, conditions, conflicting
prose or other unparsed context remain unsupported with NULL/reasons. This narrow
original-language contract avoids certifying an affirmative substring inside a
false statement. Real evidence outside this grammar needs a separately reviewed
reader extension; no actual economic originals have been qualified here.
