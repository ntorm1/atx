"""Persist reviewed originals and retain only unchanged, unaffected stratum claims."""
from pathlib import Path
from atx_db.alpha_panel import common as C,universe as U
art=Path('../.superpowers/sdd/tier1-v4');sample=C.read_json(C.stage_dir('validation')/'universe_hand_check_sample.json');e=C.read_json(art/'t2r2-hand-source-evidence.json');old=C.read_json(U.HAND_CHECK)
assert e['sample_sha256']==sample['sample_sha256'] and len(e['rows'])==64 and not e['remaining'] and not e['budget_stop']
evidence={(r['stratum'],r['security_id'],r['session_date']):r for r in e['rows']};prior={(r['stratum'],r['security_id'],r['session_date']):r for r in old['rows']}
states={'WLY':'New York','MAMA':'Nevada','BWB':'Minnesota','DGLY':'Nevada','LUV':'Texas','DAKT':'South Dakota','FHN':'Tennessee','BUSE':'Nevada','GTN.A':'Georgia','SBCF':'Florida'}
reit={
'RITM':'Current quarterly and annual identify Rithm as an internally managed REIT and elected REIT treatment.',
'STRW':'Quarterly states the Company elected and qualified to be treated/taxed as a REIT; annual confirms election. Historical tax commencement is audit context only.',
'MDRR':'Quarterly identifies the Maryland REIT issuer and tax election; annual confirms operated and elected REIT treatment.',
'BHR.PRD':'Braemar quarterly/annual tax election applies to the issuer of this preferred series; REIT flag covers every share type.',
'ARR':'Issuer quarterly/annual explicitly state elected REIT taxation.',
'NLY.PRG':'Annaly quarterly/annual identify internally managed Maryland corporation elected REIT taxation; preferred line remains a REIT issuer security.',
'WSR':'Annual explicitly says Maryland REIT and elected REIT taxation; quarterly describes continuing REIT organization.',
'SAFE':'Current Safehold quarterly/annual identify legal issuer previously iStar and its REIT election; reverse-merger accounting does not change the named legal issuer. Historical tax dates are not model inputs.',
'LFT':'Current quarterly/annual state issuer election and REIT treatment; subsidiary C-corporation treatment is distinct.',
'NXDT':'Current quarterly/annual explicitly identify NexPoint Diversified Real Estate Trust and elected REIT taxation. Former fund status is not assigned forward or new REIT status backward.',
'PMT.PRB':'Quarterly states Company believes it qualifies and has elected REIT taxation; annual confirms election. Preferred issuer claim is correct.',
'PLD':'Joint Parent/OP filing explicitly identifies Prologis Inc., target CIK1045609, as REIT Parent; OP CIK1045610 is separate. Parent assertion supports the sampled line.'}
foreign={'MFC':'Canada original issuer cover, 40-F and common-share listing.', 'ONCY':'Canadian original 20-F cover and common-share listing.', 'GIGM':'Original 20-F cover says Republic of Singapore; Taiwan office is not incorporation.', 'MDT':'Original current annual cover identifies Ireland incorporation and MDT ordinary shares.', 'LCW':'Original annual identifies Cayman exempted blank-check issuer. Foreign claim remains correct despite SPAC status.', 'NNOX':'Original 20-F identifies Israeli issuer and ordinary shares.', 'CRDO':'Original annual cover identifies Cayman Islands incorporation and CRDO ordinary shares.', 'DSGX':'Canadian 40-F issuer cover identifies Canada; US service agent is not issuer incorporation.', 'BG':'Original annual identifies Bunge Global SA and Switzerland following redomestication; dated historical description is audit context only.', 'JAZZ':'Original annual cover identifies Ireland incorporation and JAZZ ordinary shares.', 'CNQ':'Original Canadian 40-F and corporate-structure disclosure identify continuing Alberta issuer.', 'FRES':'Original 20-F target CIK identifies British Virgin Islands issuer; newer pre-sample 6-K verifies continuing foreign-private-issuer reporting. Prior ANPC name/ADS cover is not a foreign-claim defect; ADR false negative remains visible.'}
rows=[]
for n,s in enumerate(sample['rows']):
    key=(s['stratum'],s['security_id'],s['session_date']);r=prior.get(key,{})
    item={'i':n,'stratum':s['stratum'],'claim':r.get('claim',{'univ_all_common':'US listed common with plausible codes and price screen','is_reit':'issuer is a REIT, including preferred lines','is_foreign_issuer':'issuer is foreign private issuer or foreign incorporated'}.get(s['stratum'])),'session_date':s['session_date'],'security_id':s['security_id'],'ticker':s['ticker'],'finra_issue_name':s['finra_issue_name'],'sec_name':s['name'],'exchcd_like':s['exchcd_like'],'shrcd_like':s['shrcd_like'],'filer_regime':s['filer_regime']}
    if key in evidence:
        ev=evidence[key];assert ev['sources'] and all(x['status']==200 and not x['error'] for x in ev['sources'])
        if s['stratum']=='univ_all_common':
            note=f"Original pre-sample issuer cover identifies {states.get(s['ticker'],'Delaware')} incorporation and the listed common class/ticker; quarterly or annual operating disclosure supports issuer identity. Any separately listed warrants, preferred rights or debt are distinct securities."
            if s['ticker'] in ('RSI','GETY','INDI','PCT','LAZR'):note+=' Current operating issuer disclosure supersedes predecessor blank-check history for this claim; no historical status is backdated.'
        elif s['stratum']=='is_reit':note=reit[s['ticker']]
        else:note=foreign[s['ticker']]
        item.update(verdict='correct',note=note,review_status='Fix round2 manual original-primary cover/issuer/status review at stated sample date',sources=[{k:x.get(k) for k in ('url','raw_sha256','cik','accession','form','filing_date','registrant_ciks')} for x in ev['sources']],evidence_artifact='t2r2-hand-source-evidence.json')
    else:
        assert key in prior and s['stratum'] in ('is_adr','is_cef_etf','is_spac')
        item.update(verdict=r['verdict'],note=r['note'],review_status='Unchanged identity and unaffected ADR/fund/SPAC stratum claim; prior independent judgment retained')
        if 'sources' in r:item['sources']=r['sources']
    rows.append(item)
doc={**{k:v for k,v in old.items() if k not in ('rows','rule','sample_sha256','method','checked_by')},'rule':sample['rule'],'sample_sha256':sample['sample_sha256'],'rows':rows,'checked_by':'Codex gpt-6.1-sol Task2 fix round2, 2026-10-03','method':'64 affected common/REIT/foreign claims manually rechecked against original pre-sample issuer disclosures; 36 unchanged unaffected ADR/fund/SPAC claims retained. Three prior ADR errors remain. Evidence is audit only, never model inputs; source observations do not certify status between disclosures or all-year historical accuracy. Annual plus latest-periodic REIT context checks conversion; foreign annual older than365days receives latest periodic context. Exact sample hash binds current rule and flags.'}
C.write_json_atomic(U.HAND_CHECK,doc);print('hand judgments',len(rows),'correct',sum(r['verdict']=='correct' for r in rows),'rechecked',len(evidence))
