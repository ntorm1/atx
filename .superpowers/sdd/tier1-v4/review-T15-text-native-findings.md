# Task15 initial native text/TNIC review

Scope: isolated commit `9ac8af9551d1675f546ef135ba7bec4c97d48e45`, `review-T15-text-native.diff` (1667lines). Original Task15 initial review, whole-task repair **0/5**. This is an independent review, not approval of another task's fixes. Only this report is owned/written. No source jobs, HTTP, implementation changes, shared staging or unchanged suite reruns.

**Spec verdict: NEEDS CHANGES.** The new conditional source-bound publication/replay path substantially implements original text/TNIC delivery; actual landing, source plans and original95%/90% gates remain unmeasured and do not complete Task15. Required PIT/source identity, exact reproduced filter method, interruption recovery and bounded public dependency checks have implementation defects below.

**Quality verdict: NEEDS CHANGES.** Findings1–7 require correction before actual admission. None promotes source or economic gates. Frozen Task16 and Task17 ownership/counters are unaffected.

## Findings

1. **Important — `atx-db/src/atx_db/alpha_panel/tnic_native.py:63`, `:98`–99, `:125`: missing/late prior vocabulary qualification is forgotten by later calibration.** Year y's encoded vocabulary uses DF from y-1, so its histogram depends on the complete qualified original y-1 filter corpus. The loop stores `previous_clock=corpus_clock` for y, rather than retaining that filter dependency's qualification, clock and provenance. Missing2018 closure makes2019 output unqualified, but2019's histogram and own corpus clock can still qualify2020 output; a late2018 filter clock can similarly disappear from2020 availability. This violates R24's maximum clock for every vocabulary/calibration input. Carry complete filter lineage/qualification into histogram state and preserve exclusion until all required original cohorts qualify; do not infer an early clock from previous-year labels.

2. **Important — `atx-db/src/atx_db/alpha_panel/tnic_native.py:54`–63: the required original DF cohort is changed by short documents.** Native peer candidates/tokens include any nonempty business vocabulary, then compute DF across all token-bearing accessions. Original `tnic.tokenize_pass:230` excludes raw vocabularies below MIN_WORDS50 before `_load_year` and `_df_common`; thus short nonempty documents never affect that original DF denominator. Adding many one-word filings to the new corpus changes which words exceed25%, filtered vectors and subsequent SIC3 histogram thresholds relative to the required reproduction. Preserve full source/gate denominators and excluded records while reproducing the original method's eligible fitting-document universe separately. Do not count a shortened source population as coverage.

3. **Important — `atx-db/src/atx_db/alpha_panel/text_features_native.py:282`–287, `:295`–297: rename-before-activation interruption has no completing retry.** After successful immutable generation rename, a failure writing active.json preserves prior activation, which is safe. Retrying with independently pinned manifest finds the existing generation, fully validates it, then returns without activating it. The requested generation remains unavailable despite a successful publish return. There is no authenticated transition/recovery or rollback API to complete that interrupted publication. Record/recover a precise intended pointer transition, reject unrelated intervening activation, and preserve rollback history; do not activate arbitrary existing generations silently.

4. **Important — `atx-db/src/atx_db/alpha_panel/text_features_native.py:335`–350: bounded public reads do not recheck admitted source/code dependencies.** `iter_native_rows` authenticates only product and manifest bytes before/after iteration. Original source bodies/receipts/census/SIC, upstream manifests and producer/runtime code can change after `read_native_generation` without rejection during subsequent iteration. Source-plan admission is not a permanent proof that actual dependencies remain unchanged. Bind the full admission dependency inventory and actual code/source/runtime identities around public consumption, including failure/early termination behavior. Preserve full original replay; downstream wrappers cannot replace this public contract with manifest stability alone.

5. **Important — `atx-db/src/atx_db/alpha_panel/filing_text_native.py:140`–156: original Accepted-clock proof does not authenticate filing identity in the body.** The reader compares expected URL/receipt/SHA/size but accepts any index HTML with one Accepted timestamp; it never verifies native accession/issuer or archive-owner identity in the page/full-submission evidence. The existing success fixture at `tests/test_filing_text_native.py:50` intentionally contains only Accepted/time and is accepted. A wrong-page200/redirect or mislanded response with a matching retained transport receipt can assign another filing's Accepted time to the requested filing. Authenticate original page/submission accession and issuer/owner chain before clock derivation, retaining unknown/conflicting clocks. Receipt URL alone is transport evidence, not the original semantic identity. Co-registrants need explicit owner correspondence, not an unchecked caller archives_cik.

6. **Important — `atx-db/src/atx_db/alpha_panel/filing_text_native.py:172`–175: linked-filer denominator admits identity rows without native PIT/quality rules.** The native identity projection discards available_at entirely; `nt_links` unions every date-overlapping link, irrespective of link_tier, clock, conflicting links or agreement with the member CIK. A rejected or future-known identity relationship can add CIKs to the yearly linked-filer denominator. Conversely member text coverage joins only member.cik, so supported dated identity resolution for NULL member CIKs is never used. Preserve native independently qualified identity selection, original complete member cells and explicit ambiguous/unresolved outcomes; implement exact available-at selection rather than merely joining effective dates or letting a global reviewed flag qualify every row.

7. **Important — `atx-db/src/atx_db/alpha_panel/text_features_native.py:97`–99, `atx-db/src/atx_db/alpha_panel/tnic_native.py:38`, `atx-db/src/atx_db/alpha_panel/filing_text_native.py:318`: declared bounds do not cover all parsed native working sets.** The500000-word check applies only to previously retained profiles before parsing the current record. An oversized final/current issuer profile is fully tokenized/profiled and can be published without ever reaching that check. SIC originals are read wholly with no byte bound; the separate EX13 source index is read wholly without the5MB bound applied to acceptance indexes. Independently pinned plans can therefore pass declared source bounds yet invoke unbounded Python tokenization/index parsing, outside DuckDB's128MB limit. Apply limits before each affected read/profile allocation and after current profiles enter the retained set, preserving explicit source exclusions/failure reasons. No actual memory-cap failure is asserted or measured by this static finding.

## Checked invariants

Read original task brief and native continuation brief, task report, global constraints, R21, R24, and binding gap-analysis item13/section3h. Read the isolated diff in documentation/source/test passes. Named unchanged-code checks were limited to original `text_features` profiling/comparison, `tnic` vocabulary/DF/cohort/calibration and `filing_text` forms/section/EX13 helpers relevant to the new delegation; no broad code crawl or warehouse scan.

The plan is independently expected via canonical digest, full source/code/runtime actual pins and upstream manifest identities. Submissions recent/historical arrays preserve original accession records and explicit amendment/missing-clock exclusions; conflicting overlapping accessions reject. Original full quarterly SEC indexes reconcile accession/form/date and owner records, and missing quarters remain unqualified. Fitting corpus is independent of future member selection. Source review evidence is separately retained, not a fixture source-quality seal.

Native text retains exact unhashed term-frequency cosine/Jaccard, current/prior same-family annual matching with prior fiscal year plus180..550-day filing/report gaps and earlier acceptance. Missing original/empty sections/prior periods produce keyed NULLs.20FwholeItem3 fallback does not qualify Item3.D. EX13 requires original typed index link/body/receipt identities. Heuristic Fog and sentence-sequence edit proxy are explicitly named; no licensed sentiment or exact word-edit claim. Readability/sections and all declared output fields are typed even for all-NULL cases.

TNIC retains binary cosine, original vocabulary/stop/geography definitions, strict>25%DF, MIN_WORDS50,20000bins, prior-year SIC3 threshold and strict score>threshold. Same-accession co-registrants are excluded as peers. Original historical SIC receipts/observed clocks are required;2026SIC cannot qualify historical fitting. Nearest5 fallback is explicitly separate and excluded from strict95%gate. Findings1/2 concern complete dependency semantics and eligible fitting population, not replacement with official external TNIC (R24 does not require that).

Immutable publication has source/code checks before/after computation, file-backed128MB/one-thread native connection, bounded filing JSONL units, independently authenticated resume checkpoint and semantic unit reconstruction. Source/product schemas and full population including orphan keys/phase products are replayed; forged output with refreshed hash is rejected by full replay. Exclusive publication lock is retained on process kill; no automatic stale-lock deletion. Rename precedes atomic pointer update; finding3 concerns completing the interrupted transition.

Public reader recomputes all four products plus units/metadata from original bytes and compares the full reconstructed manifest, with source/code pins before/after. Products are text/features and classification_tnic/{pairs,firms,years}; keys text(CIK,accession), directed pairs(year,CIK,peerCIK), firms(year,CIK), years(year). Finding4 concerns later bounded reads, not absence of this full reader implementation.

Full yearly2019..2026filing/member reports and2020..2025three-universe reporting remain visible; original strict95%linked10K and90%member-text thresholds retained, no fallback success or parsed-row denominator substitute. Missing actual populations/landing/source review remains FAIL/unmeasured. R21 evaluation/forward-label restrictions are unchanged; this task performs source text construction without return measurements.

Author's final14focused tests across five files were acknowledged (20.41s/16.08s/13.32s/2.16s/7.21s); unchanged suites were not rerun. Earlier over60s tests are openly excluded from compliant evidence. There is no actual gate PASS from this review.

## Actual dependencies distinct from implementation findings

Root still owns verify-first complete source landing and one permitted fetch loop after shared all-attempt SEC budget reconciliation. Original retained bodies/EX13/index Accepted proofs/full submissions and quarterly census, qualified native monthly member/identity/three-universe populations, historical SIC observations, complete actual source/license/code/runtime plan and independent expected publication identities remain absent/unqualified. Actual95%strict peers for all linked10Kfilers2019+ and90%member-text cells2020+ remain unmeasured in every original year, including full2020..2025universes. Source transport-ledger audit does not admit source content or historical clocks. These are actual proof/gate dependencies; findings1–7 are code/contracts needing correction and cannot be dismissed as unavailable actual data.

## Scoped synthetic evidence

A narrow filter-lineage repro was requested and conditionally authorized by root pending Task17 serialized fixture-lane clearance. No unchanged suite is authorized. Exact final command/results will be appended after the lane is granted; no actual sources will be used.

### Completed authorized repro

Root granted the narrow repro after Task17 explicitly yielded its fixture lane. Ran committed native TNIC code unchanged; used the author's synthetic TNIC fixture only, extended with one2020cohort. File-backed DuckDB128MB/one thread/UTC; no real sources, HTTP or unchanged tests. Command from `C:/atx`, PowerShell env PYTHONPATH=`C:/atx/atx-db/src`, OPENBLAS_NUM_THREADS=`1`; here-string piped to `atx-db/.venv/Scripts/python.exe -`:

```python
import datetime as dt
import importlib.util
import tempfile
from pathlib import Path
from atx_db.alpha_panel import common as C,tnic_native as T
spec=importlib.util.spec_from_file_location('review_tnic_fixture','atx-db/tests/test_tnic_native.py')
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)
base=Path(tempfile.mkdtemp(prefix='atx-t15-review-df-lineage-'))
for case in ('missing_2018','late_2018'):
 root=base/case;root.mkdir()
 con=C.connect(memory='128MB',threads=1,temp_dir=root/'scratch',db_file='review.duckdb')
 try:
  plan=f.fixture(root,con)
  con.execute("INSERT INTO nt_text SELECT cik,replace(accession,'2019','2020'),form,filing_date+INTERVAL 1 YEAR,available_at+INTERVAL 1 YEAR,business_vocabulary,reason,business_reason FROM nt_text WHERE year(filing_date)=2019")
  con.execute("INSERT INTO nt_inventory SELECT cik,form,filing_date+INTERVAL 1 YEAR FROM nt_inventory WHERE year(filing_date)=2019")
  con.execute('INSERT INTO nt_links SELECT 2020,cik FROM nt_links WHERE y=2019')
  if case=='missing_2018':
   con.execute('UPDATE nt_source_closure SET complete=false,available_at=NULL WHERE y=2018')
  else:
   con.execute("UPDATE nt_source_closure SET available_at=TIMESTAMPTZ '2030-01-01' WHERE y=2018")
  T.build_native(con,root/'output',root,plan)
  print('CASE',case)
  print('CLOSURE',con.execute('SELECT y,complete,available_at FROM nt_source_closure WHERE y<=2020 ORDER BY y').fetchall())
  print('YEARS',con.execute('SELECT year,available_at,calibration_reason FROM nt_years WHERE year IN(2019,2020) ORDER BY year').fetchall())
  print('PAIRS',con.execute("SELECT year,count(*),min(available_at),max(available_at) FROM nt_pairs WHERE peer_basis='reproduced_tnic3_df25_sic3' GROUP BY year ORDER BY year").fetchall())
 finally:con.close()
print('RETAINED_FIXTURE',str(base))
```

Observed exit0. Initial execution returned a live session after10.005seconds; immediate completion poll returned exit0 with no further wait, approximately10seconds total (no more precise end-to-end timer recorded). Missing2018case:2018closure=False/NULL;2019year unqualified/NULL;2020year calibration_reason=NULL/available2021-01-01UTC and8strict pairs at2021-01-01UTC. Late2018case:2018closure2030-01-01UTC;2019pairs8at2030-01-01UTC;2020pairs8wronglyat2021-01-01UTC. This directly confirms finding1's qualification and clock loss. Other findings remain static; no additional repro or unchanged suite was run.

Retained synthetic location: `C:/Users/natha/AppData/Local/Temp/atx-t15-review-df-lineage-rl4uu726`. No cleanup. Fixture lane immediately returned directly to Task17 and root notified; no fixture remains live.
