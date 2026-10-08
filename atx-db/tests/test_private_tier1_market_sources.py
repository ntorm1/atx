"""Bounded original-format fixtures; these do not qualify actual market sources."""
import datetime as dt
import hashlib
import json
import ast
import inspect

import pytest

from atx_db.alpha_panel import private_tier1_market_sources as H


def _sec_share_original(tmp_path,*,context_namespace='http://www.xbrl.org/2003/instance'):
    path=tmp_path/'original-issuer-body.htm'
    path.write_text('''<html xmlns:ix="http://www.xbrl.org/2013/inlineXBRL"
        xmlns:xbrli="'''+context_namespace+'''" xmlns:dei="http://xbrl.sec.gov/dei/2020-01-31"
        xmlns:ixt="http://www.xbrl.org/inlineXBRL/transformation/2015-02-26">
        <xbrli:context id="cover"><xbrli:entity><xbrli:identifier scheme="http://www.sec.gov/CIK">123</xbrli:identifier>
        </xbrli:entity><xbrli:period><xbrli:instant>2020-01-02</xbrli:instant></xbrli:period></xbrli:context>
        <xbrli:unit id="shares"><xbrli:measure>xbrli:shares</xbrli:measure></xbrli:unit>
        <ix:nonFraction name="dei:EntityCommonStockSharesOutstanding" contextRef="cover"
        unitRef="shares" scale="3" format="ixt:numcommadot">1,234</ix:nonFraction>
        <ix:nonNumeric name="dei:TradingSymbol" contextRef="cover">EXAMPLE</ix:nonNumeric></html>''',encoding='utf-8')
    return path


def test_original_sec_inline_primitives_preserve_count_context_and_no_admission(tmp_path):
    from decimal import Decimal
    rows=H.original_sec_ixbrl_primitives(_sec_share_original(tmp_path),123)
    assert len(rows)==2
    shares=rows[0]
    assert shares['shares']==Decimal('1234000')
    assert shares['asof_date']==dt.date(2020,1,2)
    assert shares['primitive_reason']=='parsed_original_dei_primitive'
    assert rows[1]['raw_value']=='EXAMPLE'
    assert all('available_at' not in row and 'source_qualified' not in row for row in rows)


def test_original_sec_inline_primitives_refuse_custom_instance_namespace(tmp_path):
    with pytest.raises(ValueError,match='instance primitive namespace'):
        H.original_sec_ixbrl_primitives(_sec_share_original(tmp_path,context_namespace='https://example.invalid/instance'),123)


def test_original_sec_inline_primitives_preserve_wrong_owner_exclusion(tmp_path):
    rows=H.original_sec_ixbrl_primitives(_sec_share_original(tmp_path),456)
    assert all('original_context_issuer_mismatch' in row['primitive_reason'] for row in rows)


def test_original_sec_inline_primitives_ignore_excluded_display_text(tmp_path):
    path=_sec_share_original(tmp_path)
    path.write_text(path.read_text(encoding='utf-8').replace('>1,234</ix:nonFraction>',
        '>1,<ix:exclude>99</ix:exclude>234</ix:nonFraction>'),encoding='utf-8')
    assert H.original_sec_ixbrl_primitives(path,123)[0]['shares']==1234000


def test_original_sec_inline_primitives_do_not_prove_partial_continued_symbols(tmp_path):
    path=_sec_share_original(tmp_path)
    path.write_text(path.read_text(encoding='utf-8').replace('name="dei:TradingSymbol"',
        'name="dei:TradingSymbol" continuedAt="unseen-continuation"'),encoding='utf-8')
    assert H.original_sec_ixbrl_primitives(path,123)[1]['primitive_reason']=='original_inline_continuation_unproved'


def test_original_sec_inline_decoded_ledger_bytes_preserve_raw_and_storage_identities(tmp_path):
    import gzip
    plain=_sec_share_original(tmp_path)
    original=plain.read_bytes()
    stored=tmp_path/'original-ledger-body.gz'
    stored.write_bytes(gzip.compress(original,mtime=0))
    normal=H.original_sec_ixbrl_primitives(plain,123)
    ledger=H.original_sec_ixbrl_primitives(stored,123,original_bytes=original)
    assert len(normal)==len(ledger)==2
    for expected,actual in zip(normal,ledger,strict=True):
        assert actual['source_sha256']==hashlib.sha256(original).hexdigest()
        assert actual['source_storage_identity']==H.A.pin(stored)
        assert actual['source_storage_identity']['sha256']!=actual['source_sha256']
        assert actual['source_document']==str(stored)
        assert {k:v for k,v in expected.items() if k not in ('source_document','source_storage_identity')}=={
            k:v for k,v in actual.items() if k not in ('source_document','source_storage_identity')}
    with pytest.raises(ValueError,match='decoded SEC bytes do not match retained storage'):
        H.original_sec_ixbrl_primitives(stored,123,original_bytes=original+b'<p>Unrelated replacement</p>')


def test_asof_share_fitting_excludes_future_classes_links_and_matching_documents(tmp_path):
    import duckdb
    con=duckdb.connect(str(tmp_path/'causal.duckdb'),config={'threads':1,'memory_limit':'128MB'})
    con.execute("SET TimeZone='UTC'")
    try:
        con.execute('''CREATE TABLE names(security_id BIGINT,finra_type VARCHAR,
            observation_date DATE,source_available_at TIMESTAMPTZ)''')
        con.execute("""INSERT INTO names VALUES (1,'Common Stock','2020-01-01','2020-01-01T20:00:00+00:00'),
            (1,'ADR','2020-01-03','2020-01-03T20:00:00+00:00'),
            (1,'ADR','2020-01-04','2020-01-04T20:00:00+00:00'),
            (2,'Common Stock','2020-01-01','2020-01-01T20:00:00+00:00'),
            (2,'ADR','2020-01-01','2020-01-01T20:00:00+00:00')""")
        con.execute('''CREATE TABLE links(security_id BIGINT,cik BIGINT,valid_from DATE,
            valid_to DATE,link_tier VARCHAR,available_at TIMESTAMPTZ,evidence_at TIMESTAMPTZ,
            source_available_at TIMESTAMPTZ)''')
        con.execute("""INSERT INTO links VALUES
            (1,123,'2020-01-01','2020-12-31','strict','2020-01-01T20:00:00+00:00',NULL,'2020-01-01T20:00:00+00:00'),
            (2,123,'2020-01-01','2020-12-31','name','2020-01-01T20:00:00+00:00',NULL,'2020-01-01T20:00:00+00:00'),
            (3,123,'2020-01-01','2020-12-31','backfill','2020-01-01T20:00:00+00:00','2020-01-01T20:00:00+00:00','2020-01-01T20:00:00+00:00'),
            (4,123,'2020-01-01','2020-12-31','strict','2020-01-01T20:00:00+00:00',NULL,'2020-01-03T20:00:00+00:00')""")
        con.execute('''CREATE TABLE events(cik BIGINT,clock_utc TIMESTAMPTZ,filed DATE,
            shrs_q DOUBLE,shrs_src VARCHAR,source_available_at TIMESTAMPTZ)''')
        con.execute("""INSERT INTO events VALUES
            (123,'2020-01-01T20:00:00+00:00','2020-01-01',100,'dei','2020-01-01T20:00:00+00:00'),
            (123,'2020-01-03T20:00:00+00:00','2020-01-03',200,'dei','2020-01-03T20:00:00+00:00'),
            (123,'2020-01-01T20:00:00+00:00','2020-01-01',300,'dei','2020-01-03T20:00:00+00:00')""")
        snapshot=H._asof_share_inputs(con,links='links',names='names',events='events',
            cutoff='2020-01-02T22:00:00+00:00',namespace='t17_market_asof_test')
        assert con.execute('SELECT line_type FROM '+snapshot['line_type']+' WHERE security_id=1').fetchone()==('Common Stock',)
        assert con.execute('SELECT security_id FROM '+snapshot['links']+' ORDER BY 1').fetchall()==[(1,),(2,)]
        assert con.execute('SELECT shrs_q FROM '+snapshot['sec']).fetchall()==[(100.0,)]
        assert con.execute('SELECT security_id,reason FROM '+snapshot['unresolved_lines']).fetchall()==[(2,'ambiguous_dated_classification')]
    finally:
        con.close()


def _native_sql(function,locals_):
    """Evaluate the unchanged producer's query expression as independent oracle.

    Never invoke its global filesystem entrypoint or replace producer globals.
    """
    tree=ast.parse(inspect.getsource(function))
    expressions=[n.value for n in ast.walk(tree) if isinstance(n,ast.Assign) and
                 any(isinstance(t,ast.Name) and t.id=='sql' for t in n.targets)]
    assert len(expressions)==1
    expression=ast.Expression(expressions[0]);ast.fix_missing_locations(expression)
    return eval(compile(expression,'<unchanged-native-query-oracle>','eval'),function.__globals__,locals_)


def test_calendar_and_project_match_complete_typed_native_queries(tmp_path):
    import duckdb
    import pyarrow as pa
    import pyarrow.parquet as pq
    con=duckdb.connect(str(tmp_path/'oracle.duckdb'),config={'threads':1,'memory_limit':'128MB'})
    con.execute("SET TimeZone='UTC'")
    try:
        day=dt.date(2020,1,2)
        rows=[]
        for i in range(1001):
            row={'tradingDate':day,'securityID':i%17,'ticker_tk':'X'+str(i%17),
                 'open':10.,'high':11.,'low':9.,'close':10.,'closeUnadjPr':9.9,
                 'returnFactor':1.,'totalReturn':.01,'cumulReturnFactor':1.,
                 'volume':1000,'shares':20,'earnFlag':False,'GICS':'10'}
            row.update({f'atmCenI_{t}d':.2 for t in H.P.IV_TENORS})
            rows.append(row)
        # Full population includes a duplicate, sparse non-session and NULL SID.
        rows.extend([dict(rows[0],tradingDate=dt.date(2020,1,3)),dict(rows[0],securityID=None)])
        source=tmp_path/'original.parquet';pq.write_table(pa.Table.from_pylist(rows),source)
        native_calendar=_native_sql(H.P.build_calendar,{'src':source.as_posix()})
        expected=f'SELECT session_date,vendor_rows,vendor_ids FROM ({native_calendar}) WHERE is_session'
        actual=H.calendar_query(H._sql(source))
        con.execute('CREATE TEMP TABLE oracle_calendar AS '+expected)
        con.execute('CREATE TEMP TABLE replay_calendar AS '+actual)
        assert con.execute('DESCRIBE oracle_calendar').fetchall()==con.execute('DESCRIBE replay_calendar').fetchall()
        assert con.execute('SELECT * FROM oracle_calendar EXCEPT ALL SELECT * FROM replay_calendar').fetchall()==[]
        assert con.execute('SELECT * FROM replay_calendar EXCEPT ALL SELECT * FROM oracle_calendar').fetchall()==[]
        expected=_native_sql(H.P.project,{'src':source.as_posix(),
                  'iv_cols':', '.join(f'atmCenI_{t}d AS iv_raw_{t}d' for t in H.P.IV_TENORS)})
        actual=H.project_query(H._sql(source),H.P.PROJECT_START)
        con.execute('CREATE TEMP TABLE oracle_project AS '+expected)
        con.execute('CREATE TEMP TABLE replay_project AS '+actual)
        assert con.execute('DESCRIBE oracle_project').fetchall()==con.execute('DESCRIBE replay_project').fetchall()
        assert con.execute('SELECT * FROM oracle_project EXCEPT ALL SELECT * FROM replay_project').fetchall()==[]
        assert con.execute('SELECT * FROM replay_project EXCEPT ALL SELECT * FROM oracle_project').fetchall()==[]
    finally:con.close()


@pytest.mark.parametrize('case',['valid','null_sid_ambiguity','string_sid','duplicate','nonfinite'])
def test_point_projection_matches_unchanged_typed_native_reader(tmp_path,case):
    # Only the point projection is exercised here; manually constructing its
    # private reader is not public scope/admission or an original-source proof.
    import duckdb
    import pyarrow as pa
    import pyarrow.parquet as pq
    from atx_db.alpha_panel import mna_price_native_v2 as MP
    day=dt.date(2020,1,2);clock=dt.datetime(2020,1,3,tzinfo=dt.timezone.utc)
    statements=('Example common stock trades as EX.','Prices are USD per share.',
                'close is the unadjusted raw traded close.',
                'conversion terms are unchanged with no split or FX conversion.',
                'Effective from 2019-01-01 through 2021-12-31.')
    blob=' '.join(statements).encode()
    ep=MP.EconomicPin('economics',hashlib.sha256(blob).hexdigest(),'Example','EX','common stock',
        'USD','share','raw-USD-common-share',dt.date(2019,1,1),dt.date(2021,12,31),clock,
        *statements,'retrieval_floor',clock)
    rows=[{'securityID':1,'tradingDate':day,'ticker_tk':'EX','close':10.25}]
    if case=='null_sid_ambiguity':rows.append(dict(rows[0],securityID=None))
    elif case=='string_sid':rows=[dict(rows[0],securityID='1')]
    elif case=='duplicate':rows.append(dict(rows[0]))
    elif case=='nonfinite':rows=[dict(rows[0],close=float('nan'))]
    path=tmp_path/'vendor.parquet';pq.write_table(pa.Table.from_pylist(rows),path)
    schema=str(pq.read_schema(path));pin=H.A.pin(path)
    vp=MP.VendorPricePin('vendor',pin['sha256'],schema,1,day,'EX',clock,'economics')
    con=duckdb.connect(str(tmp_path/'point.duckdb'),config={'threads':1,'memory_limit':'128MB'})
    con.execute("SET TimeZone='UTC'")
    try:
        con.execute('CREATE TEMP TABLE fixture_vendor AS SELECT * FROM '+H._sql(path))
        reader=H._VendorPointReader(con,{str(path):{'identity':pin,'expected_schema':schema}},
                                    {str(path):('fixture_vendor',True)})
        if case=='valid':
            assert reader.point(path,vp,blob,ep,'oracle')==MP.read_vendor_price(path,vp,blob,ep,'oracle')
        else:
            with pytest.raises(ValueError):MP.read_vendor_price(path,vp,blob,ep,'oracle')
            with pytest.raises(ValueError):reader.point(path,vp,blob,ep,'oracle')
    finally:con.close()


def _distributed(tmp_path,label='Dec 28, 2020 3:00pm EST'):
    url=('https://www.nasdaq.com/press-release/nyse-group-announces-'
         '2021-2022-and-2023-holiday-and-early-closings-calendar-2020-12')
    body=tmp_path/'distributed.html'
    body.write_text('<html><p>Published</p><p>'+label+'</p>'
                    '<a href="https://www.businesswire.com/news/home/20201228005203/en">Source</a>'
                    '<p>Each market will close early at 1:00 p.m. on the announced dates.</p></html>',
                    encoding='utf-8')
    raw=body.read_bytes()
    receipt=tmp_path/'receipt.json'
    record={'url':url,'path':str(body),'capture_complete':True,'successful_response':True,
            'status_code':200,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),
            'finished_at_utc':'2026-10-04T12:00:00+00:00'}
    receipt.write_text(json.dumps({'status':'FOUR_ORIGINAL_SOURCE_CAPTURES_NOT_ADMISSION',
                                   'records':[record]}),encoding='utf-8')
    return body,receipt,url


def test_distributed_publication_preserves_minute_interval_and_no_ice_clock(tmp_path):
    value=H.original_distributed_nyse_clock(*_distributed(tmp_path))
    assert value['publication_lower_inclusive']==dt.datetime(2020,12,28,20,tzinfo=dt.timezone.utc)
    assert value['publication_upper_exclusive']==dt.datetime(2020,12,28,20,1,tzinfo=dt.timezone.utc)
    assert value['publication_precision']=='minute'
    assert value['published_label']=='Dec 28, 2020 3:00pm EST'
    assert value['ice_publication_clock'] is None
    assert value['source_qualified'] is False


def test_distributed_clock_rejects_wrong_season_timezone(tmp_path):
    with pytest.raises(ValueError,match='timezone/date conflict'):
        H.original_distributed_nyse_clock(*_distributed(tmp_path,'Dec 28, 2020 3:00pm EDT'))


def test_distributed_original_mutation_refuses_capture_binding(tmp_path):
    body,receipt,url=_distributed(tmp_path)
    body.write_bytes(body.read_bytes()+b'<p>Revision</p>')
    with pytest.raises(ValueError,match='capture result/body differs'):
        H.original_distributed_nyse_clock(body,receipt,url)


def test_original_ice_holiday_table_preserves_absent_juneteenth_and_blank_spacers(tmp_path):
    # Source-shaped Dec2020 factual cells recorded by root's original-structure
    # preflight. This fixture is not an actual publication/source admission.
    rows=[
        ['HOLIDAY','2021','2022','2023'],
        ["New Year's Day",'Friday, January 1','—',"Monday, January 2 (New Year's holiday observed)"],
        ['Martin Luther King, Jr. Day','Monday, January 18','Monday, January 17','Monday, January 16'],
        ["Washington's Birthday",'Monday, February 15','Monday, February 21','Monday, February 20'],
        ['Good Friday','Friday, April 2','Friday, April 15','Friday, April 7'],
        ['Memorial Day','Monday, May 31','Monday, May 30','Monday, May 29'],
        ['Independence Day','Monday, July 5 (Independence Day observed)','Monday, July 4','Tuesday, July 4*'],
        ['Labor Day','Monday, September 6','Monday, September 5','Monday, September 4'],
        ['Thanksgiving Day','Thursday, November 25','Thursday, November 24','Thursday, November 23**'],
        ['Christmas Day','Friday, December 24 (Christmas holiday observed)',
         'Monday, December 26 (Christmas holiday observed)','Monday, December 25'],
    ]
    from html import escape
    table=''.join('<tr>'+''.join('<td>'+escape(cell)+'</td><td></td>' for cell in row)+'</tr>' for row in rows)
    body=tmp_path/'calendar.html'
    body.write_text('<meta property="og:title" content="NYSE Group Announces 2021, 2022 and 2023 Holiday and Early Closings Calendar">'
                    '<table>'+table+'</table>',encoding='utf-8')
    parsed=H.original_nyse_calendar(body)
    assert parsed['years']==[2021,2022,2023]
    assert dt.date(2022,6,20) not in parsed['holidays'][2022]
    assert parsed['holidays'][2023][dt.date(2023,1,2)]=="New Year's Day"
    assert len(parsed['holidays'][2022])==8


def test_original_mourning_closure_retains_full_paragraph_digest(tmp_path):
    text='The New York Stock Exchange will be closed on Wednesday, December 5, 2018 to honor President George H. W. Bush.'
    body=tmp_path/'closure.html';body.write_text('<p>'+text+'</p>',encoding='utf-8')
    value=H.original_nyse_closure(body)
    assert value['closure_date']==dt.date(2018,12,5)
    assert value['paragraph_sha256']==hashlib.sha256(text.encode()).hexdigest()


def test_native_combined_preserves_precedence_primary_and_compression(tmp_path):
    import duckdb
    con=duckdb.connect(str(tmp_path/'combined.duckdb'),config={'threads':1,'memory_limit':'128MB'})
    con.execute("SET TimeZone='UTC'")
    try:
        con.execute("CREATE TABLE cal AS SELECT DATE '2020-01-02'+CAST(i AS INTEGER) AS session_date FROM range(3) r(i)")
        con.execute("""CREATE TABLE strict AS SELECT 10::BIGINT AS security_id,123::BIGINT AS cik,
            DATE '2020-01-02' AS start,DATE '2020-01-04' AS end_incl,'P' AS "primary",'strict' AS tier,'dated' AS basis,
            TIMESTAMP '2020-01-01 20:00:00' AS available_at""")
        con.execute("""CREATE TABLE bf AS SELECT 20::BIGINT AS security_id,session_date,row_number() OVER(ORDER BY session_date) AS sidx,
            123::BIGINT AS cik,'P' AS snap_primary,TIMESTAMP '2026-09-20 20:00:00' AS available_at FROM cal""")
        con.execute("""CREATE TABLE names AS SELECT security_id,session_date,123::BIGINT AS cik,
            TIMESTAMP '2020-01-01 21:00:00' AS available_at FROM cal CROSS JOIN(VALUES(10::BIGINT),(20::BIGINT),(30::BIGINT)) s(security_id)""")
        out=H._native_combined(con,strict='strict',backfill_days='bf',name_days='names',calendar='cal',namespace='t17_market_combined_test')
        actual=con.execute('SELECT * FROM '+out['identity/links_combined.parquet']+' ORDER BY security_id,start').fetchall()
        clock=dt.datetime
        assert actual==[
            (10,123,dt.date(2020,1,2),dt.date(2020,1,4),'P','strict','dated',clock(2020,1,1,20)),
            (20,123,dt.date(2020,1,2),dt.date(2020,1,4),'J','backfill','snapshot_run_backfill',clock(2026,9,20,20)),
            (30,123,dt.date(2020,1,2),dt.date(2020,1,4),'J','name','finra_name_match',clock(2020,1,1,21))]
        assert con.execute('SELECT count(*) FROM '+out['_strict_backfill_days']).fetchone()==(6,)
        assert con.execute('SELECT * FROM '+out['identity/links_backfill.parquet']).fetchall()==[actual[1]]
        assert [r[0] for r in con.execute('DESCRIBE '+out['identity/links_combined.parquet']).fetchall()]==[
            'security_id','cik','start','end_incl','primary','tier','basis','available_at']
    finally:con.close()


def test_native_strict_disk_intervals_match_original_all_version_oracle(tmp_path):
    import duckdb,pyarrow as pa,pyarrow.parquet as pq
    from atx_db.alpha_panel import identity_links as IL
    con=duckdb.connect(str(tmp_path/'strict.duckdb'),config={'threads':1,'memory_limit':'128MB'})
    con.execute("SET TimeZone='UTC'")
    day=dt.date;clock=dt.datetime
    versions=[
        (10,123,day(2020,1,1),None,'strict_dated','P','medium',clock(2020,1,1,20),None),
        (20,123,day(2020,1,1),None,'strict_dated','J','high',clock(2020,1,1,20),None),
        (10,999,day(2020,1,3),day(2020,1,4),'unaccepted','N','low',clock(2020,1,2,20),None),
        (20,123,day(2020,1,2),day(2020,1,5),'strict_dated','J','medium',clock(2020,1,2,23),None),
        (30,456,day(2020,1,1),None,'current_ticker_verified','P','high',clock(2020,1,1,20),None),
        (40,456,day(2020,1,1),None,'strict_dated','P','high',clock(2026,1,1,20),None)]
    try:
        con.execute('CREATE TABLE export(perm_security_id BIGINT,cik BIGINT,link_start DATE,link_end DATE,link_basis VARCHAR,link_primary VARCHAR,tier VARCHAR,available_at TIMESTAMP,valid_until TIMESTAMP)')
        con.executemany('INSERT INTO export VALUES(?,?,?,?,?,?,?,?,?)',versions)
        expected,stats=IL.compute_links(versions,day(2020,1,7))
        path=tmp_path/'original-oracle.parquet';pq.write_table(pa.Table.from_pylist(expected,schema=IL.SCHEMA),path)
        actual=H._native_strict(con,export='export',seal=day(2020,1,7),namespace='t17_market_strict_test')
        H._compare(con,'SELECT * FROM '+actual['identity/links.parquet'],path)
        assert actual['stats']['ambiguous_line_days']==stats['ambiguous_line_days']
        assert actual['stats']['multi_version_line_days']==stats['multi_version_line_days']
        assert actual['stats']['versions']==stats['version_counts']['versions']
        assert actual['stats']['visible_versions']==stats['version_counts']['visible_versions']
    finally:con.close()
