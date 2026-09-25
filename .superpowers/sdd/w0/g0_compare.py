"""Write complete, untuned old/new metric tables for the five G0 measurements."""
import csv
import json
from pathlib import Path

from g0_measure import CELLS, DATA, OUT, SHA

DEST=OUT/'comparisons'
DEST.mkdir(exist_ok=True)
COUNTS={}

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
    changed=sum(bool(row['changed']) for row in rows)
    COUNTS[name]=dict(metrics=len(rows),changed=changed)
    print(name,len(rows),'metrics',changed,'changed')

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

def l9_validation():
    def summarize(path):
        with path.open(encoding='utf-8-sig',newline='') as stream:
            rows=list(csv.DictReader(stream))
        values=[float(row['val_sharpe_net']) for row in rows]
        train=[float(row['train_sharpe_net']) for row in rows]
        return dict(families=len(rows),mean_val_sharpe_net=sum(values)/len(values),
            positive_val_sharpe_net=sum(value>0 for value in values),best_val_sharpe_net=max(values),
            min_p_raw=min(float(row['p_raw']) for row in rows),
            min_p_by=min(float(row['p_by']) for row in rows),
            min_p_rw=min(float(row['p_rw']) for row in rows),
            mean_train_sharpe_net=sum(train)/len(train),positive_train_sharpe_net=sum(v>0 for v in train),
            top_train_sharpe_net=max(train))
    a=summarize(DATA/'equity_mine_l9_guard_20260923/validation.csv')
    b=summarize(OUT/f'data/equity_mine_l9_guard_g0_{SHA}/validation.csv')
    write('l9_validation_aggregates',[dict(key='',metric=key,old=a[key],new=b[key],
        delta=delta(a[key],b[key]),changed=a[key]!=b[key]) for key in a])

def native_baseline():
    base=OUT/f'data/equity_baseline_training_2013_g0_{SHA}'
    corrected=base.with_name(base.name+'_corrected')
    disclosed=base.with_name(base.name+'_disclosed')
    json_diff('native2013_abort_control',base/'failure.json',
              base.with_name(base.name+'_abort_control')/'failure.json')
    json_diff('native2013_abort_disclosed',base/'failure.json',
              base.with_name(base.name+'_abort_disclosed')/'failure.json')
    # Frozen replay aborted before producing a summary. Blank old fields mean
    # unavailable, never a zero return or a fabricated completed baseline.
    values=flatten(json.loads((disclosed/'summary.json').read_text(encoding='utf-8-sig')))
    write('native2013_new_summary',[dict(key='',metric=key,old=None,new=value,
        delta='',changed=True) for key,value in sorted(values.items())])
    json_diff('native2013_disclosure_top_level',corrected/'summary.json',disclosed/'summary.json')
    json_diff('native2013_disclosure_replay',corrected/'report/summary.json',
              disclosed/'report/summary.json')
    a=json.loads((corrected/'report/summary.json').read_text(encoding='utf-8-sig'))
    b=json.loads((disclosed/'report/summary.json').read_text(encoding='utf-8-sig'))
    assert a==b,'Presentation-only follow-up changed underlying replay summary'
    assert b['usable_for_alpha_evidence'] is False

if __name__=='__main__':
    json_diff('l9_gate',DATA/'equity_mine_l9_guard_20260923/gate_report.json',
              OUT/f'data/equity_mine_l9_guard_g0_{SHA}/gate_report.json')
    l9_validation()
    old=DATA/'equity_fund_zoo_ic_l10v2_20260923'
    new=OUT/f'data/equity_fund_zoo_ic_l10_g0_{SHA}'
    csv_diff('l10_pooled',old/'zoo_pooled.csv',new/'zoo_pooled.csv',['cut','signal'])
    csv_diff('l10_by_year',old/'ic_by_year.csv',new/'ic_by_year.csv',
             ['context','year','signal'])
    csv_diff('l10_splits',old/'zoo_pooled_splits.csv',new/'zoo_pooled_splits.csv',
             ['split','cut','signal'])
    json_diff('l10_alignment',old/'alignment.json',new/'alignment.json')
    json_diff('l10_manifest',old/'manifest.json',new/'manifest.json')
    json_diff('l7_all',DATA/'l7_riskmodel_scorecard_pit_2014_t1000_20260923/l7_scorecards.json',
              OUT/f'data/l7_riskmodel_scorecard_pit_2014_t1000_g0_{SHA}/l7_scorecards.json')
    native_baseline()
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
        json_diff(f'ic_manifest_{year}_t{cut}',
                  DATA/f'equity_scorecard21_ic_{year}_t{cut}_20260922/manifest.json',
                  OUT/f'data/equity_g0cp21_ic_{year}_t{cut}_{SHA}/manifest.json')
    (DEST/'index.json').write_text(json.dumps(COUNTS,indent=2),encoding='utf-8')
