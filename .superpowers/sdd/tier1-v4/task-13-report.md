# Task13 distributions and buybacks

Status: BLOCKED on native-source catalog/class/clock certification and actual gates.
Actual model: GPT6.1 Sol. Original binding item11thresholds are unchanged. Code/tests/docs
are complete for the bounded acquisition-free adapter, but production source coverage is
not established. Root exclusively coordinates real-data jobs; no real source scan, hash
audit, build, network or HTTP was executed by this lane.

## What was built

`alpha_panel/distributions.py`: source-native USD/common-class cash declarations with
explicit declare/record/pay dates, tri-state special flag, explicit declared regular
rate/cadence, class/currency-qualified indicated rate, duration per-share declared XBRL
facts kept separate from rates, and explicitly stated spin-off share ratios. Publication
retains source/accession/evidence/hash, UTC availability, input manifest hashes, and
immutable code/input-addressed products plus a current compatibility copy. Source hashes
are checked before/after replay; native docs≤2MiB, output≤100,000events per catalog.
Missing class mapping/currency/cadence/special determination never invents a rate.

`events_buyback.py`: v2 schema retains native source hash, handles eligible retained10-Q
documents, and extracts a deliberately narrow PartIIItem2unscaled USD/share table as
execution events distinct from authorizations. Execution events cannot be first
announcements. Catalog/parser/fundamental/identity inputs are pinned before/after, and
publication retains immutable products; DuckDB128MB,1thread. New registry stubs/docs
describe exact native replay contracts and outstanding source limitations. No shared,
foreign or downstream ratio module was edited.

Actual new production rows/bytes/date span: **not measured** (no production build).
Fixtures: distribution1event plus class-mismatch vintage; buyback2events(new and
execution), source clocks2021. Temporary artifacts are pytest-owned tmp_path files,
not production sources or acceptance evidence.

## Gates (actual source evidence only)

| Criterion | Status | Measured | Exact measurement command |
|---|---|---|---|
| Declare date for≥90%member-line cash dividends2020+ | FAIL | unmeasured; full denominator unavailable | NOT RUN: root-approved native/catalog+vendor+universe audit required |
| Source amount within1%vendor-implied cash on≥95% | FAIL | unmeasured; independent event/class/ex-date matches unavailable | NOT RUN: same full-denominator source audit required |
| Buyback200distinct native-event handchecks≥95% | FAIL | 0 independently checked events;200required | NOT RUN: root-approved immutable source-keyed200event handcheck required |

Gate count0PASS/3FAIL. Manifest gate statuses explicitly fail closed. No code-only test
is promoted to actual acceptance. The fixture arithmetic tests exact≥threshold
boundaries, missing/unmatched cash denominator retention, fewer than200checks and
duplicate checks. Actual handchecks must verify fields against native documents rather
than assigning Boolean matches from parser output. Runtime input catalog still requires
independent semantic certification; hashing assertions does not certify class/clock truth.

2020–2025coverage by member_equity/univ_all_common/univ_r3000p: unmeasured for every
year/universe. No return statistics were computed, including2023+holdout.

## Tests

Working directory`C:/atx/atx-db`, env`PYTHONPATH=C:/atx/atx-db/src`,
`OPENBLAS_NUM_THREADS=1`:

```powershell
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_alpha_panel_distributions.py tests/test_alpha_panel_events_buyback.py -q
```

Final result: **19passed in1.93s**, offline bounded fixtures. Distribution tests verify
explicit date/rate provenance, unknown/special status, currency/class rejection, paid
versus indicated distinction, no future declaration, exact daily/monthly clock equality,
XBRL duration preservation, explicit spin-off ratio, mutated source rejection, input
manifest pins, immutable replay/vintages, class mismatch nulls and gates. Buyback
publication integration tests exercise bounded DuckDB, execution distinction, explicitly
future-clocked XBRL corroboration, pinned product manifests and immutable overwrite
rejection. Earlier intermediate test failed due misplaced test lines; corrected and
rerun, no production effect. Final covering run supersedes intermediate results.

## Source preflight/build handoff

Root received this retained metadata-only schema sample command (not run by this lane):

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.3 --wait-minutes 10 -- .venv/Scripts/python.exe -c "import json; from atx_db.alpha_panel import events_sources as E; from itertools import islice; print(json.dumps(list(islice(E.iter_catalog(lambda r: bool(r.get('form')) and r['form'].startswith(('8-K','10-Q'))),3)),default=str))"
```

The iterator stops at3eligible records. No source acquisition plan is authorized. Concrete
production distribution catalog must identify retained original bytes, original clock
provenance, explicit security-class/currency linkage and native event evidence. Replay
command after root reviews those actual inputs:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.3 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.distributions --documents <reviewed-retained-native-catalog.jsonl> --input-manifest <pinned-class-link-manifest.json> --input-manifest <pinned-source-clock-manifest.json>
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.3 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.events_buyback --phase all
```

Placeholders are unresolved inputs, not runnable acceptance commands or evidence.
Production builds alone do not measure original gates. Distribution source-native
matching/denominator audit and200buybackchecks are still required. Task4must refresh
its actual`divyield`gate and native handchecks before gold/Task17; no ratio gate has been
refreshed or closed by this lane. Real products are not claimed published.

Sources landed: none; URLs/bytes/span/cadence/terms/rate limits not acquired or measured.
Disk used: source/test/docs/report only; production0newlandings. No obsolete production
cache was deleted. Any future SEC acquisition must use`sec_http`, existing UA, shared
≤5/sbudget and receipts; other hosts≤1/s, after root source-budget review.

## Files and self-review

Owned changes: `atx-db/src/atx_db/alpha_panel/{distributions,events_buyback}.py`,
`atx-db/tests/test_alpha_panel_distributions.py`,
`atx-db/docs/datasets/{distributions,buyback}.{md,registry.json}`, this report.
Baseline owned source paths were clean. `corporate_actions.py`was read but not edited;
vendor factors remain comparison-only. No branches/worktrees were changed or agents
spawned. Commit uses explicit owned pathspecs and`git add -f`for this report; legacy
required Claude trailer is attribution-policy text, actual model stated above.

Open issues/deviations: real source acquisition/coverage/native clock-class-certification
and handchecks unavailable; adapter supports normalized UTF8 or exact JSONfacts, not a
complete original HTML/XBRL fact harvester; PartIItable caption variations/scales and
non-USDcash declarations remain missing; spin-off dates remain NULL without an explicit
supported source extraction; indicated rate carry/staleness policy unresolved; historical
events issuer-primary linkage is not dividend-class identity. Existing events linkage
must undergo source/PIT certification in actual build review. These limitations are
reported as missing evidence and do not relax gates. No broad source scan was run to
hide them. Downstream Task4R11 remains open until source-certified publication exists.

## Fix round R1 — independent review of539ee6ff

Review:`review-T13-native-findings.md`, five Important engineering findings I1–I5.
Status remains BLOCKED on actual source semantics/gates; this is an engineering
correction, not task-completion or actual-gate evidence. Revised native contract
supersedes every initial normalized-UTF8/hash-receipt build command above. Native
source prerequisites cannot be counted as the completed original task.

| Finding | Correction | Covering fixture proof |
|---|---|---|
| I1 old/new amount | Explicit from/to increase uses the new amount; ambiguous multiple amounts/classes refuse; variable/no-pay policies refuse | test_increase_new_amount_and_ambiguous_classes; test_no_pay_policy_and_holiday_cutoff |
| I2 calendar day | Prior admitted exchange session, strict22UTC; monthend requires next-session proof; registry SQL updated | test_strict_daily_monthly_clock; weekend/MLKholiday/equality tests |
| I3 PartII scaled values | Reject scales anywhere in section or explicit surrounding shares/amounts scale scope; keyed unsupported reasons retained | test_scaled_surrounding_part_ii_rejected; native unscaled execution integration |
| I4 compute/native provenance | Mandatory externally expected admission digest; complete reader/parser identities before computation; raw stored/original/normalization hashes separate; original receipt identity/clockfloor verification; complete eligible source-catalog hash partition; public full-row native/schema/key/join replay before publication and read-only CLI replay | gzip admission/expected denominator/stale code tests; fabricated parse receipt refused; mutated distribution amount and buyback join values reject public replay |
| I5 duplicate keys | Native and parent catalog nonnull unique keys reject; independently enforce output keys before exposing manifest/current product | duplicate native/parent-key fixture; public validators enforce keys |

New private module:`distributions_native.py`; new public contract:
`docs/datasets/distributions_admission.md`. Existing distribution/buyback docs and
registry stubs now describe admitted calendar, exact keys/schemas and current replay.
Distribution schema/rule bumpedv2. Legacy unadmitted phase_parse explicitly refuses;
CLI uses parse_native and cannot publish a manufactured hash-only receipt.

Externally expected admission pins catalog, original parent source-catalog, calendar,
class, clock, fundamental/identity manifests and required descendant files, exact stage
readers and imported lxml/pyarrow/DuckDBreader entrypoints/binaries. Raw source GZIP
is bounded decodedlimit+1; every original source receipt line/ledger and body digest
must match expected identities before normalization. Code/native/input identities are
rechecked after computation and independent public replay. Source metadata hashes
prove identity only. They never turn arbitrary class/clock assertions into truth:
all current indicated annual rates remainNULL; declared regular rates/cadence remain
separate source-supported fields. Availability is matching original-receipt
fetched_at/done_at conservativefloor; original acceptance remains unqualified.
Product statuscomplete means complete admitted extraction/replay, with actual task
gateFAIL and explicit source_semanticsUNQUALIFIED. Independently admitted parent
eligible metadata partitions preserve every expected key and unknown/no-supported-
declaration diagnostics. They are not a vendor-cash full denominator substitute.

Bounds now≤100native docs/partition,≤2MiBstored/decoded per doc,≤8MiBdecoded+normalized
total, original receipt ledger≤32MiB/line≤64KiB, original metadata≤8MiB/100,000rows.
Public buyback replay and publication use128MBDuckDB/1thread. No original sources,
actual hashes, network, HTTP or live data jobs were run by this lane during R1.

Only generated root evidence was read:task13-catalog-preflight-v1.json and
task13-original-feasibility-v1.json. Root reports schema preflight2s/123,408,384bytes,
source feasibility4s/141,127,680bytes,0HTTP. Original catalog51384rows/3173718bytes,
SHAa76d19d7458481fcd67de4f7397617234f34b98843b2449c291628564690bf64;
23958eligible2020+8K(/A)metadata rows, no10Qrows in this catalog (not absenceelsewhere).
Two original GZIPobjects totalstored+decoded305336bytes matched receipts/catalog:
CIK16160/accession0000016160-20-000007 originalSHAbca214bc5fc720a716e5b8131b9f41acb166598ab3b3203cd2c7f75a51ebd463,
storedSHAa58331ebb7b1a0183f2b043f5e131530806b7fed4cc75dc08de58208b0ed7272,
normalizedSHAaea4cd33e4bbddc134005809f66f52e547bc7bd21891c5c427caa7e1ddb038d9;
CIK8868/accession0000950157-20-000022 originalSHA2db7aba232e1ed2e5d2c5aa16e3a8a3fed3d89889f80a79cdc5fba7f64fbce4d,
storedSHAff608acc47a48c887caaf48c6d0696aeb94a75613d24ef5f9031faaa002495e5,
normalizedSHA4afbeddcb2f9127705796218a46cb76f5f0f6769c050225e93fe5d63b23d584a.
These are root-reported original-byte proofs, not lane reaudits or native semantic
handchecks. Cal-Maine explicitly variable-income policy and willNOTpayQ22020; no
affirmative/fixed rate inferred. Avon source is a merger document without distribution
keywords. Both remain class/acceptance/cadenceUNQUALIFIED. Neither closes a gate.

R1 covering command, same required env and working directory:

```powershell
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_alpha_panel_distributions.py -q
```

Covering suite passed18tests in9.44s before schema-version metadata-only update;
final result recorded below after current code validation. No broad unchanged-test
rerun. An intermediate scale-scope test falsely rejected a separate authorization
million amount; corrected scope to explicit shares/amounts **in** scaled units and
retested. Root's actual-source results were not substituted for fixture outputs.

Actual gates unchanged:0PASS/3FAIL; declaration coverage/vendorcashagreement
unmeasured; independent buybackchecks0/200. Exact remaining external work: qualify
native publication/class/currency/calendar and rate-carry source chains; admit complete
source partitions with original source receipts; independently admit and measure every
member-line2020+cash-event/vendor/universe/ex-date denominator (retain unresolveds);
200distinct native buyback expected-field checks with≥95%agreement; peryear2020–2025
coverage in all three universes; Task4divyieldactual gate/source-handcheck refresh
before gold/Task17. No fabricated Boolean matches or metadata-onlyPASS were published.

Final R1 covering result:18passed in12.89s before explicitly adding the loaded
PyArrowlib/parquet and DuckDBcompiled reader binary identities. Final post-reader-pin
result appended below. `git diff --check` passed for all owned paths. Only report/docs
changes follow the final covering tests; commit stages this report using`git add -f`.

Post-reader-pin final covering result:**18passed in19.85s**, command above,
offline fixtures only. Loaded compiled PyArrowlib/parquet and DuckDBbinary identities
are now included alongside lxml and stage readers before/after computation. All
five finding-specific tests and the full independent replay rejection integration
are in this single covering file; no unchanged test suite was rerun.

## Fix round R2 — review8f80ae3f

Exact findings:`review-T13-native-r1-findings.md`. I2–I5were addressed byR1;
remaining I1and newI6corrected within owned paths. Two actual fix rounds total.
Full source task remains BLOCKED, with original thresholds/denominators unchanged.

I1: selected monetary value must have an immediately attached per-share unit.
Explicit from/to increase permits the first value's omitted unit only in that narrow
from/to clause when the selected new value carries a local per-share unit. Aggregate
million/thousand/comma-scaled totals and unresolved per-share statements reject before
amount/declared_regular_rate emission. A per-share phrase elsewhere cannot qualifyUSD600
million. No clearing only the final indicated rate to hide a wrong raw amount.

I6: independently admitted source partitions are not complete prior issuer authorization
history. Producer and public native replay now retain first_announcement=NULL for every
authorization with first_announcement_basis=unknown_incomplete_global_history. Executions
remainfalse with not_applicable_execution. Neither isolated later filings nor a partition
containing an earlier matching authorization can certify global180dayhistory. No implicit
history union from only successful partitions is used. Independent complete global prior-
history admission remains required before introducing nonnull authorization classification.
Buyback schema/extractor bumpedv3, docs/registry/module contract updated consistently.

Covering offline fixtures: strict local unit/from-to arithmetic and ambiguous/unresolved
aggregate cases; native publication with a later authorization alone and with an earlier
matching authorization one month before, both retainNULL; public full native/join replay
agrees with unknown classification and still rejects tampered joins. DuckDB128MB/1thread.

Exact covering command fromC:/atx/atx-db, PYTHONPATH=C:/atx/atx-db/src,
OPENBLAS_NUM_THREADS=1:

```powershell
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_alpha_panel_distributions.py -k 'increase_new_amount or aggregate_or_unresolved or buyback_pinned_publish' -q
```

Result6passed,16deselected in19.83s before correcting the module's stale contract text;
post-contract/code-identity result recorded below. Only covering cases ran; no broad
unchanged tests. No actual source/data/hash/HTTP jobs ran; root's live exclusive slot was
not used or bypassed. Generated root feasibility evidence remains original-byte/layout
proof only. No source semantics, actual rates or Boolean handcheck matches were invented.
Actual gates unchanged0PASS/3FAIL, declaration/vendorcashcoverage unmeasured, buyback
independentchecks0/200; Task4R11and full source/coverage/publication obligations remain open.

FinalR2covering result:**6passed,16deselected in18.03s** after current module contract
and reader-code identities. `git diff --check` passes for owned paths. R2commits only
distribution/buyback source, directly covering test, corresponding docs/registry and
this forced-added report; no shared/foreign/frozen producers were edited.

## Full-task source dependency continuation after scopedR2approval

R2engineering checkpoint cb318dd9was independently Approved in
review-T13-native-r2-findings.md. This continuation is not a new repair round;
total repairs remain2/5. Original full Task13source obligations/gates remain incomplete.

Root authorized new exact-path metadata helper
`.superpowers/sdd/tier1-v4/t13-native-chain-preflight-v1.py`. Lane created it but did
not run it. It requires external ATX_GUARD_JOB, refuses output overwrite, uses no
glob/recursive source discovery, caps12files and8MiBfooter/file, reads final8bytes
then Parquet metadata only, and outputs explicit missing/schema/rowcount/groups/stat
bytes/native-reader paths. Zero payload/value/return rows, full-source hashes andHTTP.
Root inspected/ran under0.3GiBguard:1.0s/45,813,760peakbytes,0HTTP, all12paths exist;
only generated task13-native-chain-preflight-v1.json was read by this lane.

Root-reported footer rows:SECfilings18,647,938(544,988,422bytes/72groups),
cover992,162(52,120,466bytes/33groups),vendor_events262,560(4,596,662bytes/2groups),
calendar3642;2019q4/2020q1SUB12664/13561,TXT_DEI302483/343432,
NUM_DEI7536/11221,DIM115800/195139. Metadata is layout evidence only; no class,
currency, original acceptance, cadence, carry, prior history or gate was qualified.

New owned private module `distributions_qualification.py` implements concrete native
semantic readers/conditional functions, with positive and refusal offline fixtures:
original index Accepted or full SEC header clock checks accession and converts
NewYork/DST toUTC, rejecting ambiguous/nonexistent times; original inline-DEI title and
symbol must share a real context whose identifier matches unique native issuerCIK,
excluding co-registrant/ADR/preferred contexts; replay exact affirmative per-share
native regular USD declaration/cadence; require one dated symbol/CIK/security mapping
and no future class/link evidence; then produce an event-level native fixed-rate
annualization. Positive fixture0.60quarterly→2.4USD/share/year demonstrates a real
conditional path rather than permanentlyNULLstub. Quote-currency parity remains false
and separate from native dividend currency. Missing/unknown/variable/special inputs
cannot qualify. Explicit source `effective through` validity supports bounded carry;
missing validity cannot invent persistence/expiry. Parsed no-pay/cancellation/variable
tombstones and availability filtering prevent future statements rewriting earlier state.

These standalone semantic functions are not yet wired into production admission.
Native original declaration+index/header+DEI objects, exact independently admitted line
evidence and complete event inventory through cutoff must be supplied and validated by
new ancillary admission adapters before production fallbackNULLcan be replaced. In
particular, calling a list-based state helper does not certify inventory completeness.
Transformed notesSUB/DEI/cover rows alone do not establish original ZIPmember/source
proof; no arbitrary Boolean/metadata/hash assertion was used to qualify actual sources.
Exact source-dependency plan with local native paths/columns was sent to root before
implementation; next root audit must establish a few retained native semantic chains
before concrete ancillary adapters are admitted. Full180daypriorhistory and every
member-line/vendor/universe cash denominator remain required, not partial successes.

Covering command fromC:/atx/atx-db, requiredPYTHONPATH/OPENBLAS_NUM_THREADS=1:

```powershell
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q
```

Result:**5passed in0.29s**, offline original-format fixtures only. No unchanged suites
or actual source/data/hash/HTTP jobs ran. No shared/foreign/Task12/Task16producer edits.
Actual gates remain0PASS/3FAIL; independently checked buybacks0/200; all actual yearly
coverage and full source-publication/class/currency/history/gate measurements remain open.


## Production ancillary continuation (repair rounds remain2/5)

Supersedes checkpoint62896107 standalone limitation. Production and public full replay
independently admit original declaration/class/clock bodies plus receipts. v2 adds expected
price-symbol history and expected identity descendants. Parsed Accepted/SGML, same-context
inline-DEI, exact per-share declaration/cadence/currency and unique available dated line
conditionally produce event rates. Missing records NULL/reasons; claimed missing/tampered
objects reject. Complete reader/code pins include both conditional modules. Schema v3
exposes explicit quote currency false, source validity, carry status and class context.
Full persistent global history/cancellation inventory remains open.

New t13-native-object-preflight-v1.py created NOT RUN by agent: external guard/no-overwrite,
exact notes ZIP paths, earliest8 original docs, receipt/catalog before-after pins,2MiB
stored/decoded per object,24MiB aggregate,16contexts/128KiB output. Root reports2.0s/
147480576peak bytes/0HTTP:8originals receipt/SHA matched,2915510stored+decoded bytes;
2inlineDEI/2declaration leads,0Accepted/SGML. Exact2019q4/2020q1 raw notes ZIPs missing,
not an assertion about every alternative root. Leads do not qualify class/currency/clock.

Covering command fromC:/atx/atx-db with PYTHONPATH=C:/atx/atx-db/src,OPENBLAS_NUM_THREADS=1:
`.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q`
6passed7.26s: production positive2.4USD/share/year, missing-chain NULL, independently
admitted ADS negative NULL, public full replay and ancillary tamper rejection. Offline only;
no actual data/source/hash/HTTP jobs or unchanged broad suites. Actual gates0PASS/3FAIL,
buyback handchecks0/200. Full vendor cash denominator, coverage/amount, native acceptance,
quote currency and complete prior-history proofs remain unmeasured. Task13/Task4R11
incomplete. Actual modelGPT6.1Sol. Commit requires explicit owned paths and git add -f
for ignored report/helper; all foreign changes preserved.


## Repair round3/5: native qualification boundary

Scoped baseline cda1e08b; original findings read from review-T13-native-qualified-findings.md.
I1: fixed regular status/cadence now attach to the cash-dividend phrase. Unrelated annual
meeting cadence is ignored; conflicting/unsupported cadence and contingent conditions
remain NULL. Conditional, earnings-target and conflicting cases exercise production and
independent public replay; forged positive rates are rejected by full row replay.
I2: accepted_clock requires an authoritative original accession/issuer and exact SEC
member URI. Index secNum/companyInfo/Accepted/tableFile and original SGML header plus
unique FILENAME/TEXT membership are supported narrowly. Related-accession prose, wrong
issuer/header, repeated clocks, swapped URI or unequal member body remain unqualified.
Ancillary original clock receipt URI must identify the actual accession index/full submission.
Positive fixtures now contain real-format accession/header/issuer/filing-member identities;
fixture metadata never substitutes for that native authority.
I3: declared date is rechecked against reconstructed native acceptance and final availability;
unknown/future declaration chronology refuses qualification. The production future-date
fixture has valid earlier price/link rows so missing mapping cannot mask the boundary.
I4: explicit actual tzdata NewYork TZif replaces ambient TZPATH/cache. Independently expected
complete identities include TZif, compiled _zoneinfo, zoneinfo/_common, datetime/_strptime,
Python executable; loaded clock/path/class/loader entrypoints are also verified. Before/after
pins apply to producer and public replay. Changed TZif or entrypoint rejects unchanged
admission. Ambient TZPATH changes produce the same qualified UTC clock. Rule refrozen to
native-distributions-v4, unchanged physical schema v3; independently refresh all old code pins.

Covering offline commands, fromC:/atx/atx-db, with PYTHONPATH=C:/atx/atx-db/src and
OPENBLAS_NUM_THREADS=1; DuckDB128MB/1thread:

```text
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q -k 'not production'
7passed12deselected0.59s
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q -k 'production and (positive or annual_meeting or conditional or earnings)'
4passed15deselected22.87s
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q -k 'production and (conflicting_cadence or future_date or wrong_header or wrong_issuer)'
4passed15deselected25.85s
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q -k 'production and (multiple_clocks or swapped_member or tzif_drift or normalizer_drift)'
4passed15deselected16.74s
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_alpha_panel_distributions.py -q -k 'native_declaration_dates_rate_and_provenance or increase_new_amount_and_ambiguous_classes or original_gzip_admission_duplicates_and_stale_code'
3passed19deselected2.78s
```

Final commands cover19qualification tests plus3affected extraction/admission tests. An
initial combined qualification invocation was interrupted before completion to keep the
fixture jobs bounded; it is not counted as PASS. Earlier intermediate commands likewise
are not substitutes for these final-source results. No source/data/hash/HTTP audit or live
job ran; runtime-package identities and temporary synthetic originals only. No shared,
foreign, Task12 or frozen Task16 edits. Explicit owned path commit; ignored report git add -f.

Actual Task13 remains incomplete:0PASS/3FAIL-unmeasured, native buyback handchecks0/200.
Full global prior-history/cancellation/carry admission, historical quote currency, complete
vendor member-line cash denominator, declaration>=90%, amount within1%on>=95%, and200native
buybackchecks>=95% remain required. No thresholds/denominators changed. This is repair
round3/5, not a cap reset. Actual modelGPT6.1Sol; legacy co-author trailer preserved.

## Fix round4/5: independently reviewed remaining I1 and I4

Fresh implementer after checkpoint52bb3669; binding remaining findings from
review-T13-native-qualified-r3-findings.md. I2/I3 were not reopened. No actual source,
data, HTTP, metadata or economic measurement jobs; only offline synthetic originals.
Graph discovery tools unavailable, so scoped source reads used the documented fallback.

I1: qualification now fullmatches complete original text against a narrow supported
affirmative unconditional declaration grammar, rather than adding condition keywords.
Supported board declarations include declaration date, explicit regular dividend-local
cadence, USD per-share amount/common class, optional explicit effective-through date,
and the supported annual-meeting preface. Other original prose/context remains NULL
with a reason, including only-when, as-long-as, false, quoted/disputed, separate denial
and unknown-condition variants. Production and public full replay independently use
the original admitted text; each negative also rejects a forged2.4positive output.
Supported original2.4and unrelated annual-meeting positives remain qualified.

I4: load_data and its TZifHeader.from_file dependency now freeze actual loaded code
objects as well as function pointers. Independent code_sha256 includes stable canonical
semantic loaded-code hashes under runtime:zoneinfo.load_data and
runtime:zoneinfo.TZifHeader.from_file. Canonical fields include bytecode, recursive
constants/code, names/locals/freevars/cellvars, argument counts, stack/flags and exception
tables. Checks occur before/after native normalization and producer/public full replay;
producer's former disk-only final check now uses the same complete recheck. Same-function
UTC-loader code mutation under unchanged on-disk/TZif/source/admission pins rejects both
producer and public replay. Injected mutation after replay also rejects both consumers.
The original header loader same-function code drift has a direct rejection regression.

Refrozen contract: native-distributions-v5, physical schema still
atx.alpha-panel.distributions/v3. Existing independent admissions must be independently
refrozen with the new full on-disk/loaded identities; no actual admission was rewritten.
Full-text grammar intentionally leaves broader real filing layouts unsupported/NULL.
No complete source history/carry or quote-currency claim follows from event qualification.

Final covering commands from C:/atx/atx-db, environment
PYTHONPATH=C:/atx/atx-db/src, OPENBLAS_NUM_THREADS=1; offline fixtures,
DuckDB128MB/1thread, pytest-n0/no extra workers, each invocation below60seconds:

```text
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q -k 'production and (only_when or as_long_as or false_declaration or quoted_declaration)'
4 passed, 24 deselected in 47.63s
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q -k 'production and (separate_denial or unknown_condition or same_function_code_drift or same_function_public_code_drift)'
4 passed, 27 deselected in 14.25s
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q -k 'not production or producer_after_replay_code_drift or public_after_replay_code_drift or positive or annual_meeting'
13 passed, 18 deselected in 21.33s
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_distributions_qualification.py -q -k 'production and (conditional or earnings or conflicting_cadence or future_date)'
4 passed, 27 deselected in 40.41s
.venv/Scripts/python.exe -m pytest -n0 -o addopts='' tests/test_alpha_panel_distributions.py -q -k 'native_declaration_dates_rate_and_provenance or increase_new_amount_and_ambiguous_classes or original_gzip_admission_duplicates_and_stale_code'
3 passed, 19 deselected in 4.55s
git diff --check -- atx-db/src/atx_db/alpha_panel/distributions.py atx-db/src/atx_db/alpha_panel/distributions_native.py atx-db/src/atx_db/alpha_panel/distributions_qualification.py atx-db/tests/test_distributions_qualification.py atx-db/docs/datasets/distributions.md
exit0, no output
```

25 targeted qualification cases plus3 affected extraction/admission tests passed.
First four-case command preceded test-only addition of after-replay/header cases;
production code unchanged thereafter. Initial same command failed4cases in5.61s
because producer treated runtime identity keys as paths; corrected complete recheck.
Intermediate rerun failed1case/3passed in17.64s because marshal serialization changed
with interning after execution; canonical code fields corrected that instability.
Neither failed invocation is counted as PASS. Final commands above supersede them.

Files changed: distributions.py, distributions_native.py,
distributions_qualification.py, test_distributions_qualification.py,
docs/datasets/distributions.md, this report. Sources landed0; actual publication rows,
bytes/date span and disk growth not measured/no new real dataset. No shared/foreign,
Task12, Task16 or root ledger edits. Self-review found unsupported layouts preserved
NULL and before/after runtime checks retained; independent reviewer still required.

| Original criterion | Status | Measured | Command/evidence |
| --- | --- | --- | --- |
| Declare date>=90% member-line cash dividends2020+ | FAIL | Unmeasured; full vendor/member-line denominator absent | No actual command authorized/run this round; unchanged prior gate |
| Amounts within1% on>=95% | FAIL | Unmeasured; complete native/vendor comparison absent | No actual command authorized/run this round; unchanged prior gate |
| Buyback200native handchecks>=95% | FAIL | 0/200 independently checked | No actual command authorized/run this round; unchanged prior gate |

Actual gates remain0PASS/3FAIL; no synthetic counts replace original thresholds or
denominators. Full global prior-history/cancellation/carry admission, historical quote
currency, actual native acceptance proofs and Task4R11/downstream gate refresh remain
open. Task13 remains incomplete despite scoped engineering fixes. Round4/5 cumulative;
explicit owned-path commit with required legacy co-author trailer.
