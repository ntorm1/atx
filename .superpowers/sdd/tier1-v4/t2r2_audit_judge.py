"""Persist manual original-disclosure judgments for the fixed diagnostic sample."""
from pathlib import Path
from atx_db.alpha_panel import common as C
art=Path('../.superpowers/sdd/tier1-v4');path=art/'t2r2-independent-audit-evidence.json';e=C.read_json(path)
notes={
'FBC':'NYSE common FBC, Michigan bank holding issuer; bank operations are distinct from a fund security.',
'BVH':'NYSE Class A common BVH, Florida issuer; disclosed vacation-ownership subsidiary and operating holding structure.',
'MN':'NYSE Class A common MN, Delaware issuer. Advisers/fund subsidiary regulation in the annual is not an issuer fund-status assertion.',
'BKI':'NYSE common BKI, Delaware software/data segments; REIT customers and blank-check preferred authorization are not issuer class/status.',
'COOK':'NYSE common COOK and Delaware issuer cover; dated operating quarterly disclosure, no demonstrated wrong class/status in reviewed passages.',
'HLLY':'Latest pre-sample quarterly identifies Holley, Delaware, HLLY common; earlier Empower Cayman shell annual does not establish November status or justify backdating the redomicile.',
'ATIP':'Dated ATI quarterly/annual describes completed combination and ATIP NYSE Class A common, Delaware; predecessor FAII shell classification cannot be assigned forward after completion.',
'CWEN':'NYSE CWEN Class C common and CWEN.A Class A; Delaware energy operating issuer. Its fund-manager owner does not make the issuer a fund.',
'JW.A':'NYSE JW.A Class A common, New York issuer; separately disclosed JW.B common. Multiple share lines retained under benchmark documented line-count unit.',
'BIO.B':'NYSE BIO/BIO.B common classes, Delaware issuer; possible investment-company risk is conditional, not an assertion of current registered fund status.',
'WE':'NYSE WE Class A common, Delaware operating WeWork disclosure after combination; retained SIC6770 alone does not establish a blank-check issuer.',
'HPP':'Direct matching issuer assertion: Hudson Pacific Properties Inc. is a REIT. Wrong admission under original classifier; dated source remediation uses availability, not historical backdating.',
'KEY':'NYSE KEY common shares, Ohio issuer; separately listed depositary/preferred lines do not change the common KEY line.',
'AHH':'Matching Company definition and direct current REIT/tax-election assertions. Wrong admission under original classifier; dated source remediation applies to the complete candidate cohort.',
'TGI':'NYSE TGI common, Delaware issuer; separately listed purchase rights are not the sampled common line.',
'DS':'NYSE DS common, Maryland operating disclosure; broad real-estate/financial cohort inclusion alone is not REIT status. No current unambiguous REIT assertion in reviewed original.',
'ARCH':'NYSE ARCH common, Delaware coal operating issuer.',
'USER':'NYSE USER common, Delaware original registrant; preferred blank-check issuance authority is not issuer SPAC status.',
'SG':'NYSE SG Class A common and Delaware original header/cover; originally missing incorporation is independently identified without snapshot backdating.',
'BARK':'NYSE BARK common, Delaware post-combination quarterly; separately listed warrants are distinct from the sampled common line.',
'OWLT':'NYSE OWLT common, Delaware issuer; original annual explicitly distinguishes the former pre-merger SPAC and separate warrants.',
'STZ':'NYSE STZ Class A and STZ.B Class B common, Delaware issuer; dual-class count remains diagnostic, not a benchmark-unit amendment.',
'TAP.A':'NYSE TAP.A Class A common, Delaware issuer; Canadian office/address does not determine incorporation. TAP Class B is separately disclosed.',
'BH.A':'NYSE BH.A Class A and BH Class B common, Indiana issuer. Possible investment-company risk is conditional, not current fund classification.',
'SAH':'NYSE SAH Class A common, Delaware issuer; blank-check preferred stock authorization is not a blank-check operating issuer assertion.',
'TDC':'NYSE TDC common, original cover states Delaware. Selected header lacks verified incorporation state, so that source fact remains unknown; primary audit evidence is not silently injected as a model state.',
'RAMP':'NYSE RAMP common, Delaware original quarterly issuer cover; no demonstrated wrong class/status in reviewed disclosures.',
}
rows=[]
for r in e['rows']:
    bad=r['ticker'] in ('HPP','AHH')
    rows.append({k:r[k] for k in ('ticker','security_id','session_date','cik_u','source','receipt_status','header')}|{'verdict':'confirmed_wrong_admission_reit' if bad else 'no_wrong_admission_demonstrated','note':notes[r['ticker']],'source_evidence_artifact':'t2r2-independent-audit-evidence.json'})
assert len(rows)==28
out={'sample_sha256':e['sample_sha256'],'evidence_sha256':C.sha256_file(path),'reviewed_by':'Codex gpt-6.1-sol Task2 fix round2, 2026-10-03','rows':rows,'counts':{'reviewed_unique_line_dates':28,'confirmed_wrong_admission_reit':2,'no_wrong_admission_demonstrated':26},'limits':'Manually reviewed original receipt-backed primary cover, identity, business/status passages. Deterministic risk-stratified diagnostic, not population precision; nonasserting text is not negative class proof. Exact French constituent overlap cannot be established from aggregate counts. Sparse dated source observations do not prove no intervening status changes.'}
C.write_json_atomic(art/'t2r2-independent-audit-verdicts.json',out);print(out['counts'])
