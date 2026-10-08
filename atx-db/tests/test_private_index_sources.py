"""Source-shaped conditional checks; these never admit actual source bytes."""
import datetime as dt
from dataclasses import replace
from atx_db.alpha_panel import private_index_sources as S

D=dt.date
UTC=dt.timezone.utc


def test_physical_sec_main_history_replay_and_early_close_refusal(tmp_path):
    """Synthetic original SEC shapes; fixture review grants no actual source PASS."""
    import json
    import duckdb
    import pytest
    from pathlib import Path
    from atx_db.alpha_panel import common as C, filing_text as FT, filing_text_native as N
    from atx_db.alpha_panel import private_tier1_family_sources as G
    root=tmp_path/'originals'
    root.mkdir()
    accession='0000000001-21-000001'
    main_name='CIK0000000001.json'
    history_name='CIK0000000001-submissions-001.json'
    columns=lambda acc,form,date: dict(accessionNumber=[acc],form=[form],filingDate=[date],
        reportDate=[date],primaryDocument=['filing.htm'],acceptanceDateTime=[date+'T12:00:00Z'])
    main={'cik':'0000000001','filings':{'recent':columns(accession,'10-Q','2021-05-06'),
        'files':[{'name':history_name,'filingCount':1,'filingFrom':'2020-12-01','filingTo':'2020-12-01'}]}}
    history=columns('0000000001-20-000001','20-F','2020-12-01')
    submissions=[]
    for source,name,document in ((0,main_name,main),(1,history_name,history)):
        (root/name).write_text(json.dumps(document))
        url='https://data.sec.gov/submissions/'+name
        receipt=name+'.receipt.json'
        (root/receipt).write_text(json.dumps({'http_status':200,'error':None,'url':url,**N.pin(root/name)}))
        submissions.append({'cik':1,'source':source,'path':name,'receipt':receipt,'url':url,'filing_count':1})
    index='accepted-index.htm'
    (root/index).write_text(f'<html>SEC Accession No. {accession} CIK: 0000000001 Accepted 2021-05-06 08:00:00</html>')
    index_receipt=index+'.receipt.json'
    url=f"{FT.ARCHIVES}/1/{accession.replace('-','')}/{accession}-index.htm"
    (root/index_receipt).write_text(json.dumps({'http_status':200,'error':None,'url':url,**N.pin(root/index)}))
    (root/'census-evidence.json').write_text(json.dumps({'fixture_only':'independent complete one-issuer census'}))
    code={str(Path(module.__file__).resolve()):N.pin(module.__file__) for module in (C,S.F,FT,N,G,G.R,S)}
    plan={'schema':'atx.index-sec-regime-source-plan/v1','runtime':N.runtime_identity(),
          'code':code,'files':{p.name:N.pin(p) for p in root.iterdir()},
          'census_evidence':'census-evidence.json','expected_ciks':[1],'submissions':submissions,
          'acceptance_proofs':[{'cik':1,'accession':accession,'index_path':index,'receipt':index_receipt}]}
    (root/'source-plan.json').write_text(json.dumps(plan))
    review={'schema':'atx.index-sec-regime-review/v1','status':'scoped_pass',
            'source_plan_sha256':N.digest(plan),'code':code,
            'census_evidence_sha256':plan['files']['census-evidence.json']['sha256']}
    (root/'fixture-review.json').write_text(json.dumps(review))
    spec={'source_root':str(root),'plan_path':'source-plan.json','plan_pin':N.pin(root/'source-plan.json'),
          'expected_plan_sha256':N.digest(plan),
          'independent_review':{'path':'fixture-review.json','pin':N.pin(root/'fixture-review.json')},
          'dependency_pins':{**{str(root/name):value for name,value in plan['files'].items()},**code,
              str(root/'source-plan.json'):N.pin(root/'source-plan.json'),
              str(root/'fixture-review.json'):N.pin(root/'fixture-review.json')}}
    with duckdb.connect(str(tmp_path/'sec-replay.duckdb')) as con:
        con.execute("SET memory_limit='128MB'; SET threads=1; SET TimeZone='UTC'")
        rows=list(S.iter_sec_regime(spec,con))
        assert [row['form'] for row in rows]==['10-Q','20-F']
        assert rows[0]['available_at']==dt.datetime(2021,5,6,12,tzinfo=UTC)
        assert rows[0]['qualified_clock']
        assert rows[1]['available_at'] is None and not rows[1]['qualified_clock']
        assert rows[1]['source_accession']=='0000000001-20-000001'
        assert S.prepare_sec_regime(spec,con)=={
            'rows':2,'missing_or_unqualified_clock_rows':1,'source_quality_certified':False}
        assert con.execute('SELECT form,qualified_clock FROM index_sec_regime ORDER BY filing_date').fetchall()==[
            ('20-F',False),('10-Q',True)]
        stream=S.iter_sec_regime(spec,con)
        next(stream)
        (root/history_name).write_text(json.dumps({**history,'tampered':True}))
        with pytest.raises(ValueError,match='changed independently pinned original SEC dependency'):
            stream.close()


def test_unclocked_dated_original_excludes_field_without_stale_fallback():
    old=S.DatedFact('10-Q',dt.datetime(2021,4,1,tzinfo=UTC),'a'*64,
                    source_accession='old',period=D(2021,4,1))
    unresolved=replace(old,value='20-F',available_at=None,source_accession='new',
                       period=D(2021,5,6))
    selected=S.latest_facts([('form',old),('form',unresolved)],D(2021,5,7))
    assert selected['form'].value is None and selected['form'].available_at is None
    assert 'unresolved_original_public_clock_no_stale_fallback' in selected['form'].exclusions
    # The full native population is retained separately; a future period cannot
    # contaminate this earlier field merely because its raw public clock is absent.
    selected=S.latest_facts([('form',old),('form',replace(unresolved,period=D(2022,1,1)))],D(2021,5,7))
    assert selected['form']==old


def facts():
    available=dt.datetime(2021,5,6,20,tzinfo=UTC)
    values={'issue_name':'EXAMPLE INC COMMON STOCK','symbol':'EXMP','market_class':'SC',
        'cik':123,'link_tier':'strict','identity_status':'linked','form':'10-Q','sic':3571,
        'sale_ttm':100.,'is_index':False,'close':10.,'shrout':100.}
    return {key:S.DatedFact(value,dt.datetime(2021,5,7,21,tzinfo=UTC) if key=='close' else available,
            'a'*64,source_accession='original',period=D(2021,5,7) if key=='close' else (D(2021,3,31)
            if key in ('sic','sale_ttm') else D(2021,5,6))) for key,value in values.items()}


def test_backfill_cik_cannot_decide_company_or_operating():
    for tier in ('strict','name'):
        p=facts()
        p['link_tier']=replace(p['link_tier'],value=tier)
        assert S.classify_rank(D(2021,5,7),p)['eligible']
    for tier in ('backfill','snapshot-run-backfill-v1',None):
        p=facts()
        p['link_tier']=replace(p['link_tier'],value=tier)
        result=S.classify_rank(D(2021,5,7),p)
        assert not result['qualified'] and result['cik'] is None
        assert result['is_operating'] is None
    p=facts()
    del p['link_tier']
    assert not S.classify_rank(D(2021,5,7),p)['qualified']
    p=facts()
    for field in ('cik','link_tier','form','sic','sale_ttm'):
        del p[field]
    p['identity_status']=replace(p['identity_status'],value='proven_unlinked',
        available_at=dt.datetime(2021,5,7,22,tzinfo=UTC))
    p['identity_cutoff']=replace(p['identity_status'],value=dt.datetime(2021,5,7,22,tzinfo=UTC))
    result=S.classify_rank(D(2021,5,7),p)
    assert result['eligible'] and result['is_operating'] and result['cik'] is None
    p['identity_cutoff']=replace(p['identity_cutoff'],value=dt.datetime(2021,5,6,22,tzinfo=UTC))
    assert not S.classify_rank(D(2021,5,7),p)['qualified']
    p['identity_status']=replace(p['identity_status'],value='unknown')
    assert not S.classify_rank(D(2021,5,7),p)['qualified']


def test_native_calendar_axis_preserves_rank_plan_and_refuses_revision_fallback(tmp_path):
    """Typed phase fixture; no market source admission is asserted here."""
    import duckdb
    import pytest
    with duckdb.connect(str(tmp_path/'calendar-phase.duckdb')) as con:
        con.execute("SET memory_limit='128MB'; SET threads=1; SET TimeZone='UTC'")
        con.execute('CREATE TABLE index_realized(session_date DATE,available_at TIMESTAMPTZ,'
                    'qualified BOOLEAN,reason VARCHAR)')
        con.executemany('INSERT INTO index_realized VALUES (?, ?, true, ?)',
            [(day,dt.datetime.combine(day,dt.time(22),UTC),'fixture')
             for day in (D(2021,6,18),D(2021,6,22),D(2021,6,23))])
        con.execute('CREATE TABLE index_planned(calendar_date DATE,session_date DATE,'
            'is_session BOOLEAN,vintage_id VARCHAR,covered_start DATE,covered_end DATE,'
            'available_at TIMESTAMPTZ,qualified BOOLEAN,reason VARCHAR)')
        rows=[]
        for vintage,clock,closed in (('early',dt.datetime(2021,5,20,12,tzinfo=UTC),False),
                                    ('revision',dt.datetime(2021,6,20,12,tzinfo=UTC),True)):
            for offset in range(6):
                day=D(2021,6,18)+dt.timedelta(days=offset)
                session=day in (D(2021,6,18),D(2021,6,21),D(2021,6,22),D(2021,6,23))
                if closed and day==D(2021,6,21):
                    session=False
                rows.append((day,day if session else None,session,vintage,D(2021,6,18),
                    D(2021,6,23),clock,True,'fixture-only'))
        con.executemany('INSERT INTO index_planned VALUES (?,?,?,?,?,?,?,?,?)',rows)
        realized,plans=S.calendar_bindings(con,[D(2021,5,28),D(2021,6,20)],through=D(2021,6,22))
        assert D(2021,6,21) not in realized
        assert D(2021,6,21) in plans[D(2021,5,28)]
        assert D(2021,6,21) not in plans[D(2021,6,20)]
        con.execute("UPDATE index_planned SET qualified=false WHERE vintage_id='revision'")
        with pytest.raises(ValueError,match='unqualified latest'):
            S.calendar_bindings(con,[D(2021,6,20)],through=D(2021,6,22))
        con.execute("UPDATE index_planned SET available_at=NULL WHERE vintage_id='revision'")
        with pytest.raises(ValueError,match='publication clock unresolved'):
            S.calendar_bindings(con,[D(2021,5,28)],through=D(2021,6,22))


def test_native_rank_projection_preserves_census_unlinked_and_latest_exclusions(tmp_path):
    """Connection-taking projection only; no native source admission is faked.

    Fixed public source_replay must still authenticate all originals/products.
    These typed source-shaped relations exercise arithmetic/vintage semantics,
    not source-economic or all85 qualification.
    """
    import duckdb
    import pyarrow as pa
    import pyarrow.parquet as pq
    from atx_db.alpha_panel import fundamentals as F, private_index_release as L
    rank=D(2021,5,7)
    known=dt.datetime(2021,5,6,20,tzinfo=UTC)
    price_clock=dt.datetime(2021,5,7,21,tzinfo=UTC)
    cut=dt.datetime(2021,5,7,22,tzinfo=UTC)
    event=tmp_path/'events.parquet';sic=tmp_path/'sic_events.parquet'
    pq.write_table(pa.Table.from_pylist([{'cik':123,'accession':'original123','form':'10-Q',
        'filed':D(2021,5,6),'clock_utc':known.replace(tzinfo=None),
        'available_at':known.replace(tzinfo=None),'period_end':D(2021,3,31),
        'staleness_days':180,'currency':'USD','sale_ttm':100.,'ni_q':5.,'ni_ttm':20.}],
        schema=F.EVENT_SCHEMA),event)
    pq.write_table(pa.Table.from_pylist([{'cik':123,'clock_utc':known.replace(tzinfo=None),
        'accession':'original123','sic':3571,'sic_basis':'fixture'}],schema=F.SIC_PART_SCHEMA),sic)
    foundation={'qualified_fundamental_leaf':tmp_path,'dependency_pins':{
        str(event.resolve()):L.pin(event),str(sic.resolve()):L.pin(sic)}}
    with duckdb.connect(str(tmp_path/'rank-phase.duckdb')) as con:
        con.execute("SET memory_limit='128MB'; SET threads=1; SET TimeZone='UTC'")
        con.execute('CREATE TABLE index_price(security_id BIGINT,session_date DATE,raw_close DOUBLE,'
            'is_index BOOLEAN,available_at TIMESTAMPTZ,qualified BOOLEAN,reason VARCHAR,source_sha256 VARCHAR)')
        con.execute('CREATE TABLE index_shares(security_id BIGINT,session_date DATE,shrout DOUBLE,'
            'available_at TIMESTAMPTZ,shrout_qualified BOOLEAN,reason VARCHAR,source_sha256 VARCHAR)')
        con.execute('CREATE TABLE index_finra(security_id BIGINT,observation_date DATE,issue_name VARCHAR,'
            'symbol VARCHAR,market_class VARCHAR,available_at TIMESTAMPTZ,mapping_qualified BOOLEAN,'
            'reason VARCHAR,source_sha256 VARCHAR)')
        con.execute('CREATE TABLE index_identity(security_id BIGINT,cik BIGINT,link_tier VARCHAR,'
            'identity_status VARCHAR,identity_cutoff TIMESTAMPTZ,valid_from DATE,valid_to DATE,'
            'available_at TIMESTAMPTZ,mapping_qualified BOOLEAN,reason VARCHAR,source_sha256 VARCHAR)')
        con.execute('CREATE TABLE index_liquidity(security_id BIGINT,session_date DATE,adv_63 DOUBLE,'
            'available_at TIMESTAMPTZ,qualified BOOLEAN,reason VARCHAR,source_sha256 VARCHAR)')
        con.execute('CREATE TABLE index_sec_regime(cik BIGINT,form VARCHAR,available_at TIMESTAMPTZ,'
            'source_sha256 VARCHAR,source_accession VARCHAR,filing_date DATE,qualified_clock BOOLEAN,reason VARCHAR)')
        con.execute('INSERT INTO index_sec_regime VALUES (?,?,?,?,?,?,?,?)',
            [123,'10-Q',known,'a'*64,'original123',D(2021,5,6),True,'[]'])
        for sid in range(1,7):
            con.execute('INSERT INTO index_price VALUES (?,?,?,?,?,?,?,?)',
                [sid,rank,10.,False,price_clock,True,'fixture','a'*64])
            con.execute('INSERT INTO index_shares VALUES (?,?,?,?,?,?,?)',
                [sid,rank,10e6,cut+dt.timedelta(days=1) if sid==6 else known,True,'fixture','a'*64])
            con.execute('INSERT INTO index_finra VALUES (?,?,?,?,?,?,?,?,?)',
                [sid,D(2021,5,6),'EXAMPLE ETF' if sid==4 else 'EXAMPLE INC COMMON STOCK',
                 'EXMP','SC',known,True,'fixture','a'*64])
            state='proven_unlinked' if sid==2 else 'linked'
            # The unlinked source shape retains a contaminated diagnostic CIK;
            # the projection must never use it or its financial values.
            tier='backfill' if sid in (2,3) else 'strict'
            identity_clock=cut if sid==2 else known
            con.execute('INSERT INTO index_identity VALUES (?,?,?,?,?,?,?,?,?,?,?)',
                [sid,999 if sid in (2,3) else 123,tier,state,cut,D(2021,1,1),None,
                 identity_clock,True,'phase-only full-census assertion','a'*64])
            if sid==5:
                con.execute('INSERT INTO index_identity VALUES (?,?,?,?,?,?,?,?,?,?,?)',
                    [sid,456,tier,state,cut,D(2021,1,1),None,identity_clock,True,'tie','b'*64])
            con.execute('INSERT INTO index_liquidity VALUES (?,?,?,?,?,?,?)',
                [sid,rank,1e6,price_clock,True,'fixture','a'*64])
        rows,census=S.rank_snapshot(con,foundation,rank)
        assert [(row['security_id'],row['cik']) for row in rows]==[(1,123),(2,None)]
        assert rows[0]['ni_q']==5. and rows[0]['ni_ttm']==20.
        assert rows[1]['ni_q'] is None and rows[1]['ni_ttm'] is None
        assert census['original_rank_price_lines']==6
        assert census['eligible_lines']==2 and census['excluded_lines']==4
        assert census['known_excluded_line_market_equity']==300e6
        assert census['backfill_cik']=={'selected_company_keys':0,'excluded_only_is_operating':0}
        assert not census['source_quality_certified']
        assert census['financial_source_economic_model_and_coverage_gates']=='PENDING'


def test_directory_and_every_derived_flag_are_ignored():
    p=facts()
    contaminated={key:True for key in ('directory_etf','is_adr','is_fpi','is_lp','is_royalty_trust','is_spac','is_index')}
    contaminated.update(security_type='ETF',is_operating=False,cik=999999)
    classified=S.classify_rank(D(2021,5,7),p,directory=contaminated)
    assert classified['qualified'] and classified['eligible']
    assert classified['cik']==123 and classified['market_equity']==1000
    assert classified['directory_decided']==0
    p['issue_name']=replace(p['issue_name'],value='EXAMPLE AMERICAN DEPOSITARY SHARES')
    rejected=S.classify_rank(D(2021,5,7),p,directory={'security_type':'common','is_adr':False,'is_operating':True})
    assert rejected['qualified'] and not rejected['eligible'] and rejected['is_adr']


def test_future_share_classification_dependency_is_excluded():
    p=facts()
    p['shrout']=replace(p['shrout'],dependency_clocks=(dt.datetime(2026,1,1,tzinfo=UTC),))
    row=S.classify_rank(D(2021,5,7),p)
    assert not row['qualified'] and not row['eligible']
    assert 'missing_or_ineligible_dated_shrout' in row['reason']
    p['shrout']=replace(p['shrout'],dependency_clocks=(),exclusions=('undated_issuer_line_count',))
    assert not S.classify_rank(D(2021,5,7),p)['qualified']


def test_post_rank_clock_and_missing_original_do_not_fall_back_to_directory():
    p=facts()
    p['form']=replace(p['form'],available_at=dt.datetime(2021,5,7,22,0,0,1,tzinfo=UTC))
    row=S.classify_rank(D(2021,5,7),p,directory={'is_fpi':False,'is_operating':True})
    assert not row['qualified'] and 'missing_or_ineligible_dated_form' in row['reason']
    p=facts();del p['sic']
    assert not S.classify_rank(D(2021,5,7),p,directory={'sic':3571})['qualified']


def test_rank_price_at_house_mark_is_known_but_future_is_not():
    p=facts()
    p['close']=replace(p['close'],available_at=dt.datetime(2021,5,7,22,tzinfo=UTC))
    assert S.classify_rank(D(2021,5,7),p)['eligible']
    p['close']=replace(p['close'],available_at=dt.datetime(2021,5,7,22,0,0,1,tzinfo=UTC))
    assert not S.classify_rank(D(2021,5,7),p)['qualified']


def test_latest_unsupported_amendment_and_equal_clock_ambiguity_never_fall_back():
    p=facts()
    original=p['sale_ttm']
    unsupported=replace(original,value=None,available_at=dt.datetime(2021,5,7,21,tzinfo=UTC),
                        exclusions=('missing_original_context',))
    population=list(p.items())+[('sale_ttm',unsupported)]
    selected=S.latest_facts(population,D(2021,5,7))
    assert selected['sale_ttm'] is unsupported
    assert not S.classify_rank(D(2021,5,7),selected)['qualified']
    selected=S.latest_facts(list(p.items())+[('sale_ttm',original)],D(2021,5,7))
    assert selected['sale_ttm'].value is None
    assert 'ambiguous_latest_dated_fact' in selected['sale_ttm'].exclusions


def test_sec_projection_includes_quarter_and_fund_with_original_clocks_only():
    import pytest
    block={'accessionNumber':['a','b','c','d'],'form':['10-Q','N-CSR','20-F','10-Q/A'],
           'filingDate':['2021-05-06']*4,'acceptanceDateTime':['2021-05-06T19:00:00Z']*4}
    doc={'cik':123,'filings':{'recent':block}}
    clock=dt.datetime(2021,5,6,23,tzinfo=UTC)
    rows=list(S.sec_regime_rows(doc,cik=123,source=0,source_sha256='a'*64,
         proved_clocks={(123,'a'):clock,(123,'b'):clock},owner_accessions={(123,'a'),(123,'b'),(123,'c')}))
    assert [r['form'] for r in rows]==['10-Q','N-CSR','20-F']
    assert rows[0]['qualified_clock'] and rows[1]['qualified_clock']
    assert rows[0]['form_fact'].period==D(2021,5,6)
    assert not rows[2]['qualified_clock'] and rows[2]['available_at'] is None
    assert 'missing_original_index_accepted_clock' in rows[2]['reason']
    block['form'].pop()
    with pytest.raises(ValueError,match='census array'):
        list(S.sec_regime_rows(doc,cik=123,source=0,source_sha256='a'*64,
             proved_clocks={},owner_accessions=set()))


def test_future_realized_calendar_cannot_decide_rank_effective_session():
    import pytest
    known=dt.datetime(2021,1,1,tzinfo=UTC)
    days=[D(2021,6,25),D(2021,6,28),D(2021,6,29)]
    planned=[S.DatedFact(day,known,'a'*64) for day in days]
    assert S.planned_sessions(planned,D(2021,5,7))==days
    later=[replace(fact,available_at=dt.datetime(2026,1,1,tzinfo=UTC)) for fact in planned]
    with pytest.raises(ValueError,match='unavailable by rank'):
        S.planned_sessions(later,D(2021,5,7))
