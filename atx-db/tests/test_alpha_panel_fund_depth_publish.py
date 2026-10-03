"""Offline Task5 phase failure/status and owner contract regressions."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import pytest
from atx_db.alpha_panel import fundamentals as F, fund_validate as V, common as C

ROOT=Path(__file__).resolve().parents[2]
def wrapper():
    spec=importlib.util.spec_from_file_location('t5_publish',ROOT/'.superpowers/sdd/tier1-v4/t5-publish.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def test_nonzero_child_stops_before_later_validation(monkeypatch):
    calls=[]
    monkeypatch.setattr(F,'main',lambda args:calls.append(args) or 77)
    monkeypatch.setattr(V,'main',lambda args:pytest.fail('later validator must not run'))
    with pytest.raises(SystemExit) as exc:wrapper().main(['fund-check'])
    assert exc.value.code==77 and len(calls)==1

def test_failed_measured_gate_publishes_fail_and_distinct_status(monkeypatch,tmp_path):
    m=wrapper();monkeypatch.setattr(C,'stage_dir',lambda stage:tmp_path)
    (tmp_path/'manifest.json').write_text(json.dumps({'files':{'preserved':'receipt'}}))
    assert m.finish('fixture',{'cutoff':'PASS','coverage':'FAIL'})==2
    man=json.loads((tmp_path/'manifest.json').read_text())
    status=json.loads((tmp_path/'task5_check_status.json').read_text())
    assert man['files']=={'preserved':'receipt'} and man['gate_status']['coverage']=='FAIL'
    assert status['execution_status']=='SUCCESS' and status['acceptance_status']=='FAIL'
    with pytest.raises(RuntimeError,match='no measured'):m.finish('fixture',{})
    assert m.finish('ratios_v11',{'coverage':'PASS'})==2
    assert json.loads((tmp_path/'manifest.json').read_text())['gate_status']['handcheck']=='NOT_MEASURED'

def test_fund_thresholds_use_measured_counts_not_legacy_pass99_flag():
    d={'coverage':{'items_counted':117,'items_pass_covered':105},
       'balance':{'issuer_periods':1000,'ok':985,'pass_99':False},
       'benchmark':{'pass':True},'cutoff':{'pass':True},
       'exit':{'fundamentals_v11':{'pooled':{k:{'linked_usd':t} for k,t in
        {'gp_ttm':.90,'sale_ttm':.96,'capx_ttm':.95,'txt_q':.95,'xrd_ttm':.95}.items()}}}}
    assert all(v=='PASS' for v in wrapper().fund_gates(d).values())
    d['coverage']['items_pass_covered']=104
    assert wrapper().fund_gates(d)['coverage_105_of_117']=='FAIL'

def test_owner_growth_families_have_exact_inputs_and_explicit_gaps():
    d=json.loads((ROOT/'atx-db/docs/datasets/fundamentals_v11.requirements.json').read_text())
    rows=d['formula_to_gold']
    for family in ('inventory','receivables','basic_shares','diluted_shares','investment','free_cash_flow'):
        for period in ('quarterly_yoy','annual','ttm'):
            r=rows[f'{family}_{period}_growth']
            assert r['current_input_columns'] and r['prior_input_columns'] and r['prior_period'] and r['source_gap']
    assert 'basic_share_dilution' in rows and 'diluted_share_dilution' in rows
    assert 'not currently published' in rows['diluted_shares_quarterly_yoy_growth']['source_gap']
    assert rows['free_cash_flow_quarterly_yoy_growth']['current_input_columns']==['cfo_q','capx_q']
