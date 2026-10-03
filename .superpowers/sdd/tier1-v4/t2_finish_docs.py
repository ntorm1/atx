from pathlib import Path
import json
art=Path('../.superpowers/sdd/tier1-v4');h=json.loads((art/'t2-header-assessment.json').read_text())
extra='''
### Final source-coverage and dated-header assessment

June 2020 residual is -73 lines. A diagnostic cohort of 1,170 NYSE common lines present at both May29 and July31 month ends has 90 missing June30 panel rows; all 90 also lack June30 price-stage and original vendor bars. This is a measured vendor-source coverage hole. Neighboring future observations are diagnostic only and do not backfill features; aggregate French counts cannot prove which candidates belong to the benchmark. Evidence: t2-vendor-coverage.json and its producer.

Missing-incorporation admitted NYSE month-end cohorts contain 1,420 distinct CIKs in 2018 and 1,359 in 2019, union 1,454. All 1,454 have a current CIK file in existing submissions.zip; that archive can supply accession discovery, but its current incorporation profile is not historical evidence. Existing sec_text primary-document metadata covers 2,588 pre2020 filings for 1,277 cohort CIKs, earliest 2018-01-19. Selected parsed sections contain the literal STATE OF INCORPORATION in 15 pre2020 rows for nine CIKs. These are candidates, not verified original submission-header facts: completeness, state value, issuer identity and filing acceptance/publication clocks still require inspection. No complete-header cache or usable pre2018 header coverage has been established by this bounded assessment. No acquisition launched.

A primary EDGAR submission header containing STATE OF INCORPORATION, the issuer CIK and original acceptance timestamp is a feasible dated source after validating publication/revision semantics. One suitable pre-first-eligibility accession per cohort issuer implies a minimum 1,454 candidate header requests before cache reuse; state changes require more, so this is a lower bound rather than a fetch plan. Inspect the nine existing text candidates first; use cached archive accession discovery to locate earlier filings and bounded missing-header requests. Never assign a later filing state to earlier sessions. SEC header backfill should be separately authorized/reviewed and preserve strict cutoff and unresolved unknowns.

For 2021-22 excess, missing incorporation is near zero and the same FINRA classifier's zero suspects proves only internal consistency. Independent candidates are dated original SEC cover/security descriptions and prospectuses for NYSE listed classes, contemporaneous 814-N/814-NW and fund status, completed-SPAC versus retained SIC6770 chronology, foreign domicile/FPI filing history, duplicate share classes, and dated exchange/link validity. French uses its documented count unit; company counts remain a diagnostic alternative. The aggregate benchmark has no constituent identities, so these hypotheses are not established false inclusions. Preserve French FAIL pending controller ruling and independently measured remediation.

Guard commands (all from atx-db with PYTHONPATH=src): `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb CAP --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/SCRIPT`. Vendor coverage: CAP=0.8, SCRIPT=t2_vendor_coverage.py, receipt 20261003T163345-14836, peak 0.0738 GiB, 2 seconds. Header cohort/schema/hash: CAP=0.9, SCRIPT=t2_header_assessment.py, receipt 20261003T163832-2172, peak 0.8454 GiB, 13 seconds. Header text-cache: CAP=0.6, SCRIPT=t2_header_cache_check.py, receipt 20261003T163926-16608, peak 0.2476 GiB, 14 seconds. All completed with exit 0 and no cap hit.

All four published manifest LF-normalized code fingerprints match current modules (individual comparisons in t2-header-assessment.json). Owned directory disk bytes: universe 196,586,801; indexes 3,187,407; market 1,761,260,397 (includes inherited non-return market files). This is directory footprint, not new raw bytes. No foreign output modified.
'''
for name in ('task-2-report.md','task-2x-report.md'):
    p=art/name;s=p.read_text(encoding='utf-8');s=s.split('\n### Final source-coverage and dated-header assessment')[0]
    if name=='task-2-report.md' and 'Current fix-round status:' not in s:
        pos=s.find('\n');s=s[:pos+1]+'\nCurrent fix-round status: DONE_WITH_CONCERNS; 4 PASS / 1 FAIL, French 27/60 months outside unchanged +/-5%. The original report below is historical; current measurements are in Fix round 1.\n'+s[pos+1:]
    p.write_text(s.rstrip()+'\n'+extra.rstrip()+'\n',encoding='utf-8')
for name in ('universe','market_index','indexes','share_exchange_history'):
    p=Path('docs/datasets')/(name+'.md');s=p.read_text(encoding='utf-8')
    if name=='universe' and 'June30 source gap' not in s:
        s+='\nJune30 source gap: 90 NYSE lines present on neighboring May/July2020 common month ends have no June30 panel/price/vendor bar. This is diagnostic source continuity evidence, not benchmark identity proof or permission to backfill. Historical SEC header remediation candidates and unresolved 2021-22 classification hypotheses are documented in task-2-report.md Fix round 1; French FAIL remains.\n'
    p.write_text(s.rstrip()+'\n',encoding='utf-8')
print('final evidence documentation appended; no products rebuilt')
