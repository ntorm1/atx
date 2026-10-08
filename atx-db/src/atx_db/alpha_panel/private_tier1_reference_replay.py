"""Bounded original French/classification phases with explicit source inputs.

Native reference.build_french:532-569 and classification.build:350-382 bind
global landings. These phases reuse their pure parsers/maps and exact SIC-run
SQL on the caller's connection. Receipt/ZIP/original-code admission is separate;
native fetched clocks and every vintage/approximation flag remain unchanged.
"""
import datetime as dt
import json
import hashlib
from pathlib import Path
import pyarrow as pa
from . import reference as R,classification as I
from . import private_tier1_admission as A,private_tier1_factor_replay as F


def fred_tables(series,release_dates,receipts):
    """Exact reference.build448-515 projection on explicitly bound originals.

    Uses the unchanged native parsers/clock/unit kernels. Current-vintage
    economic rows are replay evidence, never historical-vintage certification.
    """
    if set(series)!=set(R.all_fred_series()) or set(receipts)!=set(series):
        raise ValueError('complete fixed original FRED series required')
    catalog=[]
    for sid in R.all_fred_series():
        obs=series[sid];rec=receipts[sid];comp=R.completeness(obs)
        catalog.append({'series_id':sid,'family':'H10' if sid in R.H10 else ('H15' if sid in R.H15 else 'VIX'),
            'currency':R.H10[sid][0] if sid in R.H10 else None,
            'quote':R.H10[sid][1] if sid in R.H10 else ('percent' if sid in R.H15 else 'index'),
            'description':R.H15[sid][0] if sid in R.H15 else R.VIX.get(sid,f'H.10 {sid}'),
            'first_obs':comp['first_obs'],'last_obs':comp['last_obs'],
            'n_obs':sum(1 for _,v in obs if v is not None),
            'business_days_expected_2010':comp['expected'],'business_days_missing_2010':comp['missing'],
            'missing_days_2010':','.join(comp['missing_days']),
            'last_modified':rec.get('last_modified'),'fetched_at':rec.get('fetched_at')})
    fx=sorted((row for sid in R.H10 for row in R.fx_rows(sid,series[sid],release_dates)),key=lambda row:(row[1],row[0]))
    rates=sorted(((d,sid,R.H15[sid][0],R.H15[sid][1],v,'H.15',R.h15_available_at(d))
        for sid in R.H15 for d,v in series[sid] if v is not None),key=lambda row:(row[1],row[0]))
    vix=[(d,v,'VIXCLS',R.vix_available_at(d)) for d,v in series['VIXCLS'] if v is not None]
    def column(rows,index,typ):return pa.array([row[index] for row in rows],typ)
    return {'fx_daily.parquet':pa.table({
        'obs_date':column(fx,0,pa.date32()),'currency':column(fx,1,pa.string()),
        'usd_per_ccy':column(fx,2,pa.float64()),'series_id':column(fx,3,pa.string()),
        'available_at':column(fx,4,pa.timestamp('us'))}),
        'rates_daily.parquet':pa.table({'obs_date':column(rates,0,pa.date32()),
            'series_id':[row[1] for row in rates],'description':[row[2] for row in rates],
            'tenor_months':column(rates,3,pa.float64()),'value_pct':column(rates,4,pa.float64()),
            'release':[row[5] for row in rates],'available_at':column(rates,6,pa.timestamp('us'))}),
        'vix_daily.parquet':pa.table({'obs_date':column(vix,0,pa.date32()),
            'vix_close':column(vix,1,pa.float64()),'series_id':[row[2] for row in vix],
            'available_at':column(vix,3,pa.timestamp('us'))}),
        'series_catalog.parquet':pa.Table.from_pylist(catalog)}


def replay_fred_sources(con,source_root,ledger,products,scratch,expected_dependencies):
    """Authenticate complete FRED/FRB originals and replay all four products."""
    from . import shortflow_common as S
    root=Path(source_root).resolve();ledger=Path(ledger).resolve()
    required={'fx_daily.parquet','rates_daily.parquet','vix_daily.parquet','series_catalog.parquet'}
    if set(products)!=required:raise ValueError('complete original FRED product inventory required')
    A.check_pins(expected_dependencies)
    dependencies={str(ledger):A.pin(ledger),**A.code(('private_tier1_reference_replay',
        'private_tier1_factor_replay','private_tier1_admission','reference','classification','shortflow_common','common'))}
    latest={};records=0;torn=0
    with ledger.open('rb') as source:
        while line:=source.readline(1024*1024+1):
            if len(line)>1024*1024:raise ValueError('bounded original FRED receipt record exceeded')
            if not line.strip():continue
            try:record=json.loads(line)
            except json.JSONDecodeError:torn+=1;continue
            records+=1
            if 'key' in record:latest[record['key']]=record
    series={};receipts={};facts={}
    keys=['h10_release_page','h10_release_dates',*R.all_fred_series()]
    bodies={}
    for key in keys:
        rec=latest.get(key)
        url=(R.RELEASE_PAGE_URL if key=='h10_release_page' else R.H10_DATES_URL
             if key=='h10_release_dates' else S.full_url(R.FRED_URL,{'id':key,'cosd':R.FRED_START}))
        kind='page' if key=='h10_release_page' else 'frb_release_dates' if key=='h10_release_dates' else 'fred_series'
        if not rec or rec.get('http_status')!=200 or rec.get('url')!=url or rec.get('kind')!=kind:
            raise ValueError('complete original official FRED receipt missing or failed: '+key)
        path=A.member(root,rec['file']);identity=A.pin(path)
        if identity!={'sha256':rec.get('sha256'),'bytes':rec.get('bytes')} or identity['bytes']>16*1024*1024:
            raise ValueError('bounded original FRED bytes differ')
        fetched=dt.datetime.fromisoformat(rec['fetched_at'])
        if fetched.tzinfo is None:raise ValueError('original FRED receipt timezone missing')
        dependencies[str(path)]=identity;body=path.read_bytes()
        if {'sha256':hashlib.sha256(body).hexdigest(),'bytes':len(body)}!=identity:
            raise ValueError('actual consumed original FRED bytes changed')
        facts[key]={'receipt':rec,'original':identity,'source_quality_certified':False}
        if key in ('h10_release_dates','h10_release_page'):bodies[key]=body
        else:
            series[key]=[(d,v) for d,v in R.parse_fred_csv(body.decode('utf-8-sig'),key)
                if d>=dt.date.fromisoformat(R.FRED_START)]
            receipts[key]=rec
    import re
    listed=sorted(set(re.findall(r'/series/(DEX[A-Z]{4})',bodies['h10_release_page'].decode('utf-8','replace'))))
    if listed!=latest['h10_release_page'].get('listed_series') or set(listed)-set(R.H10):
        raise ValueError('complete original H10 mapping/page differs')
    for path in products.values():dependencies[str(Path(path).resolve())]=A.pin(path)
    if dependencies!=expected_dependencies:raise ValueError('complete independent FRED dependency inventory differs')
    try:
        dates=R.parse_release_dates(bodies['h10_release_dates'])
        if not dates:raise ValueError('original FRB release calendar empty')
        measured={}
        for name,table in fred_tables(series,dates,receipts).items():
            rows=(row for batch in table.to_batches(max_chunksize=64) for row in batch.to_pylist())
            measured[name]=F.compare_unit(con,rows,table.schema,products[name],Path(scratch)/name)
        return {'status':'full_original_native_replay','products':products,'dependency_pins':dependencies,
            'receipt_records':records,'torn_records':torn,'sources':facts,'validation':measured,
            'source_quality_certified':False}
    finally:A.check_pins(expected_dependencies)


def french_table(text,fetched,key):
    if key not in R.FRENCH_FACTOR_FILES:raise ValueError('fixed original French product required')
    if fetched.tzinfo is None:raise ValueError('original French receipt timezone required')
    fetched=fetched.astimezone(dt.UTC).replace(tzinfo=None)
    cols,rows,vintage=R.parse_french_table(text)
    names=[R.factor_column(column) for column in cols]
    datecol='date' if key.endswith('daily') else 'month'
    data={datecol:pa.array([row[0] for row in rows],pa.date32())}
    for index,name in enumerate(names):
        data[name]=pa.array([row[1][index] if index<len(row[1]) else None for row in rows],pa.float64())
    data['vintage']=[vintage]*len(rows)
    data['available_at']=pa.array([fetched]*len(rows),pa.timestamp('us'))
    data['vintage_risk']=[True]*len(rows)
    return pa.table(data)


def sic_table(sources):
    if set(sources)!=set(R.SICCODE_SCHEMES):raise ValueError('all original French SIC schemes required')
    rows=[];clocks={}
    for scheme in R.SICCODE_SCHEMES:
        text,fetched=sources[scheme]
        if fetched.tzinfo is None:raise ValueError('original SIC receipt timezone required')
        clocks[scheme]=fetched.astimezone(dt.UTC).replace(tzinfo=None)
        rows.extend(R.parse_siccodes(text,scheme))
    names=('scheme','industry_no','industry_short','industry_name','sic_lo','sic_hi','range_desc')
    data={name:[row[index] for row in rows] for index,name in enumerate(names)}
    data['available_at']=pa.array([clocks[row[0]] for row in rows],pa.timestamp('us'))
    return pa.table(data)


def sic_runs_query(relation):
    """Exact classification.sic_runs_sql:270-279 with explicit relation."""
    return f'''SELECT cik,clock_utc,sic,accession,sic_basis FROM (
        SELECT *,lag(sic) OVER(PARTITION BY cik ORDER BY clock_utc,accession) AS prev_sic
        FROM {relation} WHERE sic>0)
        WHERE prev_sic IS NULL OR prev_sic<>sic ORDER BY cik,clock_utc'''


def industry_rows(con,sic_relation,siccodes,concordances):
    """Complete unchanged maps, streaming SIC runs instead of full Python history."""
    if set(concordances)!={'sic87_naics02',*I.CHAIN}:
        raise ValueError('all original Census concordance stages required')
    rows02=I.concordance_rows(concordances['sic87_naics02'])
    pm02=I.primary_map(rows02)
    steps=[I.primary_map(I.concordance_rows(concordances[key])) for key in I.CHAIN]
    if con.execute('SELECT count(DISTINCT sic) FROM '+sic_relation+' WHERE sic>0').fetchone()[0]>10000:
        raise ValueError('original SIC domain bound exceeded')
    sics=[row[0] for row in con.execute('SELECT DISTINCT sic FROM '+sic_relation+' WHERE sic>0 ORDER BY sic').fetchall()]
    mapped=I.sic_map_rows(sics,siccodes,pm02,steps,I.candidates_map(rows02),{code:title for _,_,code,title in rows02})
    by_sic={row['sic']:row for row in mapped}
    cursor=con.cursor()
    try:
        cursor.execute(sic_runs_query(sic_relation))
        while batch:=cursor.fetchmany(64):
            for cik,clock,sic,accession,basis in batch:
                yield {'cik':cik,'available_at':clock,'sic':sic,'accession':accession,'sic_basis':basis,
                    **{key:value for key,value in by_sic[sic].items() if key!='sic'}}
    finally:cursor.close()


def replay_french_sources(con,source_root,ledger,products,scratch,expected_dependencies):
    """Full fixed French retained-member receipt and native product replay.

    Original reference.fetch deletes ZIPs and retains original members. This
    reader preserves that limitation; independently pinned member/HTTP receipts
    establish native byte lineage, while revision/first-vintage qualification
    remains explicit. It never fabricates historical publication timestamps.
    """
    A.check_pins(expected_dependencies)
    root=Path(source_root).resolve();ledger=Path(ledger).resolve()
    required={key+'.parquet' for key in R.FRENCH_FACTOR_FILES}|{'siccodes.parquet'}
    if set(products)!=required:raise ValueError('complete original French native product inventory required')
    dependencies={str(ledger):A.pin(ledger),**A.code(('private_tier1_reference_replay','private_tier1_factor_replay','private_tier1_admission','reference','classification','shortflow_common','common'))}
    # The unchanged ledger rule is last record per key, including a last failure.
    # Stream its metadata instead of accumulating every prior failed attempt.
    latest={};receipt_records=0;torn_records=0;ledger_hash=hashlib.sha256();ledger_bytes=0
    with ledger.open('rb') as source:
        while line:=source.readline(1024*1024+1):
            ledger_hash.update(line);ledger_bytes+=len(line)
            if len(line)>1024*1024:raise ValueError('bounded original French receipt record exceeded')
            if not line.strip():continue
            try:record=json.loads(line)
            except json.JSONDecodeError:torn_records+=1;continue
            receipt_records+=1
            if 'key' in record:latest[record['key']]=record
    if {'sha256':ledger_hash.hexdigest(),'bytes':ledger_bytes}!=dependencies[str(ledger)]:
        raise ValueError('actual consumed original French receipt bytes changed')
    keys={value:key for key,value in R.FRENCH_FACTOR_FILES.items()}
    keys.update({f'Siccodes{scheme}.zip':scheme for scheme in R.SICCODE_SCHEMES})
    texts={};facts={}
    for name in keys:
        record=latest.get(name)
        if (not record or record.get('http_status')!=200 or record.get('kind')!='french_zip'
                or record.get('url')!=R.FRENCH_BASE+name or not record.get('file')
                or type(record.get('bytes')) is not int or record['bytes']<=0
                or not isinstance(record.get('sha256'),str) or len(record['sha256'])!=64
                or any(character not in '0123456789abcdef' for character in record['sha256'])):
            raise ValueError('complete original official French receipt missing or failed: '+name)
        members=record.get('members',{})
        if len(members)!=1:raise ValueError('original French single-member receipt required')
        member,identity=next(iter(members.items()))
        path=A.member(A.member(root,record['file']),member)
        actual=A.pin(path)
        if actual!=identity:raise ValueError('original French retained member differs from receipt')
        if actual['bytes']>8*1024*1024:raise ValueError('bounded original French member exceeds8MiB')
        dependencies[str(path)]=actual
        fetched=dt.datetime.fromisoformat(record['fetched_at'])
        if fetched.tzinfo is None:raise ValueError('original French receipt timezone missing')
        texts[name]=(path,fetched)
        facts[name]={'receipt':record,'retained_member':actual,'native_vintage_risk':True,
            'native_zip_deleted_declaration':record.get('zip_deleted'),'original_zip_bytes_admitted':False,
            'source_quality_certified':False}
    for path in products.values():dependencies[str(Path(path).resolve())]=A.pin(path)
    if dependencies!=expected_dependencies:raise ValueError('complete independent French dependency inventory differs')
    A.check_pins(dependencies);measured={}
    def original_text(path):
        body=path.read_bytes()
        if {'sha256':hashlib.sha256(body).hexdigest(),'bytes':len(body)}!=dependencies[str(path)]:
            raise ValueError('actual consumed original French member bytes changed')
        return body.decode('latin-1')
    def table_rows(table):
        for batch in table.to_batches(max_chunksize=64):yield from batch.to_pylist()
    for key,name in R.FRENCH_FACTOR_FILES.items():
        path,fetched=texts[name]
        table=french_table(original_text(path),fetched,key)
        leaf=key+'.parquet'
        measured[leaf]=F.compare_unit(con,table_rows(table),table.schema,products[leaf],Path(scratch)/leaf)
    schemes={scheme:(original_text(texts[f'Siccodes{scheme}.zip'][0]),texts[f'Siccodes{scheme}.zip'][1]) for scheme in R.SICCODE_SCHEMES}
    table=sic_table(schemes)
    measured['siccodes.parquet']=F.compare_unit(con,table_rows(table),table.schema,products['siccodes.parquet'],Path(scratch)/'siccodes.parquet')
    A.check_pins(dependencies)
    return {'status':'full_original_native_replay','products':products,'dependency_pins':dependencies,
        'receipt_records':receipt_records,'torn_records':torn_records,'sources':facts,
        'validation':measured,'source_quality_certified':False}
