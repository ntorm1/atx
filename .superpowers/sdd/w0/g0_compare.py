"""Write complete, untuned old/new metric tables for the five G0 measurements."""
import csv
import json
from pathlib import Path

from g0_measure import CELLS, DATA, OUT, SHA

DEST=OUT/'comparisons'
DEST.mkdir(exist_ok=True)

def flatten(obj, path=''):
    if isinstance(obj,dict):
        return {key:value for k,v in obj.items() for key,value in flatten(v,f'{path}.{k}' if path else k).items()}
    if isinstance(obj,list):
        return {key:value for i,v in enumerate(obj) for key,value in flatten(v,f'{path}[{i}]').items()}
    return {path:obj}

def delta(old,new):
    try:
        if old in ('',None) or new in ('',None):
            return ''
        return float(new)-float(old)
    except (ValueError,TypeError):
        return ''

def write(name,rows):
    rows=list(rows)
    with (DEST/f'{name}.csv').open('w',encoding='utf-8',newline='') as out:
        writer=csv.DictWriter(out,fieldnames=['key','metric','old','new','delta','changed'])
        writer.writeheader()
        writer.writerows(rows)
    print(name,len(rows),'metrics',sum(bool(row['changed']) for row in rows),'changed')

def json_diff(name,old,new):
    a=flatten(json.loads(old.read_text(encoding='utf-8-sig')))
    b=flatten(json.loads(new.read_text(encoding='utf-8-sig')))
    write(name,[dict(key='',metric=key,old=a.get(key),new=b.get(key),
                     delta=delta(a.get(key),b.get(key)),changed=a.get(key)!=b.get(key))
                for key in sorted(a.keys()|b.keys())])

def csv_diff(name,old,new,keys):
    def read(path):
        with path.open(encoding='utf-8-sig',newline='') as source:
            rows=list(csv.DictReader(source))
        result={tuple(row[key] for key in keys):row for row in rows}
        assert len(result)==len(rows),(path,keys)
        return result
    a,b=read(old),read(new)
    rows=[]
    for key in sorted(a.keys()|b.keys()):
        before,after=a.get(key,{}),b.get(key,{})
        for metric in sorted(before.keys()|after.keys()):
            if metric in keys:
                continue
            v1,v2=before.get(metric),after.get(metric)
            rows.append(dict(key='|'.join(key),metric=metric,old=v1,new=v2,
                             delta=delta(v1,v2),changed=v1!=v2))
    write(name,rows)

if __name__=='__main__':
    json_diff('l9_gate',DATA/'equity_mine_l9_guard_20260923/gate_report.json',
              OUT/f'data/equity_mine_l9_guard_g0_{SHA}/gate_report.json')
    old=DATA/'equity_fund_zoo_ic_l10v2_20260923'
    new=OUT/f'data/equity_fund_zoo_ic_l10_g0_{SHA}'
    csv_diff('l10_pooled',old/'zoo_pooled.csv',new/'zoo_pooled.csv',['cut','signal'])
    csv_diff('l10_by_year',old/'ic_by_year.csv',new/'ic_by_year.csv',
             ['context','year','signal'])
    csv_diff('l10_splits',old/'zoo_pooled_splits.csv',new/'zoo_pooled_splits.csv',
             ['split','cut','signal'])
    json_diff('l10_alignment',old/'alignment.json',new/'alignment.json')
    json_diff('l7_all',DATA/'l7_riskmodel_scorecard_pit_2014_t1000_20260923/l7_scorecards.json',
              OUT/f'data/l7_riskmodel_scorecard_pit_2014_t1000_g0_{SHA}/l7_scorecards.json')
    old=DATA/'equity_scorecard21_scorecard_20260922'
    new=OUT/f'data/equity_g0cp21_scorecard_{SHA}'
    csv_diff('cp21_all',old/'scorecard.csv',new/'scorecard.csv',
             ['signal','horizon','variant','restriction','year','cut'])
    csv_diff('cp21_capacity',old/'capacity.csv',new/'capacity.csv',
             ['signal','cut','horizon','variant','restriction'])
    for year,cut in CELLS:
        json_diff(f'base_{year}_t{cut}',
                  DATA/f'equity_scorecard19_base_{year}_t{cut}_20260921/summary.json',
                  OUT/f'data/equity_g0cp21_base_{year}_t{cut}_{SHA}/summary.json')
        json_diff(f'ic_{year}_t{cut}',
                  DATA/f'equity_scorecard21_ic_{year}_t{cut}_20260922/ic_summary.json',
                  OUT/f'data/equity_g0cp21_ic_{year}_t{cut}_{SHA}/ic_summary.json')
