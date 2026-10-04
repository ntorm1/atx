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
