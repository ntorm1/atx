from pathlib import Path
import json
from atx_db.alpha_panel import common as C, universe as U
s=C.read_json(C.build_root()/'validation/universe_hand_check_sample.json')
p=Path('docs/datasets/universe_hand_check.json');old=json.loads(p.read_text(encoding='utf-8'));prior={(r['stratum'],r['security_id']):r for r in old['rows']}
base='https://www.sec.gov/Archives/edgar/data/'
evidence={
'KLTR':('1432133/000143213325000025/kltr-20241231.htm','FY2024 10-K: Delaware since 2006, KLTR Nasdaq common; retrospective factual verification, not a historical model input'),
'MPTI':('1902314/000143774925013912/mptiars.pdf','FY2024 10-K: Delaware common MPTI NYSE American; explicitly reports June 2024 common-share market value'),
'LENZ':('1815776/000162828024013816/lenz-20240329.htm','March 2024 merger disclosure: Delaware corporations and common shares; operating therapeutics issuer'),
'MCW':('1853513/000095017024043410/mcw_annual_report_2023.pdf','FY2023 10-K: MCW common listed NYSE; Delaware issuer'),
'SERA':('1534969/000095017025057411/sera-ars-2025.pdf','FY2024 annual report explicitly describes June 28 2024 Nasdaq common-share market value and Delaware incorporation'),
'MRAC':('1830180/000119312521246807/d182243d10q.htm','June 30 2021 10-Q: blank-check issuer and proposed Enjoy combination still pending; incorporation Cayman'),
'SGAM':('1820201/000110465921106400/sgam-20210630x10q.htm','June 30 2021 10-Q: blank-check Seaport and proposed Redbox combination; completion in October 2021'),
'SCOB':('1838431/000121390021008136/f424b40221_sciontech2.htm','February 2021 prospectus: blank-check Cayman ScION Tech Growth II; no target selected'),
'GSAH':('1809987/000119312521300651/d175196d425.htm','October 2021 issuer release: GSAH still SPAC, expected combination October 20; June 30 precedes closing'),
'DISAU':('1838831/000095010321003639/dp144727_ex1006.htm','2021 SEC-filed note identifies Disruptive Acquisition Corporation I as Cayman blank-check issuer; sampled line is unit'),
'CFFVU':('1874315/000119312521282132/d178511df4a.htm','2021 combination registration: CF V agreement announced July 6 2021, after June 30; sampled unit is blank-check issuer security'),
'WPF.WS':('1809104/000119312521211067/d187891d8k.htm','July 2 2021 closing/warrant-assumption 8-K: Foley Trasimene warrant remains SPAC issuer security on June 30'),
'BENEW':('1828735/000110465921002536/tm2034409d11_ex99-1.htm','January 2021 IPO release: blank-check Benessere; sampled warrant belongs to SPAC issuer'),
'ATHN.U':('1840292/000121390021028744/f10q0321_athenatechnology.htm','March 2021 10-Q: Delaware blank-check Athena; sampled unit belongs to issuer'),
'PNTM.WS':('1830392/000121390021002376/f424b4021_pontemcorp.htm','January 2021 prospectus: Cayman blank-check Pontem; sampled warrant belongs to issuer'),
'IPVA.U':('1839608/000121390021014130/f424b40321_interprivate2.htm','March 2021 prospectus: blank-check InterPrivate II; sampled unit belongs to issuer'),
'IACB':('1835378/000121390021010219/ea135907ex99-2_ionacquis2.htm','February 2021 IPO release: blank-check ION Acquisition Corp 2; June 2021 precedes November combination'),
}
new=[]
for i,r in enumerate(s['rows']):
 k=(r['stratum'],r['security_id'])
 if k in prior:
  x=dict(prior[k]);x['review_status']='unchanged sample identity and stratum claim; prior independent judgment retained'
 else:
  source,note=evidence[r['ticker']]
  x={'stratum':r['stratum'],'session_date':r['session_date'],'security_id':r['security_id'],'ticker':r['ticker'],
     'claim':'US listed operating common' if r['stratum']=='univ_all_common' else 'issuer is blank-check on the sampled date (units/warrants included)',
     'verdict':'correct','note':note,'evidence_url':base+source,'review_status':'new sample manually rechecked against dated primary evidence'}
 x.update(i=i,finra_issue_name=r['finra_issue_name'],sec_name=r['name'],exchcd_like=r['exchcd_like'],shrcd_like=r['shrcd_like'],filer_regime=r['filer_regime'])
 new.append(x)
assert len(new)==100
out={**old,'rule':U.RULE,'seed':s['seed'],'sample_sha256':s['sample_sha256'],'strata':s['strata'],'rows':new,
 'checked_by':'Task 2 fix round 1 implementer, 2026-10-03',
 'method':'17 new sample identities manually adjudicated from primary SEC dated descriptions/closing chronology; 83 unchanged identity/stratum assertions retain prior judgments. Hash binds this exact generated sample and rule. Later historical descriptions are audit evidence only, never model inputs. Per-stratum ADR defects remain visible.'}
p.write_text(json.dumps(out,indent=2)+'\n',encoding='utf-8')
print(json.dumps(U.hand_check_gate(sample=s['rows']),indent=2))
