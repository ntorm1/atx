"""Explicit-connection original cover replay for downstream event admission.

The original notes source admission is mandatory. Financial standardized data
cannot substitute for the original DEI/context source or auditor observations.
This phase preserves native fallback/repost/context/unknown rows and gates.
"""
import contextlib
import json
import re
from pathlib import Path
from . import cover_page as CP, notes_fetch as N
from . import private_tier1_notes_sources as NS, private_tier1_admission as A


def code_pins():
    return A.code(('private_tier1_cover_replay','cover_page','notes_fetch',
        'shortflow_common','private_tier1_notes_sources','private_tier1_admission','common'))


def _reposts(con,ledger):
    """Bounded exact last-receipt selection for native string dataset keys."""
    con.execute('CREATE TABLE t17_cover_receipts(seq BIGINT,source VARCHAR,record JSON)')
    sequence=0;batch=[]
    with ledger.open('rb') as stream:
        while line:=stream.readline(1024*1024+1):
            if len(line)>1024*1024:raise ValueError('bounded original cover receipt record exceeded')
            if not line.strip():continue
            sequence+=1
            try:record=json.loads(line.decode('utf-8'))
            except json.JSONDecodeError:continue  # same native torn-record rule
            if 'key' not in record:continue
            if not isinstance(record['key'],str):
                raise ValueError('original notes dataset receipt key requires native string schema')
            batch.append((sequence,record['key'],json.dumps(record)))
            if len(batch)==32:
                con.executemany('INSERT INTO t17_cover_receipts VALUES(?,?,?)',batch);batch.clear()
    if batch:con.executemany('INSERT INTO t17_cover_receipts VALUES(?,?,?)',batch)
    con.execute('''CREATE TABLE rep AS SELECT source,
        CAST(record->'repost' AS INTEGER) AS repost FROM(
            SELECT *,row_number() OVER(PARTITION BY source ORDER BY seq DESC) rn
            FROM t17_cover_receipts) WHERE rn=1 AND (record->>'status')='parsed' ''')


def _setup(con,parts,keys,ledger):
    """Exact original cover_page.build166-177 explicit-path source projection."""
    con.execute('CREATE TABLE src(source VARCHAR,ord INTEGER)')
    con.executemany('INSERT INTO src VALUES(?,?)',[(key,index) for index,key in enumerate(sorted(keys,key=N.order_key))])
    _reposts(con,ledger)
    source=(parts/'source=*'/'sub.parquet').as_posix().replace("'","''")
    con.execute(f'''CREATE TABLE sub AS
        SELECT s.adsh,s.cik,s.form,s.period,s.fy,s.fp,s.filed,s.accepted_utc,s.pubfloatusd,s.floatdate,
            s.instance,s.source,coalesce(r.repost,0)>0 AS vintage_risk,
            count(*) OVER(PARTITION BY s.adsh) AS n_data_sets,
            coalesce(s.accepted_utc,CAST(s.filed AS TIMESTAMP) AT TIME ZONE 'UTC'+INTERVAL {CP.FC1_HOURS} HOUR) AS available_at,
            CASE WHEN s.accepted_utc IS NULL THEN 'filed_plus_46h' ELSE 'accepted_utc' END AS clock_basis
        FROM read_parquet('{source}',hive_partitioning=false) s
        JOIN src USING(source) LEFT JOIN rep r USING(source)
        QUALIFY row_number() OVER(PARTITION BY s.adsh ORDER BY src.ord)=1''')
    return [row[0] for row in con.execute('SELECT DISTINCT year(filed) FROM sub WHERE filed IS NOT NULL ORDER BY 1').fetchall()]


def coverage(con,published,links):
    """Original cover_page.coverage353-401 with an explicit admitted link relation.

    The full native identity source reader must establish ``links`` before this
    internal projection. No caller relation or native legacy backfill becomes
    independently qualified merely because coverage can be calculated.
    """
    if not isinstance(links,str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*',links):
        raise ValueError('fixed admitted cover identity relation required')
    per=','.join("'"+value+"'" for value in CP.PERIODIC)
    source=A.sql([published])
    rows=con.execute(f'''WITH c AS (
        SELECT *,year(filed) y,(symbol_norm IS NOT NULL OR exchange_norm IS NOT NULL) hit
        FROM {source} WHERE coreg IS NULL),
        den AS(SELECT DISTINCT y,cik FROM c WHERE form IN({per})),
        own AS(SELECT DISTINCT y,cik FROM c WHERE form IN({per}) AND hit),
        anyf AS(SELECT DISTINCT y,cik FROM c WHERE hit),
        nts AS(SELECT DISTINCT y,cik FROM c WHERE no_trading_symbol_flag),
        ins AS(SELECT DISTINCT y,cik FROM c WHERE hit OR instance_prefix IS NOT NULL),
        lk AS(SELECT DISTINCT y.y,l.cik,bool_or(l.ever_member) member
            FROM {links} l,(SELECT DISTINCT year(filed) y FROM {source}) y
            WHERE l.valid_from<=make_date(y.y,12,31) AND l.valid_to>=make_date(y.y,1,1)
            GROUP BY 1,2)
        SELECT d.y,count(*),count(o.cik),count(a.cik),
            count(*) FILTER(WHERE a.cik IS NULL AND n.cik IS NOT NULL),
            count(lk.cik),count(lk.cik) FILTER(WHERE a.cik IS NOT NULL),
            count(lk.cik) FILTER(WHERE lk.member),
            count(lk.cik) FILTER(WHERE lk.member AND a.cik IS NOT NULL),
            count(i.cik),count(lk.cik) FILTER(WHERE i.cik IS NOT NULL),
            count(lk.cik) FILTER(WHERE lk.member AND i.cik IS NOT NULL)
        FROM den d LEFT JOIN own o USING(y,cik) LEFT JOIN anyf a USING(y,cik)
        LEFT JOIN nts n USING(y,cik) LEFT JOIN lk USING(y,cik) LEFT JOIN ins i USING(y,cik)
        WHERE d.y>=2019 GROUP BY 1 ORDER BY 1''').fetchall()
    result={}
    for y,n,o,a,nts,nl,al,nm,am,ia,il,im in rows:
        result[str(y)]={'periodic_filer_ciks':n,'own_periodic_cover_hit':o,'any_cover_hit':a,
            'share_own_periodic':round(o/n,4),'share_any_cover':round(a/n,4),
            'no_trading_symbol_flag_only':nts,
            'share_any_cover_excl_no_symbol_flag':round(a/(n-nts),4) if n>nts else None,
            'linked_periodic_ciks':nl,'share_any_cover_linked':round(al/nl,4) if nl else None,
            'member_periodic_ciks':nm,'share_any_cover_member':round(am/nm,4) if nm else None,
            'share_with_instance_prefix':round(ia/n,4),
            'share_with_instance_prefix_linked':round(il/nl,4) if nl else None,
            'share_with_instance_prefix_member':round(im/nm,4) if nm else None}
    return result


@contextlib.contextmanager
def cover_scope(con,notes_admission,published,scratch,dependency_pins):
    """Replay every original cover year, yield the exact immutable native leaf.

    ``notes_admission`` must come from the complete fixed native notes reader.
    Its consumption guard rechecks actual originals/products/plan/runtime.
    Independent full expected pins must include the published cover and this
    adapter code. No source quality or future auditor PIT assertion is minted.
    """
    A.guard(con);published=A.absolute(published);scratch=A.absolute(scratch)
    pins={path:dict(pin) for path,pin in dependency_pins.items()}
    required={**notes_admission['dependency_pins'],**code_pins(),str(published):A.pin(published)}
    A.check_pins(pins,required)
    reserved=('src','rep','sub','suby','dim','t','n','sec','sh','fil','ecik','shtot','pf','k','cp','nsec','t17_cover_receipts')
    if {row[0] for row in con.execute('SHOW TABLES').fetchall()}.intersection(reserved):
        raise ValueError('original cover replay namespace occupied')
    if scratch.exists():raise ValueError('preserve existing original cover replay attempt')
    scratch.mkdir(parents=True)
    try:
        with NS.consuming(notes_admission,con):
            plan=NS._json(notes_admission['spec']['native']['plan_path'])
            parts=A.absolute(plan['source_root'])/'parts'
            keys=[p.name.split('=',1)[1] for p in parts.glob('source=*') if p.is_dir()]
            if set(keys)!=set(plan['archives']):
                raise ValueError('complete native cover notes source axis differs')
            years=_setup(con,parts,keys,A.absolute(plan['source_receipts']))
            leaves=[];population={}
            for year in years:
                leaf=scratch/(str(year)+'.parquet')
                population[str(year)]=CP._build_year(con,parts,keys,year,leaf);leaves.append(leaf)
            replay=scratch/'cover_page.parquet'
            N.stitch_parquet(leaves,replay,32768)
            A.compare(con,published,replay)
            validation={'rows_per_year':population,'checks':CP.checks(con,replay.as_posix()),
                'clock_rule':CP.CLOCK_RULE,'source_quality_certified':False,
                'native_replay':'complete_typed_population','future_auditor_observations':'diagnostic_only'}
            yield {'products':{'cover_page.parquet':published},'validation':validation,
                'dependency_pins':required,'source_quality_certified':False}
    finally:
        try:
            A.check_pins(pins,required);A.guard(con)
            if dependency_pins!=pins:raise ValueError('original cover dependency binding changed')
        finally:
            for table in reserved:con.execute('DROP TABLE IF EXISTS '+table)
