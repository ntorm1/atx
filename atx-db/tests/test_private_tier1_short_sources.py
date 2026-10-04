"""Source-shaped offline fixtures; no actual FINRA source certification."""
import ast
import csv
import datetime as dt
import gzip
import hashlib
import inspect
import json
from pathlib import Path

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import private_tier1_short_sources as H
from atx_db.alpha_panel import private_tier1_family_sources as A
from atx_db.alpha_panel import finra_fetch as F, short_interest as SI, short_volume as SV, short_volume_ext as E


def connection(path):
    con=duckdb.connect(str(path))
    con.execute("SET memory_limit='128MB'")
    con.execute('SET threads=1')
    con.execute("SET TimeZone='UTC'")
    return con


def write_json(path,value):
    path.write_text(json.dumps(value,sort_keys=True,default=str),encoding='utf-8')
    return path


def assignment(function,name,context):
    """Read-only native SQL expression oracle, not a producer/function clone."""
    tree=ast.parse(inspect.getsource(function))
    nodes=[node.value for node in ast.walk(tree) if isinstance(node,ast.Assign)
           and any(isinstance(target,ast.Name) and target.id==name for target in node.targets)]
    assert len(nodes)==1
    return eval(compile(ast.Expression(nodes[0]),'<original-native-sql-oracle>','eval'),context)


def test_repair_ticker_projection_native_sql_oracle(tmp_path):
    ticker=tmp_path/'ticker.parquet'
    dates=[dt.date(2021,1,4),dt.date(2021,1,5),dt.date(2021,1,6)]
    rows=[{'tradingDate':day,'securityID':sid,'ticker_tk':symbol} for day,sid,symbol in
          [(dates[0],7,'ABC'),(dates[1],0,'ABC'),(dates[2],7,'ABC'),
           (dates[0],8,'AMB'),(dates[0],9,'AMB'),(dates[1],0,'AMB'),(dates[2],8,'AMB'),
           (dates[1],0,'MISSING'),(dates[1],0,'DUPE'),(dates[1],0,'DUPE')]]
    pq.write_table(pa.Table.from_pylist(rows),ticker)
    con=connection(tmp_path/'projection.duckdb')
    repair=tmp_path/'repair.parquet'
    H.repair_sql(con,ticker,repair)
    tree=ast.parse(inspect.getsource(F.sid0_repairs))
    context={'con':con,'src':ticker.as_posix(),'lo':F.SID0_START-dt.timedelta(days=F.SID0_MAX_BRACKET_DAYS),
             'SID0_START':F.SID0_START,'SID0_MAX_BRACKET_DAYS':F.SID0_MAX_BRACKET_DAYS}
    calls=[node for node in ast.walk(tree) if isinstance(node,ast.Call)
           and isinstance(node.func,ast.Attribute) and isinstance(node.func.value,ast.Name)
           and node.func.value.id=='con' and node.func.attr=='execute']
    for call in calls[:4]:
        sql=eval(compile(ast.Expression(call.args[0]),'<original-repair-sql>','eval'),context)
        con.execute(sql)
    native=assignment(F.sid0_repairs,'sql',context)
    H._compare(con,native,repair)
    for table in ('z','nz','zb','own'):
        con.execute('DROP TABLE '+table)
    projected=tmp_path/'extracted.parquet'
    query=H.ticker_sql(ticker,repair,set(dates))
    H.C.copy_to_parquet(con,query,projected)
    native=assignment(F.ticker_extract,'sql',{'C':type('Source',(),{'TICKERHISTORY':ticker}),
        'rep':repair,'lst':', '.join(f"DATE '{d}'" for d in dates)})
    H._compare(con,native,projected)
    states=dict(con.execute(f"SELECT ticker_tk,basis FROM read_parquet('{repair.as_posix()}')").fetchall())
    assert states=={'ABC':'bracketed','AMB':'neighbour_ambiguous','MISSING':'ticker_never_nonzero','DUPE':'several_sid0_rows'}
    con.close()


def test_schedule_and_interest_parser_original_equivalence(tmp_path):
    import pyarrow.csv as pc
    date=dt.date(2021,1,6)
    raw=tmp_path/'si_20210106.csv'
    fields=dict.fromkeys(SI.RAW_COLUMNS,'')
    fields.update(symbolCode=' ABC ',marketClassCode='N',settlementDate=str(date),currentShortPositionQuantity='25')
    body=('|'.join(SI.RAW_COLUMNS)+'\n'+'|'.join(fields.values())+'\n'+'bad|row\n').encode()
    raw.write_bytes(body)
    source={'path':raw,'gz':False,'sha256':hashlib.sha256(body).hexdigest(),'file':raw.name,'landing':0}
    target=tmp_path/'parsed.parquet'
    stats=H.parse_interest([source],target)
    skipped=[]
    original=pc.read_csv(pa.BufferReader(body),read_options=pc.ReadOptions(block_size=1<<23),
        parse_options=pc.ParseOptions(delimiter='|',quote_char=False,
            invalid_row_handler=lambda row: skipped.append(row) or 'skip'),
        convert_options=pc.ConvertOptions(column_types={name:pa.string() for name in SI.RAW_COLUMNS},
            include_columns=SI.RAW_COLUMNS,strings_can_be_null=False))
    original=original.append_column('line_no',pa.array([0],pa.int32()))
    original=original.append_column('source_file',pa.array([raw.name],pa.string()))
    original=original.append_column('landing',pa.array([0],pa.int8()))
    assert pq.ParquetFile(target).read().equals(original)
    assert stats['invalid_rows_skipped']==len(skipped)==1
    sessions=[dt.date(2021,1,day) for day in (4,5,6,7,8,11,12,13,14,15,19)]
    schedule={date:(dt.date(2021,1,8),'official')}
    rows=H.settlement_rows([date,dt.date(2021,1,7)],sessions,schedule)
    assert rows[0]==(date,dt.date(2021,1,8),'official',date)
    assert rows[1][1]==SI.add_business_days(dt.date(2021,1,7),SI.RULE_BUSINESS_DAYS,sessions)
    assert rows[1][2]=='rule-7bd'


def ext_fixture(tmp_path):
    date=dt.date(2021,1,6)
    root=tmp_path/'root';root.mkdir()
    native=root/'native';native.mkdir()
    rawroot=root/'raw';rawroot.mkdir()
    ticker=root/'ticker.parquet'
    rows=[{'tradingDate':dt.date(2021,1,d),'securityID':sid,'ticker_tk':('ABC' if sid==1 else f'T{sid}')}
          for d in (4,5,6) for sid in range(1,1001)]
    pq.write_table(pa.Table.from_pylist(rows),ticker)
    body=b'Date|Symbol|ShortVolume|ShortExemptVolume|TotalVolume|Market\n20210106|ABC|10|2|100|B,Q,X\n20210106|UNKNOWN|3|0|20|D\n'
    raw=rawroot/SV._file_name(date);raw.write_bytes(gzip.compress(body,mtime=0))
    sha=hashlib.sha256(body).hexdigest()
    manifest=root/'raw-manifest.csv'
    with manifest.open('w',newline='',encoding='utf-8') as stream:
        writer=csv.DictWriter(stream,fieldnames=['date','file','http_status','sha256_of_raw_bytes','rows','url'])
        writer.writeheader();writer.writerow({'date':str(date),'file':raw.name,'http_status':200,
            'sha256_of_raw_bytes':sha,'rows':2,'url':SV.URL.format(d=date)})
    ticker_manifest=write_json(root/'ticker-manifest.json',{'tickerhistory':H.C.file_identity(ticker)})
    rawreceipt=write_json(root/'raw-receipt.json',{'http_status':200,'url':SV.URL.format(d=date),
        'sha256_of_raw_bytes':sha,'downloaded_at':'2021-01-06T20:00:00Z'})
    ticker_url='https://vendor.example.test/TickerHistory3'
    tickerreceipt=write_json(root/'ticker-receipt.json',{'http_status':200,'url':ticker_url,
        'sha256_of_raw_bytes':A.pin(ticker)['sha256'],'downloaded_at':'2021-01-06T20:00:00Z'})
    clockpath=write_json(root/'clock.json',{'schema':'atx.finra-short-original-observations/v1',
        'files':{str(raw.resolve()):str(rawreceipt)},'ticker_receipt':str(tickerreceipt),'schedule_receipt':None})
    con=connection(root/'fixture.duckdb')
    parsed=root/'fixture-parsed.parquet'
    H.parse_volume([{'date':str(date),'path':raw,'sha256':sha,'rows':2}],parsed)
    primary=root/'fixture-primary.parquet'
    H.C.copy_to_parquet(con,f"""SELECT tradingDate AS map_date,securityID AS security_id,ticker_tk,false AS sid_repaired
        FROM read_parquet('{ticker.as_posix()}') WHERE tradingDate=DATE '{date}'""",primary)
    mapped=root/'fixture-mapped.parquet';SV.map_year(con,parsed,primary,mapped)
    key='year=2021/short_volume_ext.parquet';dest=native/key;dest.parent.mkdir()
    tree=ast.parse(inspect.getsource(E.build))
    expression=next(node.args[1] for node in ast.walk(tree) if isinstance(node,ast.Call)
        and isinstance(node.func,ast.Attribute) and node.func.attr=='copy_to_parquet')
    query=eval(compile(ast.Expression(expression),'<original-ext-publication-sql>','eval'),{'m':mapped.as_posix()})
    H.C.copy_to_parquet(con,query,dest)
    native_manifest=write_json(native/'manifest.json',{'schema':E.SCHEMA,'status':'complete','stage':E.STAGE,
        'map_rule':SV.RULE,'clock_rule':E.CLOCK_RULE,'files':{key:A.pin(dest)},
        'receipt':{'tickerhistory':H.C.file_identity(ticker)},
        'code':{name+'.py':{'sha256':A.pin(Path(E.__file__).parent/(name+'.py'))['sha256']} for name in E.MODULES}})
    plan={'schema':'atx.tier1-short-source-plan/v1','family':'short_volume_ext','ticker_history':str(ticker),
        'ticker_manifest':str(ticker_manifest),'ticker_source_url':ticker_url,'clock_inventory':str(clockpath),
        'source_manifest':str(manifest),'raw_root':str(rawroot),'start':str(date),'end':str(date),
        'expected_dates':[str(date)],'expected_raw_files':1,'expected_selected_files':1,'expected_sessions':3,
        'native_products':{key:A.pin(dest)},'expected_native_rows':{key:2}}
    planpath=write_json(root/'plan.json',plan)
    reviewpath=root/'review.json'
    spec={'interface_version':H.VERSION,'root':str(root),'generation':str(native),'manifest_pin':A.pin(native_manifest),
        'native':{'plan_path':str(planpath),'expected_plan_sha256':A.pin(planpath)['sha256']},
        'independent_review':{'path':str(reviewpath)},'mapped_generation':str(root/'private-map')}
    code=H.code_pins()
    write_json(reviewpath,{'schema':'atx.tier1-short-review/v1','status':'scoped_pass','interface_version':H.VERSION,
        'family':'short_volume_ext','binding_sha256':A.digest(H._binding('short_volume_ext',spec)),'code_pins':code})
    spec['independent_review']['expected_sha256']=A.pin(reviewpath)['sha256']
    paths=(planpath,reviewpath,native_manifest,ticker,ticker_manifest,clockpath,manifest,raw,rawreceipt,tickerreceipt,dest)
    spec['dependency_pins']={**code,**{str(p.resolve()):A.pin(p) for p in paths}}
    return con,spec,raw,clockpath,tickerreceipt


def test_full_ext_replay_positive_exclusions_public_pin_drift(tmp_path):
    con,spec,raw,_,_=ext_fixture(tmp_path)
    _,_,validation,_=H._run('short_volume_ext',spec,con)
    spec['expected_validation_sha256']=A.digest(validation)
    publication=H.materialize('short_volume_ext',spec,con)
    spec.update(mapped_manifest_pin=publication['manifest_pin'],mapped_product_pins=publication['product_pins'])
    admission=H.admit('short_volume_ext',spec,con)
    rows=list(H.iter_rows(admission,'year=2021/short_volume_ext_cells.parquet',con))
    supported=next(row for row in rows if row['symbol']=='ABC')
    missing=next(row for row in rows if row['symbol']=='UNKNOWN')
    assert supported['qualified'] and supported['vintage_risk'] is True
    assert supported['available_at']==dt.datetime(2021,1,7,tzinfo=dt.timezone.utc)
    assert supported['fac_b'] and supported['fac_q'] and supported['fac_other']=='X'
    assert not missing['qualified'] and missing['security_id'] is None
    assert admission['source_quality_certified'] is False
    raw.write_bytes(raw.read_bytes()+b'drift')
    with pytest.raises(ValueError):
        list(H.iter_rows(admission,'year=2021/short_volume_ext_cells.parquet',con))
    con.close()


def test_later_capture_and_missing_clock_remain_unqualified(tmp_path):
    con,spec,_,clockpath,tickerreceipt=ext_fixture(tmp_path)
    plan=H._json(spec['native']['plan_path']);clock=H._json(clockpath)
    row={'security_id':1,'trade_date':dt.date(2021,1,6),'sid_repaired':False,
         'short_volume':10.,'short_exempt_volume':2.,'total_volume':100.}
    source={'date':'2021-01-06','path':Path(next(iter(clock['files']))),'sha256':H._json(next(iter(clock['files'].values())))['sha256_of_raw_bytes'],
            'url':SV.URL.format(d=row['trade_date'])}
    record=H._json(tickerreceipt);record['downloaded_at']='2026-09-01T00:00:00Z';write_json(tickerreceipt,record)
    assert H._qualification('short_volume_ext',row,plan,clock,[source],[],{})[1:] == (False,'ticker_later_or_unresolved_original_vintage')
    clock['files'][str(source['path'])]=None
    assert H._qualification('short_volume_ext',row,plan,clock,[source],[],{})[1:] == (False,'missing_original_contemporaneous_receipt')
    con.close()


def test_full_interest_replay_fallback_positive_native_clock(tmp_path):
    root=tmp_path/'root';root.mkdir()
    native=root/'native';native.mkdir()
    rawroot=root/'old-raw';rawroot.mkdir()
    date=dt.date(2021,1,6)
    ticker=root/'ticker.parquet'
    vendor=[{'tradingDate':dt.date(2021,1,d),'securityID':sid,'ticker_tk':'ABC' if sid==1 else f'T{sid}'}
            for d in (4,5,6,7,8) for sid in range(1,1001)]
    vendor.append({'tradingDate':dt.date(2021,1,5),'securityID':2001,'ticker_tk':'FALL'})
    pq.write_table(pa.Table.from_pylist(vendor),ticker)
    ticker_manifest=write_json(root/'ticker-manifest.json',{'tickerhistory':H.C.file_identity(ticker)})
    raw=rawroot/'si_20210106.csv'
    values=[]
    for symbol,market,shares in [('ABC','N','25'),('ABC','OTC','500'),('FALL','N','10')]:
        row=dict.fromkeys(SI.RAW_COLUMNS,'')
        row.update(accountingYearMonthNumber='20210106',symbolCode=symbol,marketClassCode=market,
            settlementDate=str(date),currentShortPositionQuantity=shares,previousShortPositionQuantity='5',
            averageDailyVolumeQuantity='0',daysToCoverQuantity='999.99')
        values.append('|'.join(row.values()))
    body=('|'.join(SI.RAW_COLUMNS)+'\n'+'\n'.join(values)+'\n').encode();raw.write_bytes(body)
    sha=hashlib.sha256(body).hexdigest()
    old=root/'old.csv'
    with old.open('w',newline='',encoding='utf-8') as stream:
        writer=csv.DictWriter(stream,fieldnames=['settlement_date','file','sha256'])
        writer.writeheader();writer.writerow({'settlement_date':str(date),'file':raw.name,'sha256':sha})
    new=root/'new.csv';new.write_text('date,file,http_status\n')
    schedule=root/'schedule.csv';schedule.write_text('settlement_date,dissemination_date,source\n2021-01-06,2021-01-07,official\n')
    source={'date':str(date),'path':raw,'file':raw.name,'sha256':sha,'gz':False,'landing':0}
    rawreceipt=write_json(root/'raw-receipt.json',{'http_status':200,'url':SI.FILE_URL.format(d=date),
        'sha256_of_raw_bytes':sha,'downloaded_at':'2021-01-07T20:00:00Z'})
    ticker_url='https://vendor.example.test/TickerHistory3'
    tickerreceipt=write_json(root/'ticker-receipt.json',{'http_status':200,'url':ticker_url,
        'sha256_of_raw_bytes':A.pin(ticker)['sha256'],'downloaded_at':'2021-01-08T00:00:00Z'})
    schedule_url='https://www.finra.org/filing-reporting/regulatory-filing-systems/short-interest'
    schedule_receipt=write_json(root/'schedule-receipt.json',{'http_status':200,'url':schedule_url,
        'sha256_of_raw_bytes':A.pin(schedule)['sha256'],'downloaded_at':'2021-01-01T00:00:00Z'})
    clockpath=write_json(root/'clock.json',{'schema':'atx.finra-short-original-observations/v1',
        'files':{str(raw):str(rawreceipt)},'ticker_receipt':str(tickerreceipt),'schedule_receipt':str(schedule_receipt)})
    con=connection(root/'fixture.duckdb')
    parsed=root/'parsed.parquet';H.parse_interest([source],parsed)
    rows=[(date,dt.date(2021,1,7),'official',date)]
    sessions=[dt.date(2021,1,d) for d in (4,5,6,7,8)]
    con.execute('CREATE TEMP TABLE settle(settlement_date DATE,dissemination_date DATE,dissemination_source VARCHAR,map_date DATE)')
    con.executemany('INSERT INTO settle VALUES (?,?,?,?)',rows)
    primary=root/'primary.parquet'
    con.execute(f"""COPY(SELECT tradingDate AS map_date,securityID AS security_id,ticker_tk,false AS sid_repaired
        FROM read_parquet('{ticker.as_posix()}') WHERE tradingDate=DATE '{date}')
        TO '{primary.as_posix()}'(FORMAT PARQUET)""")
    repair=root/'repair.parquet';H.repair_sql(con,ticker,repair)
    asof=root/'asof.parquet'
    H.C.copy_to_parquet(con,H.asof_sql(con,parsed,primary,ticker,repair,rows,sessions),asof)
    query=assignment(SI.build_asof,'sql',{'th_primary':primary,'pglob':parsed.as_posix(),
        'C':type('Original',(),{'TICKERHISTORY':ticker}),'rep':repair,
        'ck_t':F.CANON_SQL.format(x='t.ticker_tk'),'ck_r':F.CANON_SQL.format(x='r.ticker_tk'),'F':F})
    H._compare(con,query,asof)
    con.execute(SI._stage_sql(parsed.as_posix(),asof.as_posix()))
    audit=native/'mapping_audit.parquet';H.C.copy_to_parquet(con,'SELECT * FROM audit ORDER BY settlement_date,symbol',audit)
    si=native/'si.parquet';H.C.copy_to_parquet(con,assignment(SI.build,'sql',{}),si)
    for table in ('audit','settle','dmap'):
        con.execute('DROP TABLE '+table)
    native_manifest=write_json(native/'manifest.json',{'rule':SI.RULE,'tickerhistory':H.C.file_identity(ticker),
        'sources':{'files_sha256':{raw.name:sha}}})
    products={'si.parquet':A.pin(si),'mapping_audit.parquet':A.pin(audit)}
    plan={'schema':'atx.tier1-short-source-plan/v1','family':'short_interest','ticker_history':str(ticker),
        'ticker_manifest':str(ticker_manifest),'ticker_source_url':ticker_url,'clock_inventory':str(clockpath),
        'old_manifest':str(old),'new_manifest':str(new),'old_raw_root':str(rawroot),'new_raw_root':str(root),
        'schedule':str(schedule),'schedule_source_url':schedule_url,'expected_raw_files':1,'expected_selected_files':1,
        'expected_sessions':5,'native_products':products,'expected_native_rows':{'si.parquet':2,'mapping_audit.parquet':2}}
    planpath=write_json(root/'plan.json',plan);reviewpath=root/'review.json'
    spec={'interface_version':H.VERSION,'root':str(root),'generation':str(native),'manifest_pin':A.pin(native_manifest),
        'native':{'plan_path':str(planpath),'expected_plan_sha256':A.pin(planpath)['sha256']},
        'independent_review':{'path':str(reviewpath)},'mapped_generation':str(root/'private-map')}
    code=H.code_pins()
    write_json(reviewpath,{'schema':'atx.tier1-short-review/v1','status':'scoped_pass','interface_version':H.VERSION,
        'family':'short_interest','binding_sha256':A.digest(H._binding('short_interest',spec)),'code_pins':code})
    spec['independent_review']['expected_sha256']=A.pin(reviewpath)['sha256']
    paths=(planpath,reviewpath,native_manifest,ticker,ticker_manifest,clockpath,old,new,schedule,raw,
           rawreceipt,tickerreceipt,schedule_receipt,si,audit)
    spec['dependency_pins']={**code,**{str(path):A.pin(path) for path in paths}}
    _,_,validation,_=H._run('short_interest',spec,con)
    spec['expected_validation_sha256']=A.digest(validation)
    publication=H.materialize('short_interest',spec,con)
    spec.update(mapped_manifest_pin=publication['manifest_pin'],mapped_product_pins=publication['product_pins'])
    admission=H.admit('short_interest',spec,con)
    actual=list(H.iter_rows(admission,'short_interest_cells.parquet',con))
    assert all(row['qualified'] for row in actual)
    abc=next(row for row in actual if row['symbol']=='ABC')
    fallback=next(row for row in actual if row['symbol']=='FALL')
    assert abc['si_shares']==25 and abc['si_dtc'] is None
    assert fallback['vendor_date_fallback'] and fallback['vendor_date']==dt.date(2021,1,5)
    assert abc['available_at']==dt.datetime(2021,1,8,tzinfo=dt.timezone.utc)
    assert abc['native_first_visible_session']==dt.date(2021,1,8)
    con.close()
