"""R26 fixed, independently pinned original FINRA source replay.

Private projections are limited to native preparation entrypoints which otherwise
mutate fixed scratch. Original mapping kernels remain the economic authority.
"""
from __future__ import annotations

import bisect
import contextlib
import csv
import datetime as dt
import gzip
import hashlib
import io
import json
import math
import os
import tempfile
from pathlib import Path

from . import common as C, finra_fetch as F, short_interest as SI
from . import short_volume as SV, short_volume_ext as E
from . import private_tier1_family_sources as A

VERSION = 'tier1-short-sources-v1'
NAMES = ('short_interest', 'short_volume_ext')
MAX_RAW = 16 * 1024 * 1024


def _json(path):
    return json.loads(A._blob(path))


def _q(path):
    return str(Path(path).as_posix()).replace("'", "''")


def code_pins():
    import duckdb
    import pyarrow
    import _duckdb
    from . import privatefund_runtime_resources as R, shortflow_common as S
    modules = (C, F, SI, SV, E, A, R, S, __import__(__name__, fromlist=['']))
    import inspect
    import types
    def identity(value):
        if isinstance(value,types.CodeType):
            return {key:identity(getattr(value,key)) for key in ('co_code','co_consts','co_names','co_varnames',
                'co_freevars','co_cellvars','co_argcount','co_posonlyargcount','co_kwonlyargcount',
                'co_nlocals','co_stacksize','co_flags','co_exceptiontable')}
        if isinstance(value,bytes):
            return ['bytes',value.hex()]
        if isinstance(value,tuple):
            return ['tuple',[identity(item) for item in value]]
        return [type(value).__name__,repr(value)]
    loaded={m.__name__+'.'+key:identity(value.__code__) for m in modules for key,value in vars(m).items()
            if inspect.isfunction(value) and value.__module__==m.__name__}
    constants={'SI_RAW_COLUMNS':SI.RAW_COLUMNS,'SI_RULE':SI.RULE,'SI_OTC_CLASSES':SI.OTC_CLASSES,
        'LOOKBACK_DAYS':SI.LOOKBACK_DAYS,'RULE_BUSINESS_DAYS':SI.RULE_BUSINESS_DAYS,
        'NYSE_HOLIDAYS_AFTER_VENDOR':sorted(map(str,SI.NYSE_HOLIDAYS_AFTER_VENDOR)),
        'SID0_START':str(F.SID0_START),'SID0_MAX_BRACKET_DAYS':F.SID0_MAX_BRACKET_DAYS,
        'MIN_ROWS_PER_SESSION':C.MIN_ROWS_PER_SESSION,'CANON_SQL':F.CANON_SQL,'SV_RULE':SV.RULE,
        'EXT_SCHEMA':E.SCHEMA,'EXT_CLOCK':E.CLOCK_RULE,'EXT_STALENESS':E.STALENESS_DAYS,
        'SI_FILE_URL':SI.FILE_URL,'SI_API_URL':SI.API_DATA_URL,'SI_API_SORT':SI.API_SORT,'SI_API_PAGE':SI.API_PAGE,
        'SV_URL':SV.URL}
    binaries={str(path.resolve()):A.pin(path) for package in (Path(duckdb.__file__).parent,Path(pyarrow.__file__).parent)
              for path in package.iterdir() if path.suffix.lower() in ('.pyd','.dll')}
    return {str(A.absolute(m.__file__)): A.pin(m.__file__) for m in (*modules,_duckdb)} | binaries | {
        'runtime:short-loaded-code':{'sha256':A.digest(loaded),'bytes':None},
        'runtime:short-constants':{'sha256':A.digest(constants),'bytes':None},
        'runtime:short-packages': {'sha256': A.digest({'python': __import__('sys').version,
                    'duckdb': duckdb.__version__, 'pyarrow': pyarrow.__version__}), 'bytes': None}}


def _check(pins):
    runtime = code_pins()
    for path, expected in pins.items():
        actual = runtime.get(path) if path.startswith('runtime:') else A.pin(path)
        if actual != expected:
            raise ValueError('independent short dependency changed: ' + path)


def _compare(con, sql, path):
    actual = f"SELECT * FROM read_parquet('{_q(path)}',hive_partitioning=false)"
    if con.execute('DESCRIBE ' + sql).fetchall() != con.execute('DESCRIBE ' + actual).fetchall():
        raise ValueError('native short typed schema differs')
    mismatch = con.execute(f"SELECT count(*) FROM (({sql}) EXCEPT ALL ({actual}))").fetchone()[0]
    mismatch += con.execute(f"SELECT count(*) FROM (({actual}) EXCEPT ALL ({sql}))").fetchone()[0]
    if mismatch:
        raise ValueError('complete duplicate-preserving native short replay differs')


def settlement_rows(settlements, sessions, schedule):
    """SI:281-298 with an independently bound schedule, unchanged fallback."""
    rows = []
    for settlement in settlements:
        publication, source = schedule.get(settlement, (None, None))
        if publication is None:
            publication, source = SI.add_business_days(settlement, SI.RULE_BUSINESS_DAYS, sessions), 'rule-7bd'
        if publication <= settlement:
            raise ValueError('native dissemination not after settlement')
        index = bisect.bisect_right(sessions, settlement) - 1
        mapped = sessions[index] if index >= 0 and (settlement - sessions[index]).days <= SI.LOOKBACK_DAYS else None
        rows.append((settlement, publication, source, mapped))
    return rows


def repair_sql(con, ticker, target):
    """FINRA:233-280 exact source-bound SQL; retain all exclusion rows."""
    source = _q(ticker)
    lo = F.SID0_START - dt.timedelta(days=F.SID0_MAX_BRACKET_DAYS)
    con.execute(f"""CREATE OR REPLACE TEMP TABLE z AS
        SELECT tradingDate AS d,ticker_tk AS tk,count(*) AS n0
        FROM read_parquet('{source}') WHERE securityID=0 AND tradingDate>=DATE '{F.SID0_START}'
        AND ticker_tk IS NOT NULL AND ticker_tk<>'' GROUP BY 1,2""")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE nz AS
        SELECT tradingDate AS d,ticker_tk AS tk,min(securityID) AS sid,count(DISTINCT securityID) AS n
        FROM read_parquet('{source}') WHERE securityID>0 AND tradingDate>=DATE '{lo}'
        AND ticker_tk IN(SELECT DISTINCT tk FROM z) GROUP BY 1,2""")
    con.execute("""CREATE OR REPLACE TEMP TABLE zb AS WITH p AS(
        SELECT z.d,z.tk,z.n0,nz.d AS prev_d,nz.sid AS prev_sid,nz.n AS prev_n
        FROM z ASOF LEFT JOIN nz ON z.tk=nz.tk AND z.d>nz.d)
        SELECT p.*,nz.d AS next_d,nz.sid AS next_sid,nz.n AS next_n
        FROM p ASOF LEFT JOIN nz ON p.tk=nz.tk AND p.d<nz.d""")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE own AS
        SELECT DISTINCT tradingDate AS d,securityID AS sid FROM read_parquet('{source}')
        WHERE tradingDate>=DATE '{F.SID0_START}' AND securityID IN
        (SELECT DISTINCT prev_sid FROM zb WHERE prev_sid=next_sid)""")
    sql = f"""SELECT d AS trade_date,tk AS ticker_tk,
        CASE WHEN basis='bracketed' THEN prev_sid END AS security_id,
        basis,n0 AS sid0_rows,prev_d,prev_sid,next_d,next_sid FROM(
        SELECT zb.*,CASE WHEN n0>1 THEN 'several_sid0_rows'
        WHEN prev_sid IS NULL AND next_sid IS NULL THEN 'ticker_never_nonzero'
        WHEN prev_sid IS NULL THEN 'next_only' WHEN next_sid IS NULL THEN 'prev_only'
        WHEN prev_n>1 OR next_n>1 THEN 'neighbour_ambiguous'
        WHEN prev_sid<>next_sid THEN 'bracket_disagrees'
        WHEN date_diff('day',zb.prev_d,zb.d)>{F.SID0_MAX_BRACKET_DAYS}
          OR date_diff('day',zb.d,zb.next_d)>{F.SID0_MAX_BRACKET_DAYS} THEN 'bracket_too_far'
        WHEN own.sid IS NOT NULL THEN 'id_has_own_row' ELSE 'bracketed' END AS basis
        FROM zb LEFT JOIN own ON own.d=zb.d AND own.sid=zb.prev_sid)
        ORDER BY trade_date,ticker_tk"""
    C.copy_to_parquet(con, sql, target)
    for table in ('z', 'nz', 'zb', 'own'):
        con.execute('DROP TABLE ' + table)
    return sql


def ticker_sql(ticker, repair, dates):
    dates = ','.join(f"DATE '{value}'" for value in sorted(dates)) or 'NULL'
    return f"""SELECT tradingDate AS map_date,securityID AS security_id,ticker_tk,false AS sid_repaired
        FROM read_parquet('{_q(ticker)}') WHERE tradingDate IN({dates})
        AND securityID IS NOT NULL AND securityID>0 AND ticker_tk IS NOT NULL AND trim(ticker_tk)<>''
        UNION ALL SELECT trade_date,security_id,ticker_tk,true FROM read_parquet('{_q(repair)}')
        WHERE basis='bracketed' AND trade_date IN({dates}) ORDER BY 1,3"""


def asof_sql(con, parsed, primary, ticker, repair, rows, sessions):
    """SI:313-354 exact preparation with explicit source paths."""
    mapping = {}
    for settlement, _, _, mapped in rows:
        if mapped is None:
            continue
        for day in sessions[bisect.bisect_left(sessions,settlement-dt.timedelta(days=SI.LOOKBACK_DAYS)):
                            bisect.bisect_left(sessions,mapped)]:
            if day in mapping:
                raise ValueError('native fallback windows overlap')
            mapping[day] = settlement
    con.execute('CREATE OR REPLACE TEMP TABLE dmap(d DATE,settlement_date DATE)')
    if mapping:
        con.executemany('INSERT INTO dmap VALUES (?,?)', sorted(mapping.items()))
    ck_t, ck_r = F.CANON_SQL.format(x='t.ticker_tk'), F.CANON_SQL.format(x='r.ticker_tk')
    return f"""WITH prim AS(
        SELECT st.settlement_date,t.map_date AS vendor_date,t.security_id,t.ticker_tk,{ck_t} AS ck,
        t.sid_repaired,false AS fallback FROM read_parquet('{_q(primary)}') t
        JOIN settle st ON st.map_date=t.map_date),
        need AS(SELECT DISTINCT CAST(settlementDate AS DATE) AS settlement_date,
        {F.CANON_SQL.format(x='symbolCode')} AS ck FROM read_parquet('{_q(parsed)}')
        WHERE settlementDate IS NOT NULL AND settlementDate<>''
        EXCEPT SELECT settlement_date,ck FROM prim),
        win AS(SELECT dm.settlement_date,t.tradingDate AS vendor_date,t.securityID AS security_id,
        t.ticker_tk,{ck_t} AS ck,false AS sid_repaired FROM read_parquet('{_q(ticker)}') t
        JOIN dmap dm ON dm.d=t.tradingDate WHERE t.securityID>0 AND t.ticker_tk IS NOT NULL AND trim(t.ticker_tk)<>''
        UNION ALL SELECT dm.settlement_date,r.trade_date,r.security_id,r.ticker_tk,{ck_r},true
        FROM read_parquet('{_q(repair)}') r JOIN dmap dm ON dm.d=r.trade_date WHERE r.basis='bracketed'),
        fb AS(SELECT w.*,max(w.vendor_date) OVER(PARTITION BY w.settlement_date,w.ck) AS last_d
        FROM win w SEMI JOIN need n ON n.settlement_date=w.settlement_date AND n.ck=w.ck)
        SELECT * FROM prim UNION ALL SELECT settlement_date,vendor_date,security_id,ticker_tk,ck,sid_repaired,true
        FROM fb WHERE vendor_date=last_d"""


def _body(path, compressed=False):
    path = A.absolute(path)
    with (gzip.open(path,'rb') if compressed else path.open('rb')) as stream:
        result = stream.read(MAX_RAW+1)
    if len(result)>MAX_RAW:
        raise ValueError('original short file exceeds bounded per-file replay')
    return result


def parse_interest(sources, target):
    """SI:217-260 conversion, streaming CSV batches instead of whole population."""
    import pyarrow as pa
    import pyarrow.csv as pc
    import pyarrow.parquet as pq
    schema = pa.schema([(name,pa.string()) for name in SI.RAW_COLUMNS] +
                       [('line_no',pa.int32()),('source_file',pa.string()),('landing',pa.int8())])
    stats = {'files':len(sources),'rows':0,'invalid_rows_skipped':0}
    with pq.ParquetWriter(target,schema,compression='zstd') as writer:
        for source in sources:
            body = _body(source['path'],source['gz'])
            if hashlib.sha256(body).hexdigest()!=source['sha256']:
                raise ValueError('native original SI raw hash differs')
            skipped = [0]
            def skip(_):
                skipped[0] += 1
                return 'skip'
            reader = pc.open_csv(io.BytesIO(body),read_options=pc.ReadOptions(block_size=1<<20),
                parse_options=pc.ParseOptions(delimiter=',' if source['file'].startswith('si_api_') else '|',
                    quote_char='"' if source['file'].startswith('si_api_') else False,invalid_row_handler=skip),
                convert_options=pc.ConvertOptions(column_types={c:pa.string() for c in SI.RAW_COLUMNS},
                    include_columns=SI.RAW_COLUMNS,strings_can_be_null=False))
            line = 0
            for batch in reader:
                table = pa.Table.from_batches([batch])
                table = table.append_column('line_no',pa.array(range(line,line+len(table)),pa.int32()))
                table = table.append_column('source_file',pa.array([source['file']]*len(table),pa.string()))
                table = table.append_column('landing',pa.array([source['landing']]*len(table),pa.int8()))
                writer.write_table(table,row_group_size=64)
                line += len(table)
            stats['rows'] += line
            stats['invalid_rows_skipped'] += skipped[0]
    return stats


def parse_volume(sources, target):
    import pyarrow as pa
    import pyarrow.parquet as pq
    schema = pa.schema([('trade_date',pa.date32()),('symbol',pa.string()),('short_volume',pa.float64()),
                ('short_exempt_volume',pa.float64()),('total_volume',pa.float64()),('market',pa.string())])
    stats = {'files':len(sources),'rows':0,'bad':0,'date_mismatch':0,'row_count_vs_manifest_mismatch':0}
    with pq.ParquetWriter(target,schema,compression='zstd') as writer:
        for source in sources:
            body = _body(source['path'],True)
            if hashlib.sha256(body).hexdigest()!=source['sha256']:
                raise ValueError('native CNMS raw hash differs')
            # Unchanged parser runs one bounded original daily file, not a year.
            rows, parsed = SV.parse_file(body,dt.date.fromisoformat(source['date']))
            stats['rows'] += len(rows)
            for key in ('bad','date_mismatch'):
                stats[key] += parsed[key]
            stats['row_count_vs_manifest_mismatch'] += len(rows)!=int(source['rows'])
            for index in range(0,len(rows),64):
                writer.write_table(pa.Table.from_pylist([dict(zip(schema.names,row))
                    for row in rows[index:index+64]],schema=schema),row_group_size=64)
    return stats


def _manifest(path, date_key):
    with A.absolute(path).open(newline='',encoding='utf-8') as stream:
        rows = list(csv.DictReader(stream))
    result = {}
    for row in rows:
        if row.get(date_key):
            dt.date.fromisoformat(row[date_key])
            result[row[date_key]] = row
    return result


def _binding(name,spec):
    return {'family':name,**{key:spec[key] for key in
                ('interface_version','root','generation','manifest_pin','native')}}


def _prepare(name,spec,con):
    if name not in NAMES or spec['interface_version']!=VERSION:
        raise ValueError('fixed native short family/version required')
    root = A.absolute(spec['root'])
    A._guard(con,root)
    generation = A.absolute(spec['generation'])
    if not generation.is_relative_to(root) or A.pin(generation/'manifest.json')!=spec['manifest_pin']:
        raise ValueError('independent native short generation/manifest differs')
    native = spec['native']
    plan_path = A.absolute(native['plan_path'])
    if A.pin(plan_path)['sha256']!=native['expected_plan_sha256']:
        raise ValueError('independent original short plan differs')
    plan = _json(plan_path)
    if plan['schema']!='atx.tier1-short-source-plan/v1' or plan['family']!=name:
        raise ValueError('fixed complete native short source plan required')
    code = code_pins()
    review_path = A.absolute(spec['independent_review']['path'])
    if A.pin(review_path)['sha256']!=spec['independent_review']['expected_sha256']:
        raise ValueError('independent short review bytes differ')
    review = _json(review_path)
    if review != {'schema':'atx.tier1-short-review/v1','status':'scoped_pass','interface_version':VERSION,
                  'family':name,'binding_sha256':A.digest(_binding(name,spec)),'code_pins':code}:
        raise ValueError('independent original-source short review prerequisite pending')
    dependencies = dict(code)
    def add(path):
        path = A.absolute(path)
        dependencies[str(path)] = A.pin(path)
        return path
    for path in (plan_path,review_path,generation/'manifest.json',plan['ticker_history'],
                 plan['ticker_manifest'],plan['clock_inventory']):
        add(path)
    native_manifest=_json(generation/'manifest.json')
    ticker_manifest=_json(plan['ticker_manifest'])
    identity=ticker_manifest.get('tickerhistory',ticker_manifest.get('receipt',{}).get('tickerhistory'))
    actual_identity=C.file_identity(A.absolute(plan['ticker_history']))
    if (not identity or A.absolute(identity['path'])!=A.absolute(plan['ticker_history'])
            or identity.get('bytes')!=actual_identity['bytes'] or identity.get('mtime_ns')!=actual_identity['mtime_ns']):
        raise ValueError('original native ticker lineage identity differs')
    native_identity=native_manifest.get('tickerhistory',native_manifest.get('receipt',{}).get('tickerhistory'))
    if native_identity!=identity:
        raise ValueError('native short product ticker lineage differs')
    clock = _json(plan['clock_inventory'])
    if clock['schema']!='atx.finra-short-original-observations/v1':
        raise ValueError('original observation inventory required')
    # Every observation document is a separately pinned original capture receipt;
    # no source_vintage_proven/qualified Boolean is an input.
    for key in ('ticker_receipt','schedule_receipt'):
        if clock.get(key):
            add(clock[key])
    for path in clock['files'].values():
        if path is not None:
            add(path)
    sources=[]
    all_files={}
    if name=='short_interest':
        old = _manifest(add(plan['old_manifest']),'settlement_date')
        new = _manifest(add(plan['new_manifest']),'date')
        add(plan['schedule'])
        for date,row in sorted(old.items()):
            path = add(Path(plan['old_raw_root'])/row['file'])
            if A.pin(path)['sha256']!=row['sha256']:
                raise ValueError('complete old FINRA source hash differs')
            source={'date':date,'path':path,'sha256':row['sha256'],'landing':0,'gz':False,'file':row['file']}
            sources.append(source)
            all_files[str(path)] = source
        for date,row in sorted(new.items()):
            if int(row.get('http_status') or 0)!=200:
                continue
            path=add(Path(plan['new_raw_root'])/row['file'])
            source={'date':date,'path':path,'sha256':row['sha256_of_raw_bytes'],'landing':1,'gz':True,
                    'file':row['file'],'url':row['url']}
            if hashlib.sha256(_body(path,True)).hexdigest()!=source['sha256']:
                raise ValueError('complete new FINRA source hash differs')
            all_files[str(path)] = source
            if date not in old:
                sources.append(source)
        products={key:generation/key for key in ('si.parquet','mapping_audit.parquet')}
    else:
        manifest=_manifest(add(plan['source_manifest']),'date')
        start,end=dt.date.fromisoformat(plan['start']),dt.date.fromisoformat(plan['end'])
        if start>end or set(manifest)!=set(plan['expected_dates']):
            raise ValueError('complete original CNMS expected-date inventory differs')
        for date,row in sorted(manifest.items()):
            if int(row.get('http_status') or 0)!=200:
                continue
            path=add(Path(plan['raw_root'])/row['file'])
            if row['url']!=SV.URL.format(d=dt.date.fromisoformat(date)):
                raise ValueError('original CNMS URL differs')
            source={'date':date,'path':path,'sha256':row['sha256_of_raw_bytes'],'rows':row['rows'],'url':row['url']}
            all_files[str(path)]=source
            # Native ext processes all successful files for each selected year.
            if start.year<=dt.date.fromisoformat(date).year<=end.year:
                sources.append(source)
        products={f'year={year}/short_volume_ext.parquet':generation/f'year={year}'/'short_volume_ext.parquet'
                  for year in sorted({dt.date.fromisoformat(s['date']).year for s in sources})}
    if not sources or set(clock['files'])!=set(all_files):
        raise ValueError('full raw file/clock inventory differs; missing clocks require explicit null entries')
    for key,path in products.items():
        if A.pin(add(path))!=plan['native_products'][key]:
            raise ValueError('independent native product bytes differ')
    if set(plan['native_products'])!=set(products):
        raise ValueError('complete fixed native short product inventory differs')
    if name=='short_interest':
        if (native_manifest.get('rule')!=SI.RULE or native_manifest.get('sources',{}).get('files_sha256')!=
                {source['file']:source['sha256'] for source in sources}):
            raise ValueError('original SI rule/full source manifest lineage differs')
    else:
        if (native_manifest.get('schema')!=E.SCHEMA or native_manifest.get('status')!='complete'
                or native_manifest.get('stage')!=E.STAGE or native_manifest.get('map_rule')!=SV.RULE
                or native_manifest.get('clock_rule')!=E.CLOCK_RULE
                or native_manifest.get('files')!=plan['native_products']):
            raise ValueError('original ext schema/clock/product inventory differs')
        for module in E.MODULES:
            path=Path(E.__file__).parent/(module+'.py')
            if native_manifest.get('code',{}).get(path.name,{}).get('sha256')!=A.pin(path)['sha256']:
                raise ValueError('original ext producer code lineage differs')
    if len(all_files)!=plan['expected_raw_files'] or len(sources)!=plan['expected_selected_files']:
        raise ValueError('complete original short source denominator differs')
    if dependencies!=spec['dependency_pins']:
        raise ValueError('independently expected COMPLETE actual short dependencies differ')
    _check(dependencies)
    return plan,clock,sources,products,dependencies


def _observed(receipt,sha,url,ceiling):
    if receipt is None:
        return False,'missing_original_contemporaneous_receipt'
    record=_json(receipt)
    # These are original independently reviewed retrieval receipts, not a new
    # candidate-generated admission summary. Later retrieval never backdates.
    if (str(record.get('http_status'))!='200' or record.get('url')!=url
            or record.get('sha256_of_raw_bytes')!=sha):
        return False,'original_receipt_url_or_raw_hash_mismatch'
    try:
        observed=dt.datetime.fromisoformat(record['downloaded_at'].replace('Z','+00:00'))
    except (KeyError,ValueError,TypeError):
        return False,'missing_original_observation_clock'
    if observed.tzinfo is None or observed>ceiling:
        return False,'later_or_unresolved_original_vintage'
    return True,'original_contemporaneous_bytes_observed'


def _qualification(name,row,plan,clock,sources,sessions,parse):
    if name=='short_interest':
        publication=row['dissemination_date']
        index=bisect.bisect_right(sessions,publication)
        day=sessions[index] if index<len(sessions) else None
        native_clock=dt.datetime.combine(day,dt.time(),dt.timezone.utc) if day else None
        selected=[s for s in sources if s['file']==row['source_file']]
        if row['dissemination_source']=='rule-7bd':
            return native_clock,False,'unproven_official_dissemination'
        if row['revision_flag'] not in (None,'','0','N'):
            return native_clock,False,'native_revision_flag'
        if row['split_flag'] not in (None,'','0','N'):
            return native_clock,False,'unresolved_split_basis'
    else:
        day=row['trade_date']+dt.timedelta(days=1)
        native_clock=dt.datetime.combine(day,dt.time(),dt.timezone.utc)
        selected=[s for s in sources if s['date']==row['trade_date'].isoformat()]
    if native_clock is None or len(selected)!=1:
        return native_clock,False,'missing_or_ambiguous_original_clock_source'
    source=selected[0]
    date=dt.date.fromisoformat(source['date'])
    url=source.get('url') or SI.FILE_URL.format(d=date)
    if name=='short_interest' and not (url==SI.FILE_URL.format(d=date) or
            url==f"{SI.API_DATA_URL}?settlementDate={date}&sortFields={','.join(SI.API_SORT)}&limit={SI.API_PAGE}"):
        return native_clock,False,'unrecognized_original_FINRA_URL'
    qualified,reason=_observed(clock['files'][str(source['path'])],source['sha256'],url,native_clock)
    if not qualified:
        return native_clock,False,reason
    ticker_receipt=clock.get('ticker_receipt')
    ticker_url=plan['ticker_source_url']
    qualified,reason=_observed(ticker_receipt,A.pin(plan['ticker_history'])['sha256'],ticker_url,native_clock)
    if not qualified:
        return native_clock,False,'ticker_'+reason
    if row['security_id'] is None:
        return native_clock,False,'native_missing_or_ambiguous_security_mapping'
    values=(row.get('si_shares'),row.get('si_prev'),row.get('adv_finra')) if name=='short_interest' else (
        row.get('short_volume'),row.get('short_exempt_volume'),row.get('total_volume'))
    if any(value is None or not math.isfinite(value) or value<0 for value in values):
        return native_clock,False,'invalid_native_quantity'
    if name=='short_volume_ext' and (row['short_exempt_volume']>row['short_volume'] or row['short_volume']>row['total_volume']):
        return native_clock,False,'native_volume_basis_exclusion'
    if row.get('sid_repaired') or row.get('map_basis')=='sid0_repaired':
        return native_clock,False,'future_bracket_identity_repair_not_PIT'
    if name=='short_interest':
        qualified,reason=_observed(clock.get('schedule_receipt'),A.pin(plan['schedule'])['sha256'],
                                  plan['schedule_source_url'],native_clock)
        if not qualified:
            return native_clock,False,'schedule_'+reason
    if any(parse.get(key,0) for key in ('bad','date_mismatch','row_count_vs_manifest_mismatch','invalid_rows_skipped')):
        return native_clock,False,'original_parse_population_exclusions'
    return native_clock,True,'source_clock_mapping_vintage_observed'


def _extended_sql(mapped):
    # E.build:118-134; SQL, rather than parse_market, is the published authority
    # (its whitespace handling intentionally differs and must be preserved).
    return f"""SELECT security_id,symbol,trade_date,short_volume,short_exempt_volume,total_volume,market,
        list_contains(string_split(market,','),'B') AS fac_b,
        list_contains(string_split(market,','),'Q') AS fac_q,
        list_contains(string_split(market,','),'N') AS fac_n,
        list_contains(string_split(market,','),'D') AS fac_d,
        nullif(array_to_string(list_sort(list_distinct(list_filter(string_split(market,','),
          x->trim(x)<>'' AND trim(x) NOT IN('B','Q','N','D')))),','),'') AS fac_other,
        len(list_distinct(list_filter(string_split(market,','),x->trim(x)<>''))) AS n_facilities,
        map_basis,sid_repaired,CAST(trade_date+INTERVAL 1 DAY AS TIMESTAMP) AT TIME ZONE 'UTC' AS available_at,
        true AS vintage_risk FROM read_parquet('{_q(mapped)}') ORDER BY trade_date,symbol"""


def _map_cells(name,product,plan,clock,sources,sessions,parse,target):
    import pyarrow as pa
    import pyarrow.parquet as pq
    schema=pq.read_schema(product)
    # Preserve every native primitive unchanged, adding fixed source-derived
    # qualification. SI has no original available_at column; ext already does.
    if 'available_at' not in schema.names:
        schema=schema.append(pa.field('available_at',pa.timestamp('us',tz='UTC')))
    if name=='short_interest':
        schema=schema.append(pa.field('native_first_visible_session',pa.date32()))
    schema=schema.append(pa.field('qualified',pa.bool_())).append(pa.field('qualification_reason',pa.string()))
    schema=schema.append(pa.field('source_vintage_proven',pa.bool_())).append(pa.field('source_sha256',pa.string()))
    rows=qualified=0
    reasons={}
    with pq.ParquetWriter(target,schema,compression='zstd') as writer:
        for batch in pq.ParquetFile(product).iter_batches(batch_size=64):
            output=[]
            for row in batch.to_pylist():
                native_clock,valid,reason=_qualification(name,row,plan,clock,sources,sessions,parse)
                row['available_at']=native_clock
                if name=='short_interest':
                    row['native_first_visible_session']=native_clock.date() if native_clock else None
                row['qualified']=valid
                row['qualification_reason']=reason
                selected=[s for s in sources if (s['file']==row['source_file'] if name=='short_interest'
                                              else s['date']==row['trade_date'].isoformat())]
                row['source_sha256']=selected[0]['sha256'] if len(selected)==1 else None
                source=selected[0] if len(selected)==1 else None
                url=(source.get('url') or SI.FILE_URL.format(d=dt.date.fromisoformat(source['date']))) if source else None
                row['source_vintage_proven']=bool(source and native_clock and _observed(
                    clock['files'][str(source['path'])],source['sha256'],url,native_clock)[0])
                output.append(row)
                rows+=1
                qualified+=valid
                reasons[reason]=reasons.get(reason,0)+1
            writer.write_table(pa.Table.from_pylist(output,schema=schema),row_group_size=64)
    return {'rows':rows,'qualified_rows':qualified,'exclusions':reasons}


def _run(name,spec,con):
    plan,clock,sources,products,pins=_prepare(name,spec,con)
    scratch=Path(tempfile.mkdtemp(prefix='atx-short-native-replay-'))
    ticker=A.absolute(plan['ticker_history'])
    sessions=[row[0] for row in con.execute(f"""SELECT tradingDate FROM read_parquet('{_q(ticker)}')
        GROUP BY 1 HAVING count(*)>={C.MIN_ROWS_PER_SESSION} ORDER BY 1""").fetchall()]
    if not sessions or len(sessions)!=plan['expected_sessions']:
        raise ValueError('complete native vendor session denominator differs')
    for table in ('z','nz','zb','own','settle','dmap','audit'):
        if con.execute('SELECT count(*) FROM duckdb_tables() WHERE table_name=?',[table]).fetchone()[0]:
            raise ValueError('native short replay table collision: '+table)
    repair=scratch/'repair.parquet'
    repair_sql(con,ticker,repair)
    replay={}
    mapped={}
    validation={'family':name,'source_files':len(sources),'all_original_files':plan['expected_raw_files'],
                'sessions':len(sessions),'native_map_rule':SI.RULE if name=='short_interest' else SV.RULE,
                'source_quality_certified':False,'parse':{},'mapping':{},'native_rows':{}}
    if name=='short_interest':
        parsed=scratch/'parsed.parquet'
        parse=parse_interest(sources,parsed)
        validation['parse']=parse
        settlements=[r[0] for r in con.execute(f"""SELECT DISTINCT CAST(settlementDate AS DATE)
            FROM read_parquet('{_q(parsed)}') WHERE settlementDate IS NOT NULL AND settlementDate<>'' ORDER BY 1""").fetchall()]
        with A.absolute(plan['schedule']).open(newline='',encoding='utf-8') as stream:
            schedule={dt.date.fromisoformat(r['settlement_date']):(dt.date.fromisoformat(r['dissemination_date']),r['source'])
                      for r in csv.DictReader(stream)}
        rows=settlement_rows(settlements,sessions,schedule)
        con.execute('CREATE TEMP TABLE settle(settlement_date DATE,dissemination_date DATE,dissemination_source VARCHAR,map_date DATE)')
        con.executemany('INSERT INTO settle VALUES (?,?,?,?)',rows)
        primary=scratch/'ticker.parquet'
        C.copy_to_parquet(con,ticker_sql(ticker,repair,{r[3] for r in rows if r[3]}),primary)
        asof=scratch/'asof.parquet'
        C.copy_to_parquet(con,asof_sql(con,parsed,primary,ticker,repair,rows,sessions),asof)
        con.execute(SI._stage_sql(_q(parsed),_q(asof)))
        audit=scratch/'mapping_audit.parquet'
        C.copy_to_parquet(con,'SELECT * FROM audit ORDER BY settlement_date,symbol',audit)
        si=scratch/'si.parquet'
        C.copy_to_parquet(con,"""SELECT CAST(candidate_security_id AS BIGINT) AS security_id,symbol,settlement_date,dissemination_date,
            si_shares,si_prev,CASE WHEN adv_finra>0 THEN dtc_raw END AS si_dtc,adv_finra,
            revision_flag,market_class,split_flag,dissemination_source,
            CASE WHEN sid_repaired THEN 'sid0_repaired' WHEN exact THEN 'exact' ELSE 'canonical' END AS map_basis,
            vendor_date,vendor_date_fallback,source_file FROM audit WHERE outcome='mapped'
            ORDER BY settlement_date,security_id""",si)
        replay={'si.parquet':si,'mapping_audit.parquet':audit}
        cell=scratch/'short_interest_cells.parquet'
        validation['mapping']['short_interest_cells.parquet']=_map_cells(name,si,plan,clock,sources,sessions,parse,cell)
        mapped={'short_interest_cells.parquet':cell}
        for table in ('audit','settle','dmap'):
            con.execute('DROP TABLE '+table)
    else:
        for year in sorted({dt.date.fromisoformat(s['date']).year for s in sources}):
            chosen=[s for s in sources if dt.date.fromisoformat(s['date']).year==year]
            raw=scratch/f'raw_{year}.parquet'
            parse=parse_volume(chosen,raw)
            validation['parse'][str(year)]=parse
            dates={dt.date.fromisoformat(s['date']) for s in chosen}
            primary=scratch/f'ticker_{year}.parquet'
            C.copy_to_parquet(con,ticker_sql(ticker,repair,dates),primary)
            native=scratch/f'mapped_{year}.parquet'
            SV.map_year(con,raw,primary,native)
            ext=scratch/f'ext_{year}.parquet'
            C.copy_to_parquet(con,_extended_sql(native),ext)
            key=f'year={year}/short_volume_ext.parquet'
            replay[key]=ext
            cell=scratch/f'cells_{year}.parquet'
            cellkey=f'year={year}/short_volume_ext_cells.parquet'
            validation['mapping'][cellkey]=_map_cells(name,ext,plan,clock,chosen,sessions,parse,cell)
            mapped[cellkey]=cell
    for key,path in replay.items():
        _compare(con,f"SELECT * FROM read_parquet('{_q(path)}',hive_partitioning=false)",products[key])
        validation['native_rows'][key]=con.execute(f"SELECT count(*) FROM read_parquet('{_q(path)}')").fetchone()[0]
    if validation['native_rows']!=plan['expected_native_rows']:
        raise ValueError('complete native short output denominators differ')
    _check(pins)
    A._guard(con,A.absolute(spec['root']))
    return products,pins,validation,mapped


def materialize(name,spec,con):
    """Root actual slot only; isolated fixtures may publish synthetic originals."""
    _,pins,validation,mapped=_run(name,spec,con)
    if A.digest(validation)!=spec['expected_validation_sha256']:
        raise ValueError('independently expected full native short validation differs')
    target=Path(spec['mapped_generation'])
    if not target.is_absolute() or not target.parent.resolve(strict=True).is_relative_to(A.absolute(spec['root'])):
        raise ValueError('private immutable short generation must stay within root')
    files={key:A.pin(path) for key,path in mapped.items()}
    manifest={'schema':'atx.tier1-short-mapping/v1','source_binding':_binding(name,spec),
              'dependency_pins':pins,'validation':validation,'files':files}
    if target.exists():
        if _json(target/'manifest.json')!=manifest or any(A.pin(target/key)!=pin for key,pin in files.items()):
            raise ValueError('existing immutable short generation differs')
        return {'generation':str(target),'manifest_pin':A.pin(target/'manifest.json'),'product_pins':files,'validation':validation}
    pending=target.with_name(target.name+'.partial')
    if pending.exists():
        raise ValueError('retained incomplete short publication requires controller reconciliation')
    pending.mkdir()
    for key,path in mapped.items():
        dest=pending/key
        dest.parent.mkdir(parents=True,exist_ok=True)
        with path.open('rb') as source,dest.open('xb') as out:
            while block:=source.read(1024*1024):
                out.write(block)
        if A.pin(dest)!=files[key]:
            raise ValueError('short output changed during private publication')
    (pending/'manifest.json').write_text(json.dumps(manifest,sort_keys=True,separators=(',',':')),encoding='utf-8')
    _check(pins)
    os.rename(pending,target)
    return {'generation':str(target),'manifest_pin':A.pin(target/'manifest.json'),'product_pins':files,'validation':validation}


def admit(name,spec,con):
    native,pins,validation,mapped=_run(name,spec,con)
    target=A.absolute(spec['mapped_generation'])
    if not target.is_relative_to(A.absolute(spec['root'])):
        raise ValueError('private mapped short generation outside root')
    if A.pin(target/'manifest.json')!=spec['mapped_manifest_pin']:
        raise ValueError('independent mapped short manifest differs')
    files=spec['mapped_product_pins']
    manifest=_json(target/'manifest.json')
    if (set(files)!=set(mapped) or A.digest(validation)!=spec['expected_validation_sha256'] or
        manifest!={'schema':'atx.tier1-short-mapping/v1','source_binding':_binding(name,spec),
                   'dependency_pins':pins,'validation':validation,'files':files}):
        raise ValueError('independent complete native/mapped short replay differs')
    expected=set(files)|{'manifest.json'}
    actual={p.relative_to(target).as_posix() for p in target.rglob('*') if p.is_file()}
    if actual!=expected:
        raise ValueError('immutable short publication inventory differs')
    products=dict(native)
    for key,path in mapped.items():
        dest=target/key
        if A.pin(dest)!=files[key]:
            raise ValueError('independent mapped short bytes differ')
        _compare(con,f"SELECT * FROM read_parquet('{_q(path)}',hive_partitioning=false)",dest)
        pins[str(dest)]=files[key]
        products[key]=dest
    pins[str(target/'manifest.json')]=spec['mapped_manifest_pin']
    _check(pins)
    import pyarrow.parquet as pq
    schemas={key:{field.name:str(field.type) for field in pq.read_schema(path)} for key,path in products.items()}
    views={key:{'view':'short_interest_qualified' if name=='short_interest' else 'short_volume_ext_qualified',
                'keys':['security_id','settlement_date'] if name=='short_interest' else ['trade_date','symbol'],
                'clock':'available_at','qualification':'qualified; native diagnostics/exclusions retained',
                'clock_rule':('00UTC first native vendor session strictly after dissemination; GC1 prior-day22UTC '
                              'may add one formation session; no intraday backdating' if name=='short_interest' else
                              'native T+1 00UTC; GC1 strict prior-calendar-day22UTC; original vintage_risk retained')}
           for key in mapped}
    return {'status':'admitted_diagnostic','products':products,'validation':validation,'dependency_pins':pins,
            'source_quality_certified':False,'schemas':schemas,'views':views,'family':name,'spec':spec}


def _read_check(admission,con):
    name,spec=admission['family'],admission['spec']
    prepared=_prepare(name,spec,con)
    native,pins=prepared[3],prepared[4]
    target=A.absolute(spec['mapped_generation'])
    products={**native,**{key:target/key for key in spec['mapped_product_pins']}}
    pins.update({str(target/key):pin for key,pin in spec['mapped_product_pins'].items()})
    pins[str(target/'manifest.json')]=spec['mapped_manifest_pin']
    if (pins!=admission['dependency_pins'] or products!=admission['products']
            or A.digest(admission['validation'])!=spec['expected_validation_sha256']
            or admission['source_quality_certified'] is not False):
        raise ValueError('mutable short public admission differs')
    _check(pins)


def iter_rows(admission,product,con,batch_size=32):
    if product not in admission['products'] or not 1<=batch_size<=64:
        raise ValueError('fixed short product and bounded batch required')
    import pyarrow.parquet as pq
    _read_check(admission,con)
    try:
        for batch in pq.ParquetFile(admission['products'][product]).iter_batches(batch_size=batch_size):
            yield from batch.to_pylist()
    finally:
        _read_check(admission,con)


@contextlib.contextmanager
def consuming(admission,con):
    _read_check(admission,con)
    try:
        yield admission
    finally:
        _read_check(admission,con)
