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
