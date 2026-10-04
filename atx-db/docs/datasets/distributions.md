# Distributions and indicated rates

Current contract after Task13 review round1: [native admission](distributions_admission.md).
The older normalized-UTF8, self-hash-only replay contract is rejected. Source/class/clock
assertions cannot certify an indicated rate. Current producer withholds every indicated
annual rate until independent native semantic certification is implemented and measured.
Explicit source regular rate/cadence remains available as separate declared fields.

Outputs: `corporate_actions/distributions.parquet`, compatibility copy of retained
`corporate_actions/distributions_products/<id>/distributions.parquet`; independently
replayed immutable manifest and `distributions.latest.json` pointer. Keys: nonnull unique
`event_id` (accession, original-byteSHA, extraction ordinal). Duplicate catalog and output
keys reject before manifest/current publication. Public schema: `output_schema()`.

| Columns | Type | Definition/unit/source |
|---|---|---|
| event_id; accession; source_url; source_doc; evidence | string | Original retained-document identity and normalized native evidence |
| source_sha256; stored_sha256; normalized_sha256 | string | Separate original bytes, storage GZIP/plain bytes, normalized UTF8 hashes |
| security_id; cik; link_basis; link_tier | integer/integer/string/string | Catalog identifiers; linkage currently explicitly unqualified |
| declare_date; record_date; pay_date | date | Explicit clause-local dates; unknown NULL |
| kind; currency; security_class | string | Cash declaration, historical declared XBRL or spin-off; native currency/class |
| amount; declared_regular_rate | double | Native money/share; regular only when explicitly regular |
| declared_cadence; special_flag | string/nullable boolean | Explicit cadence; regular false, special true, otherwise unknown |
| indicated_annual_dividend_rate; security_class_currency_basis | double/string | Currently NULL: native class/clock certification unresolved |
| available_at; dividend_announcement_available_at | timestamp UTC | Conservative matching original receipt fetched_at/done_at floor |
| source_qualification; vintage_risk; extractor_version | string/boolean/string | Source semantics unqualified, vintage risk true, rule version |
| spin_off_ratio; distributed_security | double/string | Explicit distributed shares/parent share and distributed company |
| source_concept; period_start; period_end | string | Exact declared per-share XBRL concept/duration; never annualized |

Only explicit USD, common class and affirmative declaration clauses are parsed. Supported
from/to increases use the new value. Ambiguous multiple amounts/classes, variable policies,
no-pay statements, paid-only totals, unknown special/cadence and unsupported currency do
not invent rates. Spin-off dates remain missing unless explicitly supported. Original
HTML/GZIP bytes are verified before normalization; exact JSON facts retain source identity.

Daily visibility uses the previous admitted exchange session at22UTC (including weekends
and holidays), strictly less than cutoff. Monthly requires an admitted month-end session
and next-session proof, strictly before its22UTC. SQL registry macros now require the
admitted session calendar. Events have no expiration; no default persistent indicated-rate
carry or unsupported staleness rule is supplied.

Full eligible source-catalog hash partitions are admitted deterministically, with complete
per-partition native replay and keyed unknown/no-supported-declaration diagnostics. This
source population is not the member-line vendor cash-event denominator. The latter
requires independently admitted vendor/universe/class/ex-date evidence and remains missing.

Actual gates:0PASS/3FAIL. Declaration coverage>=90%and amount-within1%agreement>=95%
remain unmeasured. Buyback200distinct native handchecks>=95%:0checked/200required.
2020?2025member_equity, univ_all_common and univ_r3000p coverage remains unmeasured.
Task4R11 source handchecks/gates and downstream rate refresh remain open before gold/Task17.
No production build is claimed. Root feasibility verified2original objects and0HTTP;
this proves bytes only, not declarations, class mappings, acceptance or fixed cadence.
