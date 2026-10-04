M&A native deal vintages v2
==========================

Initial engineering review checkpoint; actual publication and coverage remain
unmeasured. Producer/readers and full admission rules are documented in
`delisting_v2.md` and `mna_v2.native-contract.json`. Each original document is a
new vintage keyed `(accession, source_id, available_at)`. No later terms/status
are folded into an earlier announcement clock. All fields come from the pinned
original native HTML/submission, never filing-item metadata or issuer primary
flags. `available_at` is UTC native acceptance with qualified normalization,
otherwise a documented conservative retrieval floor. Staleness: document/event
vintage. PIT: strict admitted previous exchange session22:00UTC cutoff for daily
consumers, or stamped monthly session22:00UTC; no calendar-day approximation.

| Column | Type/unit | Definition |
|---|---|---|
| accession | string | Native submission identity / independently pinned original document accession |
| source_id | string | Original object identity in complete admitted inventory |
| source_sha256 | string | Original bytes SHA, independently pinned before native replay |
| available_at | UTC timestamp | Qualified original public clock or conservative retrieval floor |
| announce_date | date nullable | Literal dated native announcement; never inferred from agreement/filing date |
| termination_date | date nullable | Literal native merger termination, not another agreement's Item1.02 |
| completion_date | date nullable | Literal dated completed/consummated merger statement |
| settlement_date | date nullable | Literal dated payment/delivery, not assumed equal to completion |
| target_name | string nullable | Explicit named native common-share issuer |
| target_share_class | string nullable | Literal target common-stock class |
| acquirer_name | string nullable | Native named merger/acquirer share issuer |
| acquirer_share_class | string nullable | Literal acquirer common-stock class |
| consideration_type | string nullable | Cash/stock/mixed; unresolved alternatives stayNULL |
| cash_per_share | decimal(30,12), currency/targetshare nullable | Unconditional literal native per-share cash; par value/aggregate amounts excluded |
| cash_currency | string nullable | Explicit nativeUSD; dollar symbol alone ambiguous |
| stock_ratio | decimal(30,12), acquirershare/targetshare nullable | Unconditional literal native common-stock ratio; not DLRET |
| status | string | Completed/pending/terminated only from distinct native statements; otherwiseunknown |
| consideration_clauses | string array | All native conversion clauses, including rejected alternative/classes |
| reasons | string array | Keyed unsupported ambiguity/class/clock/completion/settlement conditions |

Actual gates: handcheck200>=95%FAIL/unmeasured; consideration>=80%FAIL/unmeasured.
Annual2020-2025 member/common/R3000 coverageFAIL/unmeasured. Current root-retained
Avon source has ADS/ordinary elections, fractional proceeds and nonpayable
preferred consideration, plus unqualified historical public clock. It is a
source-layout lead, not certified terms/terminal coverage. No new HTTP/source
acquisition occurred. NonUTF8, multiple-document submissions, alternate units,
split/FX conversions and broader sentence grammars remain explicitlyunsupported.


Fixround1 admission is defined by `mna-native-common-raw-usd-v2-fix1-r21` in
`mna_v2.native-contract.json`. Conditional consideration is unsupported; complete
auxiliary bulk/SGML originals and receipts must match the independently frozen
inventory. Loaded readers, compiled runtimes, timezone bytes and physical output
schemas are checked before and after native public replay. Under R21, the
explicit source-only native DLRET policy may produce qualified individual values
for 2023 onward; these rows are not forward labels or evaluation observations.
The historical 44.24% blanket-cutoff ceiling is superseded, without certifying
actual coverage or changing the 2267 denominator and acceptance thresholds.


Fixround2 remaining I2: semantic contract `mna-native-common-raw-usd-v2-fix2-r21`
requires the complete original economic block to consist of the five affirmative
statements listed in `mna_v2.native-contract.json`, in that order, with case and
whitespace normalization only. All selected quotes must also match those full
statements. Additional wrappers, quotations, negation, conditions, conflicting
prose or other unparsed context remain unsupported with NULL/reasons. This narrow
original-language contract avoids certifying an affirmative substring inside a
false statement. Real evidence outside this grammar needs a separately reviewed
reader extension; no actual economic originals have been qualified here.
