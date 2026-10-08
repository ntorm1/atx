"""Explicit connection projections of frozen capital/governance event SQL.

These internal phases require separate complete original-source admission.
Native full-history selection, nearest-30d links, future auditor observations
and vintage flags remain visible diagnostics; none grants PIT qualification.
No producer globals, resource settings or publication paths are replaced.
"""
import re
import contextlib
import datetime as dt
import gzip
import hashlib
import json
from pathlib import Path
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
from . import events_common as EC, events_capital as K, events_governance as G
from . import private_tier1_admission as A
from . import events_sources as ES, private_tier1_factor_replay as F
from . import sec_filings as SF
from . import filing_text_native as FT
from .. import sec_http as SH
from .. import _submissions_archive as SA
from . import private_tier1_ownership_exception_sources as OX


def code_pins():
    return A.code(('private_tier1_event_replay','private_tier1_admission',
        'events_common','events_capital','events_governance','events_buyback','events_sources','sec_filings',
        'private_tier1_ownership_exception_sources','common'))


def extract_sec_raw(con,archive_path,receipt_path,target,expected_dependencies):
    """Complete main/history native flattening, with bounded64-row Arrow writes.

    Uses the authenticated existing read-only original directory and unchanged
    ZIP/CRC and SEC flatten/profile kernels. Only the exact two reviewed mains
    may exceed4MiB (private5MiB bound). No native index construction or source
    mutation. This extracts originals; it does not certify modeled clocks.
    """
    A.guard(con)
    supplied_dependencies=expected_dependencies
    expected_dependencies={path:dict(pin) for path,pin in supplied_dependencies.items()}
    A.check_pins(expected_dependencies)
    archive_path=A.absolute(archive_path);receipt_path=A.absolute(receipt_path);target=A.absolute(target)
    if target.exists():raise ValueError('preserve existing original SEC extraction attempt')
    dependencies={str(archive_path):A.pin(archive_path),str(receipt_path):A.pin(receipt_path),
        **code_pins(),str(Path(SA.__file__).resolve()):A.pin(SA.__file__)}
    receipt=json.loads(receipt_path.read_text())
    if (receipt.get('url')!='https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip'
            or receipt.get('http_status',receipt.get('status'))!=200 or receipt.get('error')
            or any(receipt.get(key)!=dependencies[str(archive_path)][key] for key in ('sha256','bytes'))):
        raise ValueError('complete original SEC archive receipt required')
    retrieved=dt.datetime.fromisoformat(receipt['fetched_at'].replace('Z','+00:00'))
    if retrieved.tzinfo is None:raise ValueError('original SEC retrieval clock requires timezone')
    if con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name='t17_event_history_refs'").fetchone()[0]:
        raise ValueError('original SEC reference namespace occupied')
    con.execute('CREATE TABLE t17_event_history_refs(name VARCHAR)')
    target.mkdir(parents=True);counts={'main':0,'history':0,'raw_rows':0,'profile_rows':0}
    files={'raw_filings.parquet':target/'raw_filings.parquet','raw_profiles.parquet':target/'raw_profiles.parquet'}
    try:
        with OX._ReadOnlyArchive(archive_path) as archive:
            if archive.sha256!=dependencies[str(archive_path)]['sha256']:
                raise ValueError('actual consumed original archive differs')
            for path in (archive.index_path,archive._receipt_path):dependencies[str(path)]=A.pin(path)
            if dependencies!=expected_dependencies:raise ValueError('complete original SEC archive/code/directory inventory differs')
            connection=archive._connection()
            if connection.execute('SELECT count(*) FROM members').fetchone()[0]!=connection.execute('SELECT count(*) FROM offsets').fetchone()[0]:
                raise ValueError('duplicate original ZIP names require explicit native replay')
            with (pq.ParquetWriter(files['raw_filings.parquet'],SF.RAW_FILING_SCHEMA,compression='zstd') as fw,
                  pq.ParquetWriter(files['raw_profiles.parquet'],SF.PROFILE_SCHEMA,compression='zstd') as pw):
                cursor=connection.execute('SELECT name,record FROM members WHERE kind IN(1,2) ORDER BY ordinal')
                try:
                    for name,central in cursor:
                        kind=SF.member_kind(name)
                        if kind is None:raise ValueError('native original member classification differs')
                        cik,source=kind;info=SA._one_info(central)
                        maximum=4*1024**2
                        if cik in OX.EXCEPTIONS and (name,info.file_size)==OX.EXCEPTIONS[cik]:maximum=5*1024**2
                        if info.file_size>maximum:raise ValueError('bounded complete original SEC member exceeded')
                        document=json.loads(archive.read(name));block=SF.columnar_block(document,source)
                        if source==0:
                            counts['main']+=1
                            pw.write_table(pa.Table.from_pylist([SF.profile_row(document,cik)],schema=SF.PROFILE_SCHEMA))
                            counts['profile_rows']+=1
                            refs=(document.get('filings') or {}).get('files') or []
                            for reference in refs:
                                declared=reference['name'];matched=SF.member_kind(declared)
                                if not matched or matched[0]!=cik or matched[1]==0 or declared not in archive:
                                    raise ValueError('complete original declared history missing or mismatched')
                                con.execute('INSERT INTO t17_event_history_refs VALUES(?)',[declared])
                        else:counts['history']+=1
                        n=len(block.get('accessionNumber') or [])
                        for offset in range(0,n,64):
                            sliced={key:(block[key][offset:offset+64] if block.get(key) is not None else None)
                                for key,_ in SF.FILING_FIELDS}
                            columns=SF.flatten_filings(sliced,cik,source)
                            table=pa.Table.from_pydict(columns,schema=SF.RAW_FILING_SCHEMA)
                            date=table.column('filing_date')
                            keep=pc.and_(pc.greater_equal(date,SF.RAW_START.isoformat()),pc.less_equal(date,SF.FILING_END.isoformat()))
                            table=table.filter(pc.fill_null(keep,False));fw.write_table(table)
                            counts['raw_rows']+=table.num_rows
                finally:cursor.close()
            archive.assert_unchanged()
        return {'status':'complete_original_SEC_extraction','products':files,'population':counts,
            'dependency_pins':dependencies,'source_quality_certified':False,'qualification':'raw_acceptance_unqualified'}
    finally:
        try:
            A.check_pins(expected_dependencies);A.guard(con)
            if supplied_dependencies!=expected_dependencies:
                raise ValueError('original SEC extraction dependency binding changed')
        finally:con.execute('DROP TABLE t17_event_history_refs')


@contextlib.contextmanager
def sec_filings_scope(con,raw_relation,dependency_pins):
    """Consume complete admitted raw originals in one authenticated transaction.

    The caller owns the original-source admission and relation. This scope only
    provides the unchanged diagnostic native assembly; it cannot mint Accepted,
    body coverage, issuer completeness or historical source qualification.
    """
    A.guard(con);raw_relation=identifier(raw_relation)
    pins={path:dict(pin) for path,pin in dependency_pins.items()}
    if any(pins.get(path)!=pin for path,pin in code_pins().items()):
        raise ValueError('complete original SEC assembly code pins required')
    reserved=('raw','clock_files','filings0','filings','items','_rr','_cons','_disagree','_dups')
    existing={row[0] for row in con.execute('SHOW TABLES').fetchall()}
    if existing.intersection(reserved) or raw_relation not in existing:
        raise ValueError('original SEC assembly relation namespace invalid')
    A.check_pins(pins)
    try:
        queries=assemble_sec_filings(con,raw_relation)
        yield dict(queries)
    finally:
        try:
            A.check_pins(pins);A.guard(con)
            if dependency_pins!=pins:
                raise ValueError('original SEC assembly dependency binding changed')
        finally:
            for table in reserved:con.execute('DROP TABLE IF EXISTS '+table)


def assemble_sec_filings(con,raw_relation):
    """Native SEC assemble787-827 on the complete explicitly bound raw cohort.

    Returns diagnostic filings/items relations. Native per-file heuristic,
    duplicate precedence and unresolved/fallback clocks remain unchanged;
    separate original Accepted-index qualification is mandatory for public use.
    """
    raw_relation=identifier(raw_relation)
    con.execute(f'''CREATE TABLE raw AS
        SELECT cik,source,accession,nullif(form,'') AS form,try_cast(filing_date AS DATE) AS filing_date,
            {SF.ACC_KEY_SQL} AS acc_key,{SF.ACCEPT_SQL} AS ts,
            {SF.UTC_TO_ET.format(x=SF.ACCEPT_SQL)} AS tu,acceptance_raw,
            try_cast(nullif(report_date,'') AS DATE) AS report_date,nullif(items_raw,'') AS items,
            upper(coalesce(form,'')) LIKE '%/A' AS is_amendment,nullif(primary_document,'') AS primary_document,
            nullif(file_number,'') AS file_number,nullif(act,'') AS act,is_xbrl=1 AS is_xbrl,
            is_inline_xbrl=1 AS is_inline_xbrl,size FROM {raw_relation}''')
    con.execute('CREATE TABLE clock_files AS '+SF.clock_decision_sql('('+SF.clock_evidence_sql('raw')+')'))
    SF.resolve_filings(con,'raw','clock_files','filings0')
    con.execute('DROP TABLE raw')
    con.execute(f'''CREATE TABLE filings AS
        SELECT *,coalesce(acceptance_utc,{SF.EOD_SQL}) AS available_at,
            CASE WHEN acceptance_utc IS NULL THEN 'filing_date_eod_et' ELSE 'acceptance' END AS available_basis
        FROM filings0''')
    con.execute('DROP TABLE filings0')
    con.execute('CREATE TABLE items AS '+SF.items_explode_sql('filings',
        'cik, accession, form, filing_date, acceptance_utc, available_at, report_date, is_amendment, acceptance_clock, vintage_risk',
        f"filing_date BETWEEN DATE '{SF.FILING_START}' AND DATE '{SF.FILING_END}' AND upper(regexp_replace(form,'/A$','')) LIKE '8-K%'"))
    return {'filings':f"SELECT * FROM filings WHERE filing_date BETWEEN DATE '{SF.FILING_START}' AND DATE '{SF.FILING_END}' ORDER BY filing_date,cik",
        'items':'SELECT * FROM items ORDER BY filing_date,cik,accession,item',
        'clock_files':'SELECT * FROM clock_files ORDER BY cik,source'}


@contextlib.contextmanager
def accepted_clock_scope(con,source_root,filings,plan,expected_plan_sha256,dependency_pins):
    """Bounded original Accepted evidence overlay on the full filing population.

    Missing evidence remains an explicit row. Native modeled clocks are retained
    separately; neither this overlay nor an index transport receipt establishes
    complete filing bodies, historical identity, or public source qualification.
    The public source adapter must admit the entire source plan independently.
    """
    A.guard(con);root=A.absolute(source_root);filings=identifier(filings)
    sealed=json.loads(json.dumps(plan));pins={path:dict(pin) for path,pin in dependency_pins.items()}
    if A.digest(sealed)!=expected_plan_sha256 or set(sealed)!={'files','acceptance_proofs'}:
        raise ValueError('independent fixed original Accepted plan required')
    referenced={proof[field] for proof in sealed['acceptance_proofs'] for field in ('index_path','receipt')}
    if referenced!=set(sealed['files']):
        raise ValueError('complete exact original Accepted file inventory required')
    required={**code_pins(),**A.code(('filing_text_native','filing_text'))}
    for relative,identity in sealed['files'].items():required[str(A.member(root,relative))]=identity
    A.check_pins(pins,required)
    existing={row[0] for row in con.execute('SHOW TABLES').fetchall()}
    table='t17_event_original_accepted'
    if table in existing or filings not in existing:
        raise ValueError('original Accepted scope namespace invalid')
    con.execute(f'CREATE TABLE {table}(cik BIGINT,accession VARCHAR,accepted_at TIMESTAMPTZ,reason VARCHAR,PRIMARY KEY(cik,accession))')
    try:
        for proof in sealed['acceptance_proofs']:
            key=(int(proof['cik']),proof['accession'])
            # Each unchanged native proof is bounded to one original index;
            # complete source hashing occurs only around the whole scope.
            clocks=FT.acceptance_proofs(root,{'files':sealed['files'],'acceptance_proofs':[proof]})
            clock=clocks[key]
            receipt=FT.receipt_json(root,proof['receipt'])
            try:
                fetched=dt.datetime.fromisoformat(receipt['fetched_at'].replace('Z','+00:00'))
                if fetched.tzinfo is None or (clock is not None and fetched<clock):clock=None
            except (KeyError,TypeError,ValueError):clock=None
            con.execute(f'INSERT INTO {table} VALUES(?,?,?,?)',
                [*key,clock,None if clock is not None else 'original_Accepted_identity_or_clock_unresolved'])
        query=f'''SELECT f.*,a.accepted_at AS original_accepted_at,
            (a.accepted_at IS NOT NULL) AS original_accepted_clock_supported,
            CASE WHEN a.accession IS NULL THEN 'original_Accepted_evidence_missing'
                 ELSE a.reason END AS original_accepted_reason
            FROM {filings} f LEFT JOIN {table} a USING(cik,accession)'''
        yield query
    finally:
        try:
            A.check_pins(pins,required);A.guard(con)
            if dependency_pins!=pins or A.digest(plan)!=expected_plan_sha256:
                raise ValueError('original Accepted consumption binding changed')
        finally:con.execute('DROP TABLE IF EXISTS '+table)


class LedgerDocuments:
    """Transaction-local original FetchLedgerStore body access, no global store."""
    def __init__(self,con,root):self.con=con;self.root=root

    def rows(self):
        cursor=self.con.cursor()
        try:
            cursor.execute('SELECT url,sha256,body_path,body_present FROM t17_event_body_registry ORDER BY url')
            while batch:=cursor.fetchmany(64):
                for values in batch:yield dict(zip(('url','sha256','body_path','body_present'),values))
        finally:cursor.close()

    def read(self,url,expected_sha256):
        row=self.con.execute('SELECT record FROM t17_event_body_terminal WHERE url=?',[url]).fetchone()
        if not row:return None
        record=SH._record_from_json(json.loads(row[0]))
        if not record.ok:return None
        if record.sha256!=expected_sha256 or record.bytes>ES.MAX_DOCUMENT_BYTES:
            raise ValueError('original event body/current terminal identity differs')
        stored=self.con.execute('SELECT body_path FROM t17_event_body_registry WHERE url=?',[url]).fetchone()
        if not stored or stored[0] is None:return None
        path=A.absolute(stored[0]);blob=_bounded_event_body(path)
        if len(blob)!=record.bytes or hashlib.sha256(blob).hexdigest()!=record.sha256:
            raise ValueError('actual consumed original event body differs')
        return blob


def _bounded_event_body(path):
    if path.stat().st_size>ES.MAX_DOCUMENT_BYTES+65536:
        raise ValueError('stored original event body bound exceeded')
    with (gzip.open(path,'rb') if path.suffix=='.gz' else path.open('rb')) as stream:
        raw=stream.read(ES.MAX_DOCUMENT_BYTES+1)
    if len(raw)>ES.MAX_DOCUMENT_BYTES:raise ValueError('decoded original event body bound exceeded')
    return raw


class LandedDocuments:
    """Separate Task9 latest-success registry; later failures do not erase it."""
    def __init__(self,con,root):self.con=con;self.root=root

    def rows(self):
        cursor=self.con.cursor()
        try:
            cursor.execute('SELECT accession,file,sha,body_path,body_present FROM t17_event_landed_registry ORDER BY accession')
            while batch:=cursor.fetchmany(64):
                for values in batch:yield dict(zip(('accession','file','sha','body_path','body_present'),values))
        finally:cursor.close()

    def read(self,sha256):
        if not re.fullmatch('[0-9a-f]{64}',sha256):raise ValueError('original landed body SHA invalid')
        row=self.con.execute('SELECT body_path FROM t17_event_landed_registry WHERE sha=? LIMIT 1',[sha256]).fetchone()
        if not row or row[0] is None:return None
        blob=_bounded_event_body(A.absolute(row[0]))
        if hashlib.sha256(blob).hexdigest()!=sha256:raise ValueError('original landed body identity differs')
        return blob


def _event_object_path(root,sha):
    if not re.fullmatch('[0-9a-f]{64}',sha):raise ValueError('original body registry SHA invalid')
    directory=A.member(root,'objects/'+sha[:2])
    compressed,plain=directory/(sha+'.gz'),directory/sha
    selected=compressed if compressed.exists() else plain
    return selected if selected.exists() else None


@contextlib.contextmanager
def landed_body_scope(con,source_root,dependency_pins):
    """Replay the original Task9 units-done role independently of fetch ledger.

    Complete successful accession/exhibit selection follows ES497-512; all
    attempts remain authenticated in the original ledger. Failed later attempts
    never replace the last document_ok. Missing objects stay registry rows.
    This transport replay does not qualify Accepted clocks or Task9 publication.
    """
    A.guard(con);root=A.absolute(source_root)
    pins={path:dict(pin) for path,pin in dependency_pins.items()}
    ledger=A.member(root,SH.UNITS_DONE_NAME)
    required={str(ledger):A.pin(ledger),**code_pins(),str(Path(SH.__file__).resolve()):A.pin(SH.__file__)}
    A.check_pins(pins,required)
    owned=('t17_event_landed_attempts','t17_event_landed_registry')
    if {row[0] for row in con.execute('SHOW TABLES').fetchall()}.intersection(owned):
        raise ValueError('original landed body scope namespace occupied')
    con.execute('CREATE TABLE t17_event_landed_attempts(seq BIGINT,accession VARCHAR,file VARCHAR,sha VARCHAR)')
    try:
        batch=[];seq=0
        with ledger.open('rb') as stream:
            while line:=stream.readline(1024*1024+1):
                if len(line)>1024*1024:raise ValueError('original units-done record bound exceeded')
                seq+=1
                try:record=json.loads(line)
                except ValueError:continue
                if record.get('outcome')=='document_ok' and record.get('document_sha256'):
                    batch.append((seq,str(record['accession']),str(record.get('exhibit') or ''),str(record['document_sha256'])))
                    if len(batch)==32:
                        con.executemany('INSERT INTO t17_event_landed_attempts VALUES(?,?,?,?)',batch);batch.clear()
        if batch:con.executemany('INSERT INTO t17_event_landed_attempts VALUES(?,?,?,?)',batch)
        con.execute('''CREATE TABLE t17_event_landed_registry AS SELECT accession,file,sha,
            CAST(NULL AS VARCHAR) body_path,false body_present FROM(
            SELECT *,row_number() OVER(PARTITION BY accession ORDER BY seq DESC) rn
            FROM t17_event_landed_attempts) WHERE rn=1''')
        reader=LandedDocuments(con,root)
        for row in reader.rows():
            path=_event_object_path(root,row['sha'])
            if path is None:continue
            required[str(path)]=A.pin(path)
            if pins.get(str(path))!=required[str(path)]:raise ValueError('complete original landed body inventory omitted')
            blob=_bounded_event_body(path)
            if hashlib.sha256(blob).hexdigest()!=row['sha']:raise ValueError('original landed registry body differs')
            con.execute('UPDATE t17_event_landed_registry SET body_path=?,body_present=true WHERE accession=?',[str(path),row['accession']])
        yield reader
    finally:
        try:
            A.check_pins(pins,required);A.guard(con)
            if dependency_pins!=pins:raise ValueError('original landed consumption binding changed')
            if con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name='t17_event_landed_registry'").fetchone()[0]:
                for row in LandedDocuments(con,root).rows():
                    path=_event_object_path(root,row['sha'])
                    if (str(path) if path is not None else None)!=row['body_path']:
                        raise ValueError('original landed body presence/selection changed')
        finally:
            for table in owned:con.execute('DROP TABLE IF EXISTS '+table)


@contextlib.contextmanager
def event_body_scope(con,source_root,dependency_pins):
    """Full native SEC events body ledger, distinct from annual FT receipts.

    Replays every ledger attempt into disk tables; the native catalog's latest
    successful body axis and store's latest terminal read semantics are both
    retained. Missing bodies remain rows. This never fetches/quarantines bytes,
    admits a partial corpus, or infers source qualification from transport.
    """
    A.guard(con);root=A.absolute(source_root)
    pins={path:dict(pin) for path,pin in dependency_pins.items()}
    ledger=A.member(root,SH.FETCH_LEDGER_NAME)
    required={str(ledger):A.pin(ledger),**code_pins(),str(Path(SH.__file__).resolve()):A.pin(SH.__file__)}
    A.check_pins(pins,required)
    reserved=('t17_event_body_attempts','t17_event_body_terminal','t17_event_body_registry')
    if {row[0] for row in con.execute('SHOW TABLES').fetchall()}.intersection(reserved):
        raise ValueError('original event body scope namespace occupied')
    con.execute('CREATE TABLE t17_event_body_attempts(seq BIGINT,url VARCHAR,record VARCHAR,terminal BOOLEAN,ok BOOLEAN,sha256 VARCHAR)')
    try:
        batch=[];sequence=0
        with ledger.open('rb') as stream:
            while line:=stream.readline(1024*1024+1):
                if len(line)>1024*1024:raise ValueError('bounded original event body ledger record exceeded')
                if not line.strip():continue
                sequence+=1
                try:record=SH._record_from_json(json.loads(line))
                except (KeyError,TypeError,ValueError):continue
                batch.append((sequence,record.url,line.decode(),record.terminal,record.ok,record.sha256))
                if len(batch)==32:
                    con.executemany('INSERT INTO t17_event_body_attempts VALUES(?,?,?,?,?,?)',batch);batch.clear()
        if batch:con.executemany('INSERT INTO t17_event_body_attempts VALUES(?,?,?,?,?,?)',batch)
        con.execute('''CREATE TABLE t17_event_body_terminal AS SELECT * EXCLUDE(rn) FROM(
            SELECT *,row_number() OVER(PARTITION BY url ORDER BY seq DESC) rn
            FROM t17_event_body_attempts WHERE terminal) WHERE rn=1''')
        con.execute('''CREATE TABLE t17_event_body_registry AS SELECT url,sha256,
            CAST(NULL AS VARCHAR) body_path,false body_present FROM(
            SELECT *,row_number() OVER(PARTITION BY url ORDER BY seq DESC) rn
            FROM t17_event_body_attempts WHERE ok AND starts_with(url,?)) WHERE rn=1''',[ES.ARCHIVES])
        reader=LedgerDocuments(con,root)
        for row in reader.rows():
            sha=row['sha256'];directory=A.member(root,'objects/'+sha[:2])
            compressed,plain=directory/(sha+'.gz'),directory/sha
            path=compressed if compressed.exists() else plain
            if not path.exists():continue
            required[str(path)]=A.pin(path)
            if pins.get(str(path))!=required[str(path)]:raise ValueError('complete original event body inventory omitted')
            # Native catalog can still reference an old200 after a terminal404;
            # authenticate those original bytes without granting a current read.
            blob=_bounded_event_body(path)
            if hashlib.sha256(blob).hexdigest()!=sha:raise ValueError('original catalog body identity differs')
            con.execute('UPDATE t17_event_body_registry SET body_path=?,body_present=true WHERE url=?',[str(path),row['url']])
        yield reader
    finally:
        try:
            A.check_pins(pins,required);A.guard(con)
            if dependency_pins!=pins:raise ValueError('original event body consumption binding changed')
            if con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name='t17_event_body_registry'").fetchone()[0]:
                for row in LedgerDocuments(con,root).rows():
                    directory=A.member(root,'objects/'+row['sha256'][:2])
                    compressed,plain=directory/(row['sha256']+'.gz'),directory/row['sha256']
                    selected=compressed if compressed.exists() else plain
                    actual=str(selected) if selected.exists() else None
                    if actual!=row['body_path']:
                        raise ValueError('original event body presence/selection changed during consumption')
        finally:
            for table in reserved:con.execute('DROP TABLE IF EXISTS '+table)


def replay_query_corpus(name,con,source_root,products,scratch,expected_dependencies):
    """Complete original fixed FTS URL/window/page-axis native hit replay.

    Retains every failed ledger attempt in authenticated original bytes, uses
    unchanged terminal-record rules and native URL/parse/dedup/window splitting.
    No network, quarantine, mutable store loader or selected-document shortcut.
    """
    required=set(K.CONVERT_QIDS)|{'spinoff'} if name=='capital' else {
        query for group in G.DEPART_QIDS.values() for query in group}|{'going_concern'} if name=='governance' else set(ES.QUERIES) if name=='catalog' else None
    if required is None or set(products)!=required:raise ValueError('complete fixed event query products required')
    A.guard(con);A.check_pins(expected_dependencies)
    root=A.absolute(source_root);ledger=A.member(root,SH.FETCH_LEDGER_NAME)
    dependencies={str(ledger):A.pin(ledger),**code_pins(),str(Path(SH.__file__).resolve()):A.pin(SH.__file__),
        **A.code(('private_tier1_factor_replay',))}
    if con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name='t17_event_ledger'").fetchone()[0]:
        raise ValueError('event original ledger replay namespace occupied')
    con.execute('CREATE TABLE t17_event_ledger(seq BIGINT,url VARCHAR,record VARCHAR)')
    attempts=0;corrupt=0;failed=0;stats={}
    try:
        batch=[]
        with ledger.open('rb') as stream:
            while line:=stream.readline(1024*1024+1):
                if len(line)>1024*1024:raise ValueError('bounded original event ledger record exceeded')
                if not line.strip():continue
                attempts+=1
                try:record=SH._record_from_json(json.loads(line))
                except (ValueError,KeyError,TypeError):corrupt+=1;continue
                failed+=not record.ok
                if record.terminal:
                    batch.append((attempts,record.url,line.decode('utf-8')))
                    if len(batch)==32:
                        con.executemany('INSERT INTO t17_event_ledger VALUES(?,?,?)',batch);batch.clear()
        if batch:con.executemany('INSERT INTO t17_event_ledger VALUES(?,?,?)',batch)
        def page(url):
            row=con.execute('SELECT record FROM t17_event_ledger WHERE url=? ORDER BY seq DESC LIMIT 1',[url]).fetchone()
            if not row:raise ValueError('complete original event query page absent: '+url)
            record=SH._record_from_json(json.loads(row[0]))
            if not record.ok or record.bytes>ES.MAX_FTS_BYTES:raise ValueError('original event query page failed/oversized')
            compressed=A.member(root,'objects/'+record.sha256[:2]+'/'+record.sha256+'.gz')
            raw=A.member(root,'objects/'+record.sha256[:2]+'/'+record.sha256)
            path=compressed if compressed.is_file() else raw
            if path.stat().st_size>ES.MAX_FTS_BYTES+65536:
                raise ValueError('bounded original event object at rest exceeded')
            identity=A.pin(path)
            if expected_dependencies.get(str(path))!=identity:raise ValueError('independent original event page pin differs')
            dependencies[str(path)]=identity
            with (gzip.open(path,'rb') if path==compressed else path.open('rb')) as stream:
                body=stream.read(ES.MAX_FTS_BYTES+1)
            if len(body)!=record.bytes or hashlib.sha256(body).hexdigest()!=record.sha256:
                raise ValueError('actual consumed original event query page differs')
            return json.loads(body)
        for qid in sorted(required):
            forms,query=ES.QUERIES[qid];summary={'windows':0,'hits_total':0,'hits_unique':0,'short_windows':[],
                'censored_windows':[]}
            def rows():
                wins=ES.month_windows(ES.WINDOW_START,ES.WINDOW_END) if qid in ES.MONTHLY_QUERIES else ES.year_windows(ES.WINDOW_START,ES.WINDOW_END)
                stack=list(reversed(wins))
                while stack:
                    start,end=stack.pop();first=page(ES.fts_url(query,forms,start,end))
                    total_info=(first.get('hits') or {}).get('total') or {}
                    total=int(total_info.get('value') or 0);relation=total_info.get('relation','eq')
                    if (relation!='eq' or total>ES.FTS_CEILING-ES.FTS_PAGE) and end>start:
                        mid=start+(end-start)//2;stack.extend([(mid+dt.timedelta(days=1),end),(start,mid)]);continue
                    if relation!='eq' or total>ES.FTS_CEILING:
                        summary['censored_windows'].append([start.isoformat(),end.isoformat(),total,relation])
                    pages=[first];seen=set();unique=0
                    offsets=iter(range(ES.FTS_PAGE,min(total,ES.FTS_CEILING),ES.FTS_PAGE))
                    while pages:
                        payload=pages.pop();parsed=ES.parse_hits(payload,qid,(start,end))
                        if len(parsed)>ES.FTS_PAGE:raise ValueError('original FTS page population exceeds native page bound')
                        for item in parsed:
                            key=(item['adsh'],item['file'])
                            if key not in seen:
                                seen.add(key);unique+=1;yield item
                        offset=next(offsets,None)
                        if offset is not None:pages.append(page(ES.fts_url(query,forms,start,end,offset)))
                    summary['windows']+=1;summary['hits_total']+=total;summary['hits_unique']+=unique
                    if unique<min(total,ES.FTS_CEILING):summary['short_windows'].append([start.isoformat(),end.isoformat(),total,unique])
            stats[qid]={'validation':F.compare_unit(con,rows(),ES.HIT_SCHEMA,products[qid],Path(scratch)/(qid+'.parquet')),'population':summary}
            dependencies[str(Path(products[qid]).resolve())]=A.pin(products[qid])
        if dependencies!=expected_dependencies:raise ValueError('complete independent original event corpus inventory differs')
        return {'status':'full_original_native_replay','queries':stats,'dependency_pins':dependencies,
            'attempts':attempts,'failed_attempts':failed,'corrupt_attempts':corrupt,
            'complete_query_population':all(not item['population']['short_windows']
                and not item['population']['censored_windows'] for item in stats.values()),
            'source_quality_certified':False}
    finally:
        try:A.check_pins(expected_dependencies)
        finally:con.execute('DROP TABLE t17_event_ledger')


@contextlib.contextmanager
def projection(name,con,relations,hits,dependency_pins):
    """Transaction-local replay scope, not source admission or publication.

    The outer source reader must admit the complete source/corpus/clock/market
    contract before entering. This scope authenticates its actual complete pins
    before/finally, retains native diagnostic semantics and cleans only its own
    initially absent query tables. It never caches a caller's mutable relation.
    """
    A.guard(con);pins=dict(dependency_pins)
    if any(pins.get(path)!=identity for path,identity in code_pins().items()):
        raise ValueError('complete original event projection code pins required')
    expected={'filings','profile','lines','links'} if name=='capital' else {'filings','items','links','cover'} if name=='governance' else {'rows','fundamentals','links'} if name=='buyback' else None
    if expected is None or set(relations)!=expected:
        raise ValueError('fixed complete event relations required')
    sealed=A.digest({'relations':relations,'hits':hits})
    if name=='buyback' and hits:raise ValueError('buyback native rows have no query-relation override')
    names=('f','cs','cfirst','spac','b4','ipo','ev','tk','tk2','ev2') if name=='capital' else ('ceo','cfo','k','aud','k2','k3') if name=='governance' else ('b','fx','b2','b3')
    reserved=(*names,'_lk','_mem','_ev','_hit')
    existing={row[0] for row in con.execute('SHOW TABLES').fetchall()}
    if existing.intersection(reserved):raise ValueError('event replay table namespace already occupied')
    A.check_pins(pins)
    try:
        sql=capital(con,hits=hits,**relations) if name=='capital' else governance(con,hits=hits,**relations) if name=='governance' else buyback(con,**relations)
        yield sql
    finally:
        try:
            A.check_pins(pins);A.guard(con)
            if dependency_pins!=pins or A.digest({'relations':relations,'hits':hits})!=sealed:
                raise ValueError('event replay consumption binding changed')
        finally:
            for table in reserved:con.execute('DROP TABLE IF EXISTS '+table)


def identifier(value):
    if not re.fullmatch('[A-Za-z_][A-Za-z0-9_]*',value):
        raise ValueError('fixed SQL relation identifier required')
    return value


def buyback(con,rows,fundamentals,links):
    """Exact events_buyback.validate_product/phase_publish join projection.

    Inputs must be established by full original Task13 body/catalog replay,
    independently admitted financial generation and native identity replay.
    This phase cannot establish the missing complete 180-day announcement
    history: native unknown flags remain unknown; future XBRL is diagnostic.
    """
    rows,fundamentals,links=map(identifier,(rows,fundamentals,links))
    con.execute(f'''CREATE TABLE b AS SELECT r.*,
        CASE WHEN r.event_type='execution' THEN false ELSE CAST(NULL AS BOOLEAN) END first_announcement,
        CASE WHEN r.event_type='execution' THEN 'not_applicable_execution'
            ELSE 'unknown_incomplete_global_history' END first_announcement_basis
        FROM {rows} r''')
    con.execute(f'''CREATE TABLE fx AS SELECT cik,accession xbrl_accession,clock_utc,
        buyback_authorized,buyback_remaining FROM {fundamentals}
        WHERE buyback_authorized IS NOT NULL OR buyback_remaining IS NOT NULL''')
    con.execute('''CREATE TABLE b2 AS SELECT b.*,
        pb.buyback_authorized xbrl_authorized_before,pb.buyback_remaining xbrl_remaining_before,
        pa.buyback_authorized xbrl_authorized_after,pa.buyback_remaining xbrl_remaining_after,
        pa.xbrl_accession xbrl_after_accession,pa.clock_utc xbrl_after_available_at
        FROM b ASOF LEFT JOIN fx pb ON pb.cik=b.cik AND b.available_at>pb.clock_utc
        ASOF LEFT JOIN fx pa ON pa.cik=b.cik AND pa.clock_utc>b.available_at''')
    attach_security(con,'b2','b3',links,date_col='filing_date')
    return '''SELECT cik,security_id,link_tier,link_basis,is_member_issuer,accession,form,items,
        source_doc,doc_source,filing_date,event_date,available_at,acceptance_clock,vintage_risk,
        event_type,amount_usd,amount_shares,total_after_usd,first_announcement,first_announcement_basis,
        xbrl_authorized_before,xbrl_remaining_before,xbrl_authorized_after,xbrl_remaining_after,
        xbrl_after_accession,xbrl_after_available_at,evidence,extractor_version,source_sha256,
        execution_period,execution_shares,execution_average_price,public_program_shares,
        remaining_authorization_usd FROM b3 ORDER BY cik,available_at,accession'''


def event_catalog(con,filings,landed,fetched,hits):
    """Original ES.build_catalog native union and ambiguity resolution.

    ``landed`` is the separate original Task9 units_done/body role; ``fetched``
    is the complete original FetchLedgerStore document role. The outer reader
    must independently replay both and the entire actual native query inventory.
    Neither role is replaced by annual FT evidence or optional caller flags.
    Relations have the exact native _landed/_fetched schemas, including NULLs.
    """
    filings,landed,fetched=map(identifier,(filings,landed,fetched))
    if not set(hits).issubset(ES.QUERIES):raise ValueError('unknown native event catalog query')
    if {r[0] for r in con.execute('SHOW TABLES').fetchall()}.intersection(('t17_catalog_hits','t17_catalog_acc')):
        raise ValueError('original event catalog namespace occupied')
    if hits:
        union=' UNION ALL '.join('SELECT * FROM '+identifier(hits[key]) for key in sorted(hits))
        con.execute(f'''CREATE TEMP TABLE t17_catalog_hits AS
            SELECT replace(adsh,'-','') acc18,file,any_value(file_type) file_type,
                any_value(file_description) file_description,list(DISTINCT qid) qids,any_value(ciks) ciks
            FROM({union}) GROUP BY 1,2''')
    else:
        con.execute('''CREATE TEMP TABLE t17_catalog_hits(acc18 VARCHAR,file VARCHAR,
            file_type VARCHAR,file_description VARCHAR,qids VARCHAR[],ciks BIGINT[])''')
    con.execute(f'''CREATE TEMP TABLE t17_catalog_acc AS WITH w AS(
        SELECT accession,replace(accession,'-','') acc18 FROM {landed}
        UNION SELECT NULL,acc18 FROM {fetched})
        SELECT f.cik,f.accession,replace(f.accession,'-','') acc18,f.form,f.items,f.filing_date,
            f.report_date event_date,f.available_at,f.acceptance_clock,f.vintage_risk,f.is_amendment
        FROM {filings} f WHERE replace(f.accession,'-','') IN(SELECT acc18 FROM w)''')
    return f'''WITH d AS(
        SELECT l.accession acc_hint,replace(l.accession,'-','') acc18,NULL::BIGINT url_cik,
            'landed_v2' doc_source,l.file source_doc,l.sha doc_sha256,NULL url FROM {landed} l
        UNION ALL SELECT NULL,x.acc18,x.url_cik,'sec_events',x.file,x.sha,x.url FROM {fetched} x),
        m AS(SELECT d.*,a.cik,a.accession,a.form,a.items,a.filing_date,a.event_date,a.available_at,
            a.acceptance_clock,a.vintage_risk,a.is_amendment,
            row_number() OVER(PARTITION BY d.doc_source,d.acc18,d.source_doc
                ORDER BY(a.cik=d.url_cik) DESC,a.cik) rn
            FROM d JOIN t17_catalog_acc a ON a.acc18=d.acc18)
        SELECT m.cik,m.accession,m.form,m.items,m.filing_date,m.event_date,m.available_at,
            m.acceptance_clock,m.vintage_risk,m.is_amendment,m.doc_source,m.source_doc,m.doc_sha256,
            m.url,h.file_type,h.file_description,coalesce(h.qids,[]::VARCHAR[]) qids
        FROM m LEFT JOIN t17_catalog_hits h ON h.acc18=m.acc18 AND h.file=m.source_doc
        WHERE m.rn=1 ORDER BY m.cik,m.available_at,m.accession,m.source_doc'''


@contextlib.contextmanager
def catalog_scope(con,relations,hits,dependency_pins):
    """Authenticate the complete catalog source consumption, never each batch."""
    A.guard(con)
    if set(relations)!={'filings','landed','fetched'}:
        raise ValueError('both fixed original event catalog body roles required')
    pins={path:dict(pin) for path,pin in dependency_pins.items()}
    A.check_pins(pins,code_pins())
    sealed=A.digest({'relations':relations,'hits':hits})
    owned=('t17_catalog_hits','t17_catalog_acc')
    if {r[0] for r in con.execute('SHOW TABLES').fetchall()}.intersection(owned):
        raise ValueError('original catalog replay namespace occupied')
    try:yield event_catalog(con,hits=hits,**relations)
    finally:
        try:
            A.check_pins(pins);A.guard(con)
            if dependency_pins!=pins or A.digest({'relations':relations,'hits':hits})!=sealed:
                raise ValueError('original event catalog source binding changed')
        finally:
            for table in owned:con.execute('DROP TABLE IF EXISTS '+table)


@contextlib.contextmanager
def catalog_sources_scope(con,sec_admission,event_root,landed_root,query_products,
                          query_dependencies,event_dependencies,landed_dependencies,scratch):
    """Bind both complete original body roles and native full query population.

    Source-phase SEC admission is independently replayed before construction;
    its complete Accepted overlay remains separate evidence. Native catalog
    clocks and vintage risks are preserved. No Task9 publication/clock proof is
    inferred from units-done transport, and no annual-text corpus substitutes.
    """
    from . import private_tier1_sec_sources as S
    A.guard(con);scratch=A.absolute(scratch)
    seal=A.digest({'query_products':{k:str(v) for k,v in query_products.items()},
        'query_dependencies':query_dependencies,'event_dependencies':event_dependencies,
        'landed_dependencies':landed_dependencies,'sec_spec':sec_admission['spec']})
    if scratch.exists():raise ValueError('preserve existing catalog replay attempt')
    owned=('t17_catalog_source_filings','t17_catalog_source_landed','t17_catalog_source_fetched',
           *('t17_catalog_query_'+str(i) for i in range(len(ES.QUERIES))))
    if {row[0] for row in con.execute('SHOW TABLES').fetchall()}.intersection(owned):
        raise ValueError('catalog original source namespace occupied')
    # No fabricated prior admission is accepted: re-enter the fixed full reader.
    sec=S.admit(sec_admission['spec'],con)
    scratch.mkdir(parents=True)
    try:
        with S.consuming(sec,con),event_body_scope(con,event_root,event_dependencies) as fetched,\
                landed_body_scope(con,landed_root,landed_dependencies) as landed:
            corpus=replay_query_corpus('catalog',con,event_root,query_products,scratch,query_dependencies)
            con.execute('CREATE VIEW t17_catalog_source_filings AS SELECT * FROM '+A.sql([sec['products']['filings.parquet']]))
            con.execute('CREATE VIEW t17_catalog_source_landed AS SELECT accession,file,sha FROM t17_event_landed_registry')
            con.execute('CREATE TABLE t17_catalog_source_fetched(url VARCHAR,sha VARCHAR,url_cik BIGINT,acc18 VARCHAR,file VARCHAR)')
            batch=[]
            for row in fetched.rows():
                parts=row['url'][len(ES.ARCHIVES)+1:].split('/')
                # Exact native build_catalog URL interpretation, no guessed SID/CIK.
                batch.append((row['url'],row['sha256'],int(parts[0]),parts[1],parts[2]))
                if len(batch)==32:
                    con.executemany('INSERT INTO t17_catalog_source_fetched VALUES(?,?,?,?,?)',batch);batch.clear()
            if batch:con.executemany('INSERT INTO t17_catalog_source_fetched VALUES(?,?,?,?,?)',batch)
            hits={}
            for index,qid in enumerate(sorted(ES.QUERIES)):
                table='t17_catalog_query_'+str(index)
                con.execute('CREATE VIEW '+table+' AS SELECT * FROM '+A.sql([query_products[qid]]));hits[qid]=table
            pins={**sec['dependency_pins'],**query_dependencies,**event_dependencies,**landed_dependencies}
            relations={'filings':'t17_catalog_source_filings','landed':'t17_catalog_source_landed',
                       'fetched':'t17_catalog_source_fetched'}
            with catalog_scope(con,relations,hits,pins) as sql:
                yield {'sql':sql,'fetched':fetched,'landed':landed,'corpus':corpus,
                    'original_accepted':sec['products']['original_accepted.parquet'],
                    'dependency_pins':pins,'source_quality_certified':False,
                    'clock_rule':'native_catalog_diagnostics_with_separate_original_Accepted_evidence'}
    finally:
        try:
            if A.digest({'query_products':{k:str(v) for k,v in query_products.items()},
                'query_dependencies':query_dependencies,'event_dependencies':event_dependencies,
                'landed_dependencies':landed_dependencies,'sec_spec':sec_admission['spec']})!=seal:
                raise ValueError('catalog complete original source binding changed')
            A.guard(con)
        finally:
            for table in owned:
                con.execute('DROP VIEW IF EXISTS '+table) if table!='t17_catalog_source_fetched' else con.execute('DROP TABLE IF EXISTS '+table)


def attach_security(con,table,out,links,date_col='filing_date'):
    """Exact events_common.attach_security35-67 with explicit link relation."""
    table,out,links,date_col=map(identifier,(table,out,links,date_col))
    con.execute(f'''CREATE OR REPLACE TEMP TABLE _lk AS
        SELECT security_id,cik,valid_from,valid_to,link_tier,
               coalesce(is_issuer_primary,false) AS prim,
               coalesce(sessions,0) AS sessions,coalesce(ever_member,false) AS ever_member
        FROM {links} WHERE cik IS NOT NULL''')
    con.execute('CREATE OR REPLACE TEMP TABLE _mem AS SELECT DISTINCT cik FROM _lk WHERE ever_member')
    con.execute(f'CREATE OR REPLACE TEMP TABLE _ev AS SELECT row_number() OVER () AS _rid,* FROM {table}')
    con.execute(f'''CREATE OR REPLACE TEMP TABLE _hit AS
        SELECT _rid,security_id,link_tier,link_basis FROM (
            SELECT e._rid,l.security_id,l.link_tier,
                CASE WHEN CAST(e.{date_col} AS DATE) BETWEEN l.valid_from AND l.valid_to
                     THEN 'on_date' ELSE 'nearest_30d' END AS link_basis,
                row_number() OVER (PARTITION BY e._rid ORDER BY
                    (CAST(e.{date_col} AS DATE) BETWEEN l.valid_from AND l.valid_to) DESC,
                    least(abs(date_diff('day',CAST(e.{date_col} AS DATE),l.valid_from)),
                          abs(date_diff('day',CAST(e.{date_col} AS DATE),l.valid_to))),
                    l.prim DESC,{EC.TIER_RANK_SQL.replace('link_tier','l.link_tier')},
                    l.sessions DESC,l.security_id) AS rn
            FROM _ev e JOIN _lk l ON l.cik=e.cik
              AND CAST(e.{date_col} AS DATE) BETWEEN l.valid_from - INTERVAL {EC.LINK_TOLERANCE_DAYS} DAY
                                                AND l.valid_to + INTERVAL {EC.LINK_TOLERANCE_DAYS} DAY
        ) WHERE rn=1''')
    con.execute(f'''CREATE OR REPLACE TABLE {out} AS
        SELECT e.* EXCLUDE (_rid),h.security_id,coalesce(h.link_tier,'unlinked') AS link_tier,
            coalesce(h.link_basis,'unlinked') AS link_basis,(m.cik IS NOT NULL) AS is_member_issuer
        FROM _ev e LEFT JOIN _hit h USING (_rid) LEFT JOIN _mem m ON m.cik=e.cik''')
    for name in ('_lk','_mem','_ev','_hit'):con.execute('DROP TABLE '+name)


def departure_sql(hits,qids):
    """Native governance._hits_sql filtering, with complete explicit queries."""
    selected=[identifier(hits[key]) for key in qids]
    relation='('+' UNION ALL '.join('SELECT adsh,file_type FROM '+name for name in selected)+')'
    return f'''SELECT DISTINCT adsh FROM {relation}
        WHERE upper(coalesce(file_type,'')) IN ('8-K','8-K/A')
           OR upper(coalesce(file_type,'')) LIKE 'EX-99%' '''


def governance(con,filings,items,links,hits,cover=None):
    """Exact frozen governance65-119 population; returns ordered native SQL."""
    filings,items,links=map(identifier,(filings,items,links))
    required={query for group in G.DEPART_QIDS.values() for query in group}|{'going_concern'}
    if set(hits)!=required:raise ValueError('complete original governance query corpus required')
    con.execute('CREATE TABLE ceo AS '+departure_sql(hits,G.DEPART_QIDS['ceo']))
    con.execute('CREATE TABLE cfo AS '+departure_sql(hits,G.DEPART_QIDS['cfo']))
    case=' '.join(f"WHEN '{key}' THEN '{value}'" for key,value in G.ITEM_EVENTS.items())
    item_list=','.join("'"+key+"'" for key in G.ITEM_EVENTS)
    con.execute(f'''CREATE TABLE k AS
        SELECT i.cik,i.accession,i.form,i.is_amendment,i.item,CASE i.item {case} END AS event_type,
            i.filing_date,i.report_date AS event_date,i.available_at,i.acceptance_clock,i.vintage_risk,
            CASE WHEN i.item='5.02' THEN (i.accession IN (SELECT adsh FROM ceo)) END AS ceo_departure,
            CASE WHEN i.item='5.02' THEN (i.accession IN (SELECT adsh FROM cfo)) END AS cfo_departure
        FROM {items} i WHERE i.item IN ({item_list}) AND i.filing_date>=DATE '{G.START}' ''')
    gc=identifier(hits['going_concern'])
    con.execute(f'''INSERT INTO k
        SELECT f.cik,f.accession,f.form,f.is_amendment,NULL,'going_concern',f.filing_date,
            f.report_date,f.available_at,f.acceptance_clock,f.vintage_risk,NULL,NULL
        FROM {filings} f WHERE f.form IN ('10-K','10-KT') AND f.filing_date>=DATE '{G.START}'
            AND f.accession IN (SELECT DISTINCT adsh FROM {gc}
                WHERE upper(coalesce(file_type,'')) IN ('10-K','10-KT'))''')
    if cover is not None:
        cover=identifier(cover)
        con.execute(f'''CREATE TABLE aud AS
            SELECT DISTINCT entity_cik AS cik,available_at AS aud_at,auditor_name FROM {cover}
            WHERE form IN ('10-K','10-K/A','20-F','40-F')
                AND auditor_name IS NOT NULL AND entity_cik IS NOT NULL''')
        con.execute('''CREATE TABLE k2 AS
            SELECT k.*,CASE WHEN k.item='4.01' THEN b.auditor_name END AS auditor_name_before,
                CASE WHEN k.item='4.01' THEN a.auditor_name END AS auditor_name_after
            FROM k ASOF LEFT JOIN aud b ON b.cik=k.cik AND k.available_at>b.aud_at
                ASOF LEFT JOIN aud a ON a.cik=k.cik AND a.aud_at>k.available_at''')
    else:
        con.execute('''CREATE TABLE k2 AS SELECT *,CAST(NULL AS VARCHAR) AS auditor_name_before,
            CAST(NULL AS VARCHAR) AS auditor_name_after FROM k''')
    attach_security(con,'k2','k3',links)
    return '''SELECT cik,security_id,link_tier,link_basis,is_member_issuer,accession,form,is_amendment,item,
        event_type,filing_date,event_date,available_at,acceptance_clock,vintage_risk,ceo_departure,
        cfo_departure,CASE WHEN ceo_departure OR cfo_departure THEN 'fts_phrase' END AS departure_basis,
        auditor_name_before,auditor_name_after FROM k3 ORDER BY cik,available_at,accession,event_type'''


def capital(con,filings,profile,lines,links,hits):
    """Exact frozen capital56-159 population on independently admitted relations."""
    filings,profile,lines,links=map(identifier,(filings,profile,lines,links))
    if set(hits)!={*K.CONVERT_QIDS,'spinoff'}:
        raise ValueError('complete original capital query corpus required')
    forms=','.join("'"+form+"'" for form in (*K.REG_FORMS,'424B4','424B5','424B7','10-12B'))
    con.execute(f'''CREATE TABLE f AS
        SELECT cik,accession,form,filing_date,report_date,available_at,acceptance_clock,vintage_risk
        FROM {filings} WHERE form IN ({forms}) AND filing_date>=DATE '{K.LOOKBACK}' ''')
    con.execute(f'''CREATE TABLE cs AS
        SELECT DISTINCT l.cik,s.security_id,s.first_session,s.left_censored
        FROM {links} l JOIN {lines} s USING (security_id) WHERE l.cik IS NOT NULL''')
    con.execute('CREATE TABLE cfirst AS SELECT cik,min(first_session) AS cik_first_session FROM cs GROUP BY 1')
    con.execute(f"CREATE TABLE spac AS SELECT DISTINCT cik FROM {profile} WHERE sic='6770'")
    con.execute("CREATE TABLE b4 AS SELECT *,row_number() OVER (PARTITION BY cik ORDER BY available_at,accession) AS k FROM f WHERE form='424B4'")
    reg=','.join("'"+form+"'" for form in K.REG_FORMS)
    con.execute(f'''CREATE TABLE ipo AS
        SELECT b.*,EXISTS(SELECT 1 FROM f r WHERE r.cik=b.cik AND r.form IN ({reg})
            AND r.filing_date BETWEEN b.filing_date-INTERVAL 540 DAY AND b.filing_date) AS has_reg,
            (SELECT min(first_session) FROM cs WHERE cs.cik=b.cik AND NOT coalesce(cs.left_censored,false)
                AND cs.first_session BETWEEN b.filing_date-INTERVAL 5 DAY AND b.filing_date+INTERVAL 45 DAY) AS first_session,
            cf.cik_first_session FROM b4 b LEFT JOIN cfirst cf USING(cik) WHERE b.k=1''')
    con.execute('''CREATE TABLE ev(cik BIGINT,accession VARCHAR,form VARCHAR,event_type VARCHAR,filing_date DATE,
        event_date DATE,available_at TIMESTAMP,acceptance_clock VARCHAR,vintage_risk VARCHAR,
        last_accession VARCHAR,last_available_at TIMESTAMP,n_filings INTEGER,first_session DATE,
        listed BOOLEAN,is_spac BOOLEAN,evidence VARCHAR)''')
    con.execute(f'''INSERT INTO ev
        SELECT cik,accession,form,'ipo',filing_date,filing_date,available_at,acceptance_clock,vintage_risk,
            accession,available_at,1,first_session,first_session IS NOT NULL,cik IN(SELECT cik FROM spac),
            'first 424B4 after S-1/F-1/S-11 within 540d; no line trading before'
        FROM ipo WHERE has_reg AND filing_date>=DATE '{K.START}'
            AND(cik_first_session IS NULL OR cik_first_session>=filing_date-INTERVAL 5 DAY)''')
    con.execute(f'''INSERT INTO ev
        SELECT b.cik,b.accession,b.form,'follow_on',b.filing_date,b.filing_date,b.available_at,
            b.acceptance_clock,b.vintage_risk,b.accession,b.available_at,1,NULL,NULL,
            b.cik IN(SELECT cik FROM spac),'424B4 not classified as the IPO'
        FROM b4 b WHERE b.filing_date>=DATE '{K.START}'
            AND b.accession NOT IN(SELECT accession FROM ev WHERE event_type='ipo')''')
    case=' '.join(f"WHEN '{key}' THEN '{value}'" for key,value in K.TAKEDOWN.items())
    takedown=','.join("'"+key+"'" for key in K.TAKEDOWN)
    cv=departure_sql(hits,K.CONVERT_QIDS)
    con.execute(f'''CREATE TABLE tk AS
        SELECT cik,accession,form,CASE form {case} END AS event_type,filing_date,
            filing_date AS event_date,available_at,acceptance_clock,vintage_risk FROM f
        WHERE form IN({takedown}) AND filing_date>=DATE '{K.START}'
        UNION ALL SELECT g.cik,g.accession,g.form,'convert_pricing',g.filing_date,g.report_date,
            g.available_at,g.acceptance_clock,g.vintage_risk FROM {filings} g
        WHERE g.form IN('8-K','8-K/A') AND g.filing_date>=DATE '{K.START}'
            AND g.accession IN({cv})''')
    con.execute(f'''CREATE TABLE tk2 AS
        SELECT *,sum(new) OVER(PARTITION BY cik,event_type ORDER BY available_at,accession) AS grp FROM(
            SELECT *,CASE WHEN lag(filing_date) OVER(PARTITION BY cik,event_type ORDER BY available_at,accession)
                >=filing_date-INTERVAL {K.CLUSTER_DAYS} DAY THEN 0 ELSE 1 END AS new FROM tk)''')
    con.execute(f'''INSERT INTO ev
        SELECT cik,arg_min(accession,(available_at,accession)),arg_min(form,(available_at,accession)),
            event_type,min(filing_date),arg_min(event_date,(available_at,accession)),min(available_at),
            arg_min(acceptance_clock,(available_at,accession)),arg_min(vintage_risk,(available_at,accession)),
            arg_max(accession,(available_at,accession)),max(available_at),count(*),NULL,NULL,
            cik IN(SELECT cik FROM spac),'filings of one issuer and type within {K.CLUSTER_DAYS} days form one event'
        FROM tk2 GROUP BY cik,event_type,grp''')
    sp=identifier(hits['spinoff'])
    con.execute(f'''INSERT INTO ev
        SELECT f.cik,f.accession,f.form,'spin_off',f.filing_date,f.filing_date,f.available_at,
            f.acceptance_clock,f.vintage_risk,f.accession,f.available_at,1,
            (SELECT min(first_session) FROM cs WHERE cs.cik=f.cik
                AND cs.first_session BETWEEN f.filing_date AND f.filing_date+INTERVAL 400 DAY),
            NULL,false,'first Form 10-12B whose text names a distribution; no line trading before'
        FROM(SELECT *,row_number() OVER(PARTITION BY cik ORDER BY available_at,accession) AS k
            FROM f WHERE form='10-12B') f LEFT JOIN cfirst cf USING(cik)
        WHERE f.k=1 AND f.filing_date>=DATE '{K.START}'
            AND(cf.cik_first_session IS NULL OR cf.cik_first_session>=f.filing_date)
            AND f.accession IN(SELECT DISTINCT adsh FROM {sp})''')
    con.execute("UPDATE ev SET listed=first_session IS NOT NULL WHERE event_type='spin_off'")
    attach_security(con,'ev','ev2',links)
    return '''SELECT cik,security_id,link_tier,link_basis,is_member_issuer,accession,form,event_type,
        filing_date,event_date,available_at,acceptance_clock,vintage_risk,last_accession,last_available_at,
        n_filings,first_session,listed,is_spac,evidence
        FROM ev2 ORDER BY cik,available_at,accession,event_type'''
