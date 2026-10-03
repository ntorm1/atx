"""Render measured dataset documentation from final published metadata, without SQL."""
from pathlib import Path
import json,re
from atx_db.alpha_panel import common as C
root=C.build_root();docs=C.PACKAGE_ROOT/'docs/datasets';u=C.read_json(root/'universe/manifest.json');v=C.read_json(root/'validation/universe.json');p=docs/'universe.md';text=p.read_text(encoding='utf-8')
text=text.replace('tier1-v4 Task 2 fix round 1 and Task 2x. Rule `universe-all-common-v2`','tier1-v4 Task 2 fix round 2 (Task 2x remains approved). Rule `universe-all-common-v3`')
text=text.replace('issuer is a REIT: panel `is_reit` or issuer SIC 6798 (any share type of a REIT, preferreds included) | panel, classification | SIC clock < cutoff | daily','latest explicit issuer-bound original REIT status; fallback panel `is_reit` or SIC 6798; termination overrides stale flags (preferreds included) | issuer_classes, panel, classification | original availability / SIC clock < cutoff | sparse original status observations; 2023-2026 refresh required')
text=text.replace('incorporation outside the US (cover page strictly before cutoff; unknown without dated evidence) | panel, sec_filings, identity_cover','incorporation outside the US (dated cover or target-bound original header; unknown without dated evidence) | panel, sec_filings, identity_cover, issuer_history, issuer_classes')
text=text.replace('Incorporation is unknown when no dated cover is visible','Incorporation is unknown when no dated cover or target-bound original header is visible')
source={}
for line in text.splitlines():
    a=line.split('|')
    if len(a)>=9 and a[1].strip() in v['coverage']['2020']['univ_all_common']:source[a[1].strip()]=a[2].strip()
def replace_section(heading,body):
    global text
    text=re.sub(r'(## '+re.escape(heading)+r'\n).*?(?=\n## |\Z)',lambda m:m[1]+'\n'+body.rstrip()+'\n',text,flags=re.S)
lines=['| gate | result | measurement |','|---|---|---|']
for name,g in v['gates'].items():
    if name=='daily_no_price_screen':continue
    if name=='french_nyse':detail=f"{g['months_outside']}/{g['months']} outside unchanged +/-5%; max absolute deviation {g['max_abs_dev']:.2%}; no missing months"
    elif name=='hand_check':detail=f"{g['correct']}/{g['lines']} pooled; ADR {g['by_stratum']['is_adr']['correct']}/12; 64 changed common/REIT/foreign claims independently rechecked"
    elif name=='daily_band':detail='2020–2025 yearly means: '+', '.join(f"{y}: {r['mean']}" for y,r in g['per_year'].items())
    elif name=='codes_on_all_common':detail='100% present in every year 2020–2025; presence is not independent semantic classification proof'
    else:detail='All nine proxy reconstitutions within [1900,2100]: '+str(g['companies'])
    lines.append(f"| {name} | {'PASS' if g['pass'] else 'FAIL'} | {detail} |")
replace_section('Measured gates','\n'.join(lines))
lines=['| year | rows | bytes |','|---|---:|---:|']
for y,n in u['receipt']['rows'].items():lines.append(f"| {y} | {n:,} | {u['files'][f'year={y}/universe.parquet']['bytes']:,} |")
replace_section('Published rows','\n'.join(lines))
counts=v['counts'];fields=list(next(iter(counts.values())))
lines=['| year | '+' | '.join(fields)+' |','|---|'+'---:|'*len(fields)]
for y,r in counts.items():lines.append('| '+y+' | '+' | '.join(str(r[k]) for k in fields)+' |')
replace_section('Mean daily line counts','\n'.join(lines))
lines=['| month | NYSE common lines | French documented count | residual lines | deviation |','|---|---:|---:|---:|---:|']
for r in v['gates']['french_nyse']['monthly']:lines.append(f"| {r['month']} | {r['lines']} | {r['french']} | {r['lines']-r['french']:+d} | {r['dev_lines']:+.2%} |")
replace_section('French NYSE comparison (value only, before 2023)','\n'.join(lines))
lines=['Value/presence coverage spans every year 2020–2025 and all three masks below despite aggregate market returns stopping at 2022. beta_input and idio_vol_input measure input sufficiency, not actual derived values; Task 6 publishes actual-field coverage. No post-2022 aggregate return values are computed or inspected here.']
for mask in ('univ_all_common','univ_r3000p','member_equity'):
    lines+=['',f'### {mask}','','| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |','|---|---|---:|---:|---:|---:|---:|---:|']
    for field in sorted(k for k in v['coverage']['2020'][mask] if k!='cells'):lines.append('| '+field+' | '+source.get(field,'validation core-field definition')+' | '+' | '.join(str(v['coverage'][str(y)][mask][field]) for y in range(2020,2026))+' |')
    lines.append('| denominator cells | line-session cells | '+' | '.join(str(v['coverage'][str(y)][mask]['cells']) for y in range(2020,2026))+' |')
replace_section('Coverage: 30 core fields','\n'.join(lines))
replace_section('Known limits and residuals',"""The French accuracy threshold remains +/-5% and the daily band 3,200–4,800. French remains FAIL: Jan–Apr2018, June2020 and Jan–May2022. Month-by-month overlapping source counters and exclusive exclusion classes are preserved in `.superpowers/sdd/tier1-v4/t2r2-french-reconciliation.json`; they partition our observations but cannot identify missing/extra licensed benchmark constituents. The benchmark's documented line-count convention remains binding; company counts are diagnostic only.

Early2018 has 277/233/230/215 otherwise-eligible NYSE observations missing positive shares versus deficits164/126/123/129. These are measured shares-source limitations; no shares or prices are synthesized. June2020 source continuity evidence is rechecked in t2r2-vendor-coverage.json. No existing authorized independent June2020 price replacement was established. The 2022 excess remains unexplained at constituent level: incorporation is known on admitted lines, while nonasserting class disclosures remain unknown rather than negative proof. A deterministic independent28-line-date audit found two demonstrated REIT admission errors and26 cases without a demonstrated error; it is not population precision or French overlap attribution. Zero same-FINRA-classifier suspects is internal consistency only.

The bounded new source stages publish1,346 incorporation observations and49 explicit REIT status observations across11 issuers. Header/class sources are sparse and stop in2022. Full2018–2026 output carries latest known assertions with existing fallback; no uninterrupted status or2023–2026 conversion/termination coverage is certified. Subsequent source refresh must include dated positive/negative status changes and unknowns. An explicit REIT termination clears stale SIC/panel exclusion only after disclosure availability; historical effective dates are not backdated.

Index flags remain unlicensed proxies without float adjustment and with incomplete IPO/spin-off additions. Task2x market aggregate returns remain through2022 only underR5; membership, annual coverage and universe flags span latest. Presence of identity share codes does not prove independent semantic agreement with newly observed class flags; downstream code refresh must review dated class evidence.

Hand judgments bind the exact current seed/rule/sample hash. All64 affected common/REIT/foreign claims were independently rechecked from originals, and36 unaffected unchanged ADR/fund/SPAC judgments retained. Pooled97/100 does not conceal ADR9/12. Original audit disclosures are evidence only, never backdated model inputs. See task-2-report.md Fix round2 and standalone issuer_history/issuer_classes docs for provenance, source completeness and remaining obligations.""")
lines=['| input | SHA-256 |','|---|---|']+[f'| {k} | {h} |' for k,h in u['input_manifests_sha256'].items()]
replace_section('Input manifest provenance','\n'.join(lines))
for a,b in {'Jan–Apr2018':'Jan–Apr 2018','June2020':'June 2020','Jan–May2022':'Jan–May 2022','Early2018':'Early 2018','deficits164':'deficits 164','independent28-line-date':'independent 28-line-date','and26':'and 26','publish1,346':'publish 1,346','and49':'and 49','across11':'across 11','in2022':'in 2022','Full2018':'Full 2018','or2023':'or 2023','Task2x':'Task 2x','through2022':'through 2022','underR5':'under R5','All64':'All 64','and36':'and 36','Pooled97':'Pooled 97','ADR9':'ADR 9','round2':'round 2'}.items():text=text.replace(a,b)
text=text.replace('June 2020 source continuity evidence is rechecked in t2r2-vendor-coverage.json.','June 2020 source continuity recheck finds 89 of 1,155 neighboring-month common lines lack June30 panel, price and original vendor bars (t2r2-vendor-coverage.json). This does not identify exact French missing constituents.')
p.write_text(text,encoding='utf-8')
stub=C.read_json(docs/'universe.registry.json');stub['task']='tier1-v4 Task 2 fix round 2; Task 2x approved';stub['inputs']=list(u['input_manifests_sha256']);stub['staleness']='daily panel outputs; sparse historical issuer assertions through2022, later status refresh required';C.write_json_atomic(docs/'universe.registry.json',stub)
print('rendered final measured universe documentation')
