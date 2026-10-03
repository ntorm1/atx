import datetime as dt
import math

import pytest

from atx_db.alpha_panel import ea_actuals as EA


def release(extra="", unit="USD in millions, except per-share amounts", duration="Three months ended"):
    return f'''<html><p>{unit}</p><table><tr><th>{duration}</th></tr>
    <tr><td></td><td>March 31, 2020</td><td>March 31, 2019</td></tr>
    <tr><td>Net revenues</td><td>120</td><td>100</td></tr>
    <tr><td>Net income (loss)</td><td>(12)</td><td>10</td></tr>
    <tr><td>Diluted earnings per share</td><td>(0.50)</td><td>0.40</td></tr>{extra}</table></html>'''.encode()


def parse(blob):
    return EA.parse_release(blob, dt.date(2020, 5, 1))


def test_quarter_current_comparative_units_and_loss_sign():
    r = parse(release())
    assert r["period_end"] == dt.date(2020, 3, 31)
    assert (r["saleq"], r["niq"], r["epsfiq"]) == (120e6, -12e6, -.5)
    assert r["saleq_seasonal_comparative"] == 100e6
    assert r["epsfiq_seasonal_comparative"] == .4
    assert r["eps_split_comparable"]
    assert r["period_days"] is None


@pytest.mark.parametrize("unit", ["in millions", "USD CAD in millions", "USD", "USD in millions and in thousands"])
def test_unknown_or_conflicting_currency_and_scale_is_missing(unit):
    assert parse(release(unit=unit))["saleq"] is None


@pytest.mark.parametrize("duration", ["Six months ended", "Year ended", "Three months and nine months ended"])
def test_duration_ambiguity_never_recasts_ytd_as_quarter(duration):
    assert parse(release(duration=duration))["saleq"] is None


def test_conflicting_gaap_rows_not_chosen_by_rank():
    r = parse(release('<tr><td>Net revenues</td><td>121</td><td>100</td></tr>'))
    assert r["saleq"] is None and "saleq:conflicting_rows" in r["reason"]


def test_non_gaap_table_excluded():
    assert parse(release(duration="Adjusted three months ended"))["saleq"] is None


def test_dash_is_missing_and_eps_scale_requires_declared_exception():
    assert EA.number("—") is None
    assert EA.number("12%") is None
    assert EA.number("(1,000)") == -1000
    r = parse(release(unit="USD in millions"))
    assert r["saleq"] == 120e6 and r["epsfiq"] is None


def test_ambiguous_and_missing_acceptance_clock():
    assert EA.qualified_clock({"available_at": dt.datetime(2020, 1, 1)}) is None
    assert EA.qualified_clock({"acceptance_clock": "unresolved_conservative", "available_at": dt.datetime(2020, 1, 1)}) is None


def test_prior_eight_surprises_and_cutoff_invariance():
    rows = [{"accession": str(i), "period_end": dt.date(2015+i//4, 3*(i%4+1), 28),
             "available_at": dt.datetime(2015+i//4, 3*(i%4+1), 29), "currency": "USD",
             "fiscal_year_label":2015+i//4,"fiscal_quarter":i%4+1,"fiscal_calendar_id":"12-31",
             "saleq": 100+i*i, "saleq_seasonal_comparative": 100,
             "epsfiq": None} for i in range(12)]
    result = EA.sue_history(rows)
    assert result[7]["sue_rev_ann"] is None
    assert result[8]["sue_rev_ann"] == pytest.approx(64/math.sqrt(sum((i*i-17.5)**2 for i in range(8))/7))
    assert EA.sue_history(rows[:9]) == result[:9]


def test_mixed_metric_table_currencies_fail_closed():
    import re
    first=re.sub(r'<tr><td>(?:Net income \(loss\)|Diluted earnings per share)</td>.*?</tr>','',release().decode())
    first=first.replace('<html>','<html><div>').replace('</html>','</div>')
    second=re.sub(r'<tr><td>Net revenues</td>.*?</tr>','',release(unit='CAD in millions, except per-share amounts').decode())
    second=second.replace('<html>','<div>').replace('</html>','</div></html>')
    r=parse((first+second).encode())
    assert r['currency'] is None and all(r[f] is None for f in EA.FIELDS)
    assert r['reason']=='mixed_currencies_across_metrics'


def test_comparative_conflicts_merge_independent_of_document_order():
    a=parse(release()); b=parse(release().replace(b'<td>100</td>',b'<td>99</td>'))
    merged=EA.merge_extracts([a,b])
    assert merged==EA.merge_extracts([b,a])
    assert merged['saleq']==120e6 and merged['saleq_seasonal_comparative'] is None
    assert 'saleq_seasonal_comparative:conflicting_documents' in merged['reason']
    b={**a,'period_basis':'unsupported_other_basis'}
    assert all(EA.merge_extracts([a,b])[f] is None for f in EA.FIELDS)


def test_source_quarter_requires_explicit_calendar_and_matching_period():
    text='First quarter 2020 financial results. Fiscal year ends December 31.'
    assert EA.fiscal_identity(text,dt.date(2020,3,31))['fiscal_quarter']==1
    assert EA.fiscal_identity('First quarter 2020 financial results',dt.date(2020,3,31))['fiscal_quarter'] is None
    assert EA.fiscal_identity(text,dt.date(2020,9,30))['fiscal_quarter'] is None


def test_annual_only_missing_quarter_and_calendar_transition_fail_sue():
    annual=[{'accession':str(i),'period_end':dt.date(2010+i,3,31),'available_at':dt.datetime(2010+i,5,1),
             'currency':'USD','saleq':100+i*i,'saleq_seasonal_comparative':100,
             'fiscal_year_label':2010+i,'fiscal_quarter':1,'fiscal_calendar_id':'12-31'} for i in range(9)]
    assert EA.sue_history(annual)[-1]['sue_rev_ann'] is None
    quarterly=[{**r,'accession':str(i),'period_end':dt.date(2010+i//4,3*(i%4+1),28),
                'available_at':dt.datetime(2010+i//4,3*(i%4+1),29),
                'fiscal_year_label':2010+i//4,'fiscal_quarter':i%4+1} for i,r in enumerate(annual)]
    assert EA.sue_history(quarterly)[-1]['sue_rev_ann'] is not None
    assert EA.sue_history(quarterly[:4]+quarterly[5:])[-1]['sue_rev_ann'] is None
    quarterly[-1]['fiscal_calendar_id']='09-30'
    last=EA.sue_history(quarterly)[-1]
    assert last['sue_rev_ann'] is None and 'calendar_transition' in last['sue_rev_ann_reason']


def test_same_clock_and_future_restated_vintages_not_visible_to_sue():
    rows = [{"accession": str(i), "period_end": dt.date(2010+i//4, 3*(i%4+1), 28),
             "available_at": dt.datetime(2020, 1, 1), "currency": "USD", "saleq": i,
             "fiscal_year_label":2010+i//4,"fiscal_quarter":i%4+1,"fiscal_calendar_id":"12-31",
             "saleq_seasonal_comparative": 0} for i in range(10)]
    assert all(r["sue_rev_ann"] is None for r in EA.sue_history(rows))
    assert EA.sue_history([{**r,'available_at':dt.datetime.combine(r['period_end'],dt.time())+dt.timedelta(days=1)} for r in rows])[-1]['sue_rev_ann'] is not None


def test_reaction_window_boundary_and_completion_clock():
    clock = dt.datetime(2022, 12, 30, 22)
    dates = [dt.date(2022, 12, d) for d in (28, 29, 30)]
    value, at, reason = EA.reaction_return(dates, [.1, -.1, .02], clock)
    assert value == pytest.approx(.0098) and at == clock and reason == "qualified"
    dates[-1] = dt.date(2023, 1, 3)
    assert EA.reaction_return(dates, [object(), object(), object()], clock)[2].startswith("holdout")
    assert EA.reaction_return(dates[:2], [0, 0], clock)[2] == "incomplete_window"


def test_resumable_units_reuse_exact_input_and_reject_changes(tmp_path,monkeypatch):
    row={'cik':1,'accession':'a','filing_date':dt.date(2020,5,1),'source_available_at':dt.datetime(2020,5,1,18),'acceptance_clock':'file_utc'}
    doc={'source_doc':'ex99.htm','doc_sha256':'abc'}
    from collections import defaultdict
    counts=defaultdict(int)
    first=EA.cached_extract(release(),row,doc,tmp_path,'code-input-v1',counts,1)
    assert counts['new_units']==1
    monkeypatch.setattr(EA,'parse_release',lambda *args:pytest.fail('complete unit was reparsed'))
    again=EA.cached_extract(release(),row,doc,tmp_path,'code-input-v1',counts,1)
    assert first==again and counts['cache_hits']==1
    changed={**row,'filing_date':dt.date(2020,5,2)}
    with pytest.raises(EA.ParseUnitLimit):
        EA.cached_extract(release(),changed,doc,tmp_path,'code-input-v1',counts,1)
    with pytest.raises(EA.ParseUnitLimit):
        EA.cached_extract(release(),row,doc,tmp_path,'code-input-v2',counts,1)
    # Interrupted writes carry no complete status and are never admitted.
    for path in tmp_path.rglob('*.json'):
        path.write_text('{"status":"incomplete"}',encoding='utf-8')
    with pytest.raises(EA.ParseUnitLimit):
        EA.cached_extract(release(),row,doc,tmp_path,'code-input-v1',counts,1)
