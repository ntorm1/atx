"""Synthetic original French/Census/SIC kernel phases, no source admission."""
import datetime as dt
import json
import hashlib
import duckdb
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import reference as R,classification as I
from atx_db.alpha_panel import private_tier1_reference_replay as T
from atx_db.alpha_panel import private_tier1_admission as A


def test_original_french_parser_values_fetched_clock_and_vintage_retained():
    text='using the 202601 CRSP database\n,Mkt-RF,SMB,HML,RMW,CMA,RF\n20200102,1,2,-99.99,4,5,.01\n20200103,2,3,4,5,6,.02\n\nAnnual Factors\n2020,99,99,99,99,99,99'
    fetched=dt.datetime(2026,2,1,12,tzinfo=dt.UTC)
    native=T.french_table(text,fetched,'ff5_daily')
    columns,rows,vintage=R.parse_french_table(text)
    assert native.num_rows==len(rows)==2
    assert native.to_pylist()[0]['hml'] is None
    assert native.to_pylist()[0]['rf']==rows[0][1][-1]
    assert all(row['vintage_risk'] and row['vintage']==vintage and row['available_at']==fetched.replace(tzinfo=None) for row in native.to_pylist())


def test_streamed_industry_full_native_pure_kernel_equivalence(tmp_path):
    fetched=dt.datetime(2030,1,1,tzinfo=dt.UTC)
    sources={scheme:(' 1 Test Synthetic industry\n  1000-1999 Original range\n 2 Other Other industry',fetched) for scheme in R.SICCODE_SCHEMES}
    siccodes=T.sic_table(sources).to_pylist()
    concordances={'sic87_naics02':[['SIC','','NAICS',''],[1001,'Industry',111111,'Title'],[2001,'Industry2',222222,'Title2']]}
    concordances.update({key:[['Old NAICS CODE','','New',''],[111111,'Title',111111,'Title'],[222222,'Title2',222222,'Title2']] for key in I.CHAIN})
    events=[]
    for cik in range(1,41):
        for index,sic in enumerate((1001,1001,2001,1001)):
            events.append((cik,dt.datetime(2020,1,1+index,tzinfo=dt.UTC),sic,str(cik)+'-'+str(index),'native'))
    rows02=I.concordance_rows(concordances['sic87_naics02']);runs=I.sic_runs(events)
    mapping=I.sic_map_rows([row[2] for row in runs],siccodes,I.primary_map(rows02),
        [I.primary_map(I.concordance_rows(concordances[key])) for key in I.CHAIN],I.candidates_map(rows02),{code:title for _,_,code,title in rows02})
    mapped={row['sic']:row for row in mapping}
    expected=[{'cik':cik,'available_at':clock,'sic':sic,'accession':accession,'sic_basis':basis,
        **{key:value for key,value in mapped[sic].items() if key!='sic'}} for cik,clock,sic,accession,basis in runs]
    with duckdb.connect(str(tmp_path/'classification.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        con.execute('CREATE TABLE source(cik BIGINT,clock_utc TIMESTAMPTZ,sic BIGINT,accession VARCHAR,sic_basis VARCHAR)')
        con.executemany('INSERT INTO source VALUES(?,?,?,?,?)',events)
        actual=list(T.industry_rows(con,'source',siccodes,concordances))
    assert len(actual)==120 and actual==expected
    assert all(row['naics_approx'] for row in actual)
    # Native producer clocks remain SIC clocks; the independent source-proof
    # layer must retain this future reference clock and refuse PIT qualification.
    assert all(row['available_at']<fetched for row in actual)


def test_full_original_french_receipts_products_and_stale_actual_bytes(tmp_path):
    root=tmp_path/'originals';root.mkdir();output=tmp_path/'products';output.mkdir()
    fetched=dt.datetime(2026,2,1,tzinfo=dt.UTC);records=[];products={};scheme_sources={}
    members=[]
    sources={name:key for key,name in R.FRENCH_FACTOR_FILES.items()}
    sources.update({f'Siccodes{scheme}.zip':scheme for scheme in R.SICCODE_SCHEMES})
    for name,key in sources.items():
        directory=root/name.removesuffix('.zip');directory.mkdir()
        if isinstance(key,int):text=' 1 Test Test industry\n  1000-1999 Range\n 2 Other Other industry'
        else:
            date='20200102' if key.endswith('daily') else '202001'
            text='using the 202601 CRSP database\n,Mkt-RF,SMB,HML,RMW,CMA,RF\n'+date+',1,2,3,4,5,.01\n'
        member=directory/'original.txt';member.write_bytes(text.encode('latin-1'));members.append(member)
        records.append({'key':name,'kind':'french_zip','url':R.FRENCH_BASE+name,'http_status':200,
            'file':directory.relative_to(root).as_posix(),'members':{'original.txt':A.pin(member)},
            'fetched_at':fetched.isoformat(),'zip_deleted':True,'bytes':123,
            'sha256':hashlib.sha256(('declared deleted original '+name).encode()).hexdigest()})
        if isinstance(key,int):scheme_sources[key]=(text,fetched)
        else:
            path=output/(key+'.parquet');pq.write_table(T.french_table(text,fetched,key),path);products[key+'.parquet']=path
    products['siccodes.parquet']=output/'siccodes.parquet';pq.write_table(T.sic_table(scheme_sources),products['siccodes.parquet'])
    ledger=root/'receipts.jsonl';ledger.write_text(''.join(json.dumps(record)+'\n' for record in records),encoding='utf-8')
    expected={str(ledger.resolve()):A.pin(ledger),
        **A.code(('private_tier1_reference_replay','private_tier1_factor_replay','private_tier1_admission','reference','classification','shortflow_common','common')),
        **{str(path.resolve()):A.pin(path) for path in [*members,*products.values()]}}
    with duckdb.connect(str(tmp_path/'original-replay.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        result=T.replay_french_sources(con,root,ledger,products,tmp_path/'expected',expected)
        assert result['status']=='full_original_native_replay' and len(result['validation'])==7
        assert len(result['sources'])==14 and not result['source_quality_certified']
        assert all(not source['original_zip_bytes_admitted'] for source in result['sources'].values())
        missing={key:value for key,value in expected.items() if key!=str(members[-1].resolve())}
        with pytest.raises(ValueError,match='complete independent French dependency inventory differs'):
            T.replay_french_sources(con,root,ledger,products,tmp_path/'missing',missing)
        members[0].write_bytes(b'changed original')
        with pytest.raises(ValueError,match='native dependency bytes changed'):
            T.replay_french_sources(con,root,ledger,products,tmp_path/'stale',expected)
