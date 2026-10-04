"""R26 private original-submissions supplement; never admits ownership/public clocks.

The frozen inventory API retains its 4 MiB cap. Only the independently bound
two original exception mains use this separate 5 MiB source working unit.
"""
from __future__ import annotations

import datetime as dt
import copy
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path

from atx_db import _submissions_archive as A
from . import common as C, thirteenf_quality as Q

SCHEMA='atx.ownership-exception-source/v1'
TABLE='t17_ownership_inventory'
EXCEPTIONS={19617:('CIK0000019617.json',4636526),1665650:('CIK0001665650.json',4452571)}
FIELDS={'accession':'VARCHAR','cik':'BIGINT','form':'VARCHAR','filing_date':'VARCHAR',
        'filing_quarter':'VARCHAR','report_date':'VARCHAR','report_quarter':'VARCHAR',
        'available_at':'VARCHAR','clock_basis':'VARCHAR','source_url':'VARCHAR',
        'inventory_url':'VARCHAR','inventory_sha256':'VARCHAR','inventory_member':'VARCHAR'}


def _bounded_connection(con):
    threads, timezone, limit, current = con.execute(
        "SELECT current_setting('threads'),current_setting('TimeZone'),"
        "current_setting('memory_limit'),current_database()"
    ).fetchone()
    quantity, unit = limit.split()
    scales = {'B':1,'KB':1000,'MB':1000000,'GB':1000000000,
              'KiB':1024,'MiB':1048576,'GiB':1073741824}
    if threads != 1 or timezone != 'UTC' or unit not in scales or float(quantity)*scales[unit] > 128000000:
        raise ValueError('original inventory replay requires128MB/one thread/UTC')
    if not any(row[1] == current and row[2] for row in con.execute('PRAGMA database_list').fetchall()):
        raise ValueError('original inventory replay requires file-backed primary database')


def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode()).hexdigest()


def identity(path):
    path=Path(path);before=path.stat()
    result={'bytes':before.st_size,'sha256':C.sha256_file(path)}
    after=path.stat()
    if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):
        raise ValueError('source changed during hash')
    return result


def member(root,name):
    root=Path(root).resolve();path=(root/name).resolve()
    if Path(name).is_absolute() or '..' in Path(name).parts or not path.is_relative_to(root):
        raise ValueError('invalid original-source member')
    return path


def _json(path,maximum=1_000_000):
    if Path(path).stat().st_size>maximum:raise ValueError('bounded source metadata exceeded')
    with Path(path).open('rb') as handle:raw=handle.read(maximum+1)
    if len(raw)>maximum:raise ValueError('bounded source metadata exceeded')
    return json.loads(raw)


def _check(root,plan,expected):
    if not os.environ.get('ATX_GUARD_JOB'):raise ValueError('external guard required')
    if digest(plan)!=expected or plan.get('schema')!=SCHEMA:raise ValueError('independent source plan mismatch')
    for name,pin in plan['files'].items():
        if identity(member(root,name))!=pin:raise ValueError('changed pinned source: '+name)
    required={str(Path(m.__file__).resolve()) for m in (A,C,Q)}|{str(Path(__file__).resolve())}
    if not required<=set(plan['code']):raise ValueError('complete original source/kernel/helper code required')
    for name,pin in plan['code'].items():
        if not Path(name).is_absolute() or identity(name)!=pin:raise ValueError('changed pinned code')
    for field in ('archive','directory','directory_receipt','prior_audit','metadata_audit'):
        if plan[field] not in plan['files']:raise ValueError('incomplete original source binding')
    if plan.get('first_filing_date')!='2019-01-01':raise ValueError('preserve original full filing-date population')


def _report_pins(root,plan,report):
    for path,pin in report['pins'].items():
        resolved=Path(path).resolve()
        if resolved.is_relative_to(Path(root).resolve()):
            bound=plan['files'].get(resolved.relative_to(Path(root).resolve()).as_posix())
        else:
            bound=plan['code'].get(str(resolved))
        if bound!=pin:raise ValueError('original audit dependency omitted or substituted')


def _chain(root,plan):
    prior=_json(member(root,plan['prior_audit']))
    metadata=_json(member(root,plan['metadata_audit']))
    _report_pins(root,plan,prior);_report_pins(root,plan,metadata)
    if prior.get('status')!='BOUNDED_INVENTORY_CHAIN_BYTES_AND_EXECUTION_VERIFIED' or prior.get('terminal_exhaustion') is not True:
        raise ValueError('complete original partition chain required')
    if metadata.get('status')!='ORIGINAL_DIRECTORY_METADATA_VERIFIED' or any(metadata.get(key) is not False for key in ('source_payloads_read','exception_closure','history_reference_closure','source_qualified','gate_promotion')):
        raise ValueError('metadata cannot assert source admission')
    if len(prior['partitions'])!=99 or [p['part'] for p in prior['partitions']]!=list(range(99)):
        raise ValueError('complete independently pinned 99-part chain required')
    expected=[{'cik':cik,'member':value[0],'reason':'submission member exceeds explicit byte bound'} for cik,value in EXCEPTIONS.items()]
    if prior['exceptions']!=expected:raise ValueError('exact original exception frontier required')
    records={r['cik']:r for r in metadata['records']}
    if len(records)!=2 or len(metadata['records'])!=2 or set(records)!=set(EXCEPTIONS):raise ValueError('exact two exception metadata records required')
    for cik,(name,size) in EXCEPTIONS.items():
        main=records[cik]['main']
        if main['name']!=name or main['kind']!=1 or main['expanded_bytes']!=size:
            raise ValueError('original main exception identity differs')
    # Paths/receipts cannot be an abbreviated caller-selected inventory.
    for part in prior['partitions']:
        for suffix,pin_key in (('.jsonl','output_identity'),('.jsonl.receipt.json','native_receipt')):
            name=f"task11-submissions-inventory-part{part['part']:04d}-v{part['version']}{suffix}"
            matches=[path for path,pin in prior['pins'].items() if Path(path).name==name and pin==part[pin_key]]
            if len(matches)!=1:raise ValueError('original partition path/identity incomplete')
        guard_name=f"t11-submissions-inventory-part{part['part']:04d}-v{part['version']}.receipt.json"
        if len([path for path,pin in prior['pins'].items() if Path(path).name==guard_name and pin==part['guard_receipt']])!=1:
            raise ValueError('original partition execution identity incomplete')
    return prior,metadata,records


class _ReadOnlyArchive(A.SubmissionsArchive):
    def _build_index(self):
        raise ValueError('authenticated native directory required; rebuild forbidden')


def _central(source,name,expected,maximum):
    row=source._connection().execute('SELECT central_offset,header_offset,record FROM members WHERE name=?',(name,)).fetchone()
    if row is None:raise ValueError('original declared history missing: '+name)
    offset,header,record=row;source._source.seek(offset)
    if source._source.read(len(record))!=record:raise ValueError('original central record changed')
    info=A._one_info(record)
    actual={'name':info.filename,'expanded_bytes':info.file_size,'compressed_bytes':info.compress_size,
            'crc32':info.CRC,'central_record_sha256':hashlib.sha256(record).hexdigest()}
    if any(actual[k]!=expected[k] for k in actual) or info.header_offset+source._concat!=header:
        raise ValueError('independently pinned original member metadata differs')
    if info.file_size>maximum:raise ValueError('private original-member byte bound exceeded')
    raw=source.read(name)  # Unchanged native length/CRC/overlap/source validation.
    if len(raw)!=info.file_size:raise ValueError('original expanded size differs')
    return raw,actual


def _arrays(document):
    if not isinstance(document,dict) or ('filings' in document and not isinstance(document['filings'],dict)):
        raise ValueError('malformed original filing document')
    rows=document.get('filings',{}).get('recent',document)
    if not isinstance(rows,dict) or not isinstance(rows.get('form'),list):raise ValueError('malformed original filing arrays')
    n=len(rows['form'])
    for field in ('accessionNumber','filingDate'):
        if not isinstance(rows.get(field),list) or len(rows[field])!=n:raise ValueError('malformed required original arrays')
    for field in ('reportDate','acceptanceDateTime'):
        if field in rows and (not isinstance(rows[field],list) or len(rows[field])!=n):raise ValueError('malformed optional original arrays')
    if any(len(value)!=n for value in rows.values() if isinstance(value,list)):
        raise ValueError('malformed original parallel array lengths')
    for index in range(n):
        if not isinstance(rows['form'][index],str):raise ValueError('malformed original form')
        if not isinstance(rows['accessionNumber'][index],str):raise ValueError('malformed original accession')
        Q.accession_key(rows['accessionNumber'][index])
        dt.date.fromisoformat(rows['filingDate'][index])
        if rows.get('reportDate',[]) and rows['reportDate'][index]:dt.date.fromisoformat(rows['reportDate'][index])
        if rows.get('acceptanceDateTime',[]) and rows['acceptanceDateTime'][index] is not None and not isinstance(rows['acceptanceDateTime'][index],str):
            raise ValueError('malformed raw original acceptance lead')
    return rows,n


def _verify_axis(source,prior):
    """Re-enumerate every native main boundary without retaining the population."""
    index=0;count=0;total=0;last=None
    for name in source.main_members(through='CIK9999999999.json'):
        if index>=99:raise ValueError('original partition chain omits main population')
        count+=1;total+=1;last=name
        boundary=prior['partitions'][index]
        if name==boundary['through_observed']:
            if count!=boundary['counts']['main_members']:raise ValueError('original main boundary count differs')
            index+=1;count=0
        elif name>boundary['through_observed']:raise ValueError('original main boundary is absent')
    if index!=99 or count or total!=source.main_member_count or total!=prior['counts']['main_members'] or last!=prior['last_main_member']:
        raise ValueError('original partition terminal population differs')
    axis=prior['archive_axis']
    if axis!={'main_members':total,'history_members_in_directory':source.history_member_count,'last_main_member':last,'partitions':99}:
        raise ValueError('original full native directory axis differs')


def _write(path,rows):
    with path.open('x',encoding='utf-8',newline='\n') as handle:
        for row in rows:handle.write(json.dumps(row,sort_keys=True,separators=(',',':'),allow_nan=False)+'\n')
        handle.flush();os.fsync(handle.fileno())


def _owner_matches(value,cik):
    """SEC CIK is an integer or an ASCII digit identifier, never a coercion."""
    if type(value) is int:
        return value==cik
    if isinstance(value,str) and re.fullmatch(r'[0-9]{1,10}',value):
        return int(value)==cik
    return False


def _supplement(root,plan,prior,records,destination):
    ledger=[];exclusions=[]
    with _ReadOnlyArchive(member(root,plan['archive'])) as source, destination.open('x',encoding='utf-8',newline='\n') as sink:
        if identity(source.path)!=plan['files'][plan['archive']] or source.index_path.resolve()!=member(root,plan['directory']) or source._receipt_path.resolve()!=member(root,plan['directory_receipt']):
            raise ValueError('native original archive/directory binding differs')
        _verify_axis(source,prior)
        for cik,(name,_) in EXCEPTIONS.items():
            raw,central=_central(source,name,records[cik]['main'],5*1024*1024)
            document=json.loads(raw)
            if not _owner_matches(document.get('cik'),cik):raise ValueError('original main CIK mismatch')
            rows,count=_arrays(document)
            refs=document.get('filings',{}).get('files')
            if not isinstance(refs,list) or len(refs)>1000:raise ValueError('bounded original history reference inventory required')
            references=[];seen=set()
            for ref in refs:
                if not isinstance(ref,dict) or set(ref)!={'name','filingCount','filingFrom','filingTo'}:raise ValueError('malformed original history reference')
                history=ref['name']
                if not isinstance(history,str) or not history.startswith(name[:-5]+'-submissions-') or '/' in history or '\\' in history or not history.endswith('.json'):
                    raise ValueError('foreign original history reference')
                if history in seen:raise ValueError('duplicate original history reference')
                seen.add(history)
                if type(ref['filingCount']) is not int or ref['filingCount']<0 or dt.date.fromisoformat(ref['filingFrom'])>dt.date.fromisoformat(ref['filingTo']):raise ValueError('invalid original history count/range')
                references.append(dict(ref))
            emitted=0
            for row in Q.submissions_inventory(document,cik=cik,member=name,archive_sha256=source.sha256,first=dt.date.fromisoformat(plan['first_filing_date'])):
                sink.write(json.dumps(row,sort_keys=True,separators=(',',':'))+'\n');emitted+=1
            ledger.append({'cik':cik,'name':name,'raw_sha256':hashlib.sha256(raw).hexdigest(),**central,'array_records':count,'inventory_records':emitted,'references':references})
            del raw,document,rows,refs
            directory={m['name']:m for m in records[cik]['history_directory_members']}
            if len(directory)!=records[cik]['history_directory_count'] or len(directory)!=len(records[cik]['history_directory_members']):raise ValueError('ambiguous directory history metadata')
            native_names=source._connection().execute(
                'SELECT name FROM members WHERE kind=2 AND substr(name,1,?)=? ORDER BY name',
                (len(name[:-5])+1,name[:-5]+'-')
            )
            actual_names=set()
            for (history,) in native_names:
                if len(actual_names)>=1000:raise ValueError('bounded original history directory exceeded')
                actual_names.add(history)
            if actual_names!=set(directory):raise ValueError('complete original history directory differs')
            for ref in references:
                history=ref['name']
                if history not in directory:raise ValueError('declared original history absent from independently pinned directory')
                raw,central=_central(source,history,directory[history],4*1024*1024)
                document=json.loads(raw);rows,count=_arrays(document)
                if 'cik' in document and not _owner_matches(document['cik'],cik):raise ValueError('original history CIK mismatch')
                dates=[dt.date.fromisoformat(value) for value in rows['filingDate']]
                if count!=ref['filingCount'] or (dates and (min(dates).isoformat()!=ref['filingFrom'] or max(dates).isoformat()!=ref['filingTo'])):
                    raise ValueError('original history count/range mismatch')
                emitted=0
                for row in Q.submissions_inventory(document,cik=cik,member=history,archive_sha256=source.sha256,first=dt.date.fromisoformat(plan['first_filing_date'])):
                    sink.write(json.dumps(row,sort_keys=True,separators=(',',':'))+'\n');emitted+=1
                ledger.append({'cik':cik,**central,'raw_sha256':hashlib.sha256(raw).hexdigest(),'array_records':count,'inventory_records':emitted})
                del raw,document,rows,dates
            for unreferenced in sorted(set(directory)-seen):
                exclusions.append({'cik':cik,'member':unreferenced,'reason':'directory_member_not_declared_by_original_main'})
        source.assert_unchanged();sink.flush();os.fsync(sink.fileno())
    return ledger,exclusions


def _reconcile(con,root,plan,prior,supplement,output):
    con.execute('CREATE OR REPLACE TABLE _ownership_exception_rows(cik BIGINT,accession VARCHAR,row_sha256 VARCHAR,payload VARCHAR,origin VARCHAR)')
    def add(path,origin,bounds=None):
        import pyarrow as pa
        count=0;batch=[]
        def flush():
            con.register('_ownership_original_rows',pa.Table.from_pylist(batch))
            try:con.execute('INSERT INTO _ownership_exception_rows SELECT cik,accession,row_sha256,payload,origin FROM _ownership_original_rows')
            finally:con.unregister('_ownership_original_rows')
            batch.clear()
        with path.open(encoding='utf-8') as handle:
            while line:=handle.readline(65537):
                if len(line)>65536 or not line.endswith('\n'):raise ValueError('bounded native inventory row exceeded')
                row=json.loads(line)
                if set(row)!=set(FIELDS):raise ValueError('native inventory schema differs')
                if row['inventory_sha256']!=plan['files'][plan['archive']]['sha256']:raise ValueError('inventory original archive differs')
                if type(row['cik']) is not int or not re.fullmatch(r'\d{18}',row['accession']) or row['form'] not in Q.FORMS or row['filing_date']<plan['first_filing_date']:
                    raise ValueError('native inventory key/filter differs')
                if any(value is not None and not isinstance(value,str) for name,value in row.items() if name!='cik'):
                    raise ValueError('native inventory string schema differs')
                if bounds and not (bounds[0] or '')<f"CIK{row['cik']:010d}.json"<=bounds[1]:
                    raise ValueError('original inventory escapes partition main boundary')
                batch.append({'cik':row['cik'],'accession':row['accession'],'row_sha256':digest(row),
                              'payload':json.dumps(row,sort_keys=True,separators=(',',':')),'origin':origin});count+=1
                if len(batch)==128:flush()
        if batch:flush()
        return count
    previous=None;counts=0;exceptions=[];histories=0
    for part in prior['partitions']:
        if part['after']!=previous:raise ValueError('original partition continuity differs')
        previous=part['through_observed']
        name=f"task11-submissions-inventory-part{part['part']:04d}-v{part['version']}.jsonl"
        path=next(Path(p) for p in prior['pins'] if Path(p).name==name)
        receipt=_json(Path(str(path)+'.receipt.json'))
        code={'thirteenf_quality':plan['code'][str(Path(Q.__file__).resolve())]['sha256'],
              'submissions_archive':plan['code'][str(Path(A.__file__).resolve())]['sha256']}
        if (receipt['schema']!='atx.thirteenf-submissions-inventory-partition/v1' or receipt['source_completeness']!='UNQUALIFIED_PARTITION'
            or receipt['counts']!=part['counts'] or receipt['archive_sha256']!=plan['files'][plan['archive']]['sha256']
            or receipt['first_filing_date']!=plan['first_filing_date'] or receipt['after']!=part['after']
            or receipt['through']!='CIK9999999999.json' or receipt['last_main_member']!=part['through_observed']
            or receipt['output_sha256']!=part['output_identity']['sha256'] or receipt['code_sha256']!=code
            or receipt['archive_source_url']!='https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip'
            or receipt['archive_retrieval_clock'] is not None
            or bool(receipt['bound_reached'])!=(part['part']<98)):
            raise ValueError('original partition receipt contract differs')
        guard_name=f"t11-submissions-inventory-part{part['part']:04d}-v{part['version']}.receipt.json"
        guard=_json(next(Path(p) for p in prior['pins'] if Path(p).name==guard_name))
        expected_return=2 if receipt['exceptions'] or receipt['bound_reached'] else 0
        command=guard['command'];max_main=int(command[command.index('--max-main')+1])
        expected_command=['.venv/Scripts/python.exe','-m','atx_db.alpha_panel.thirteenf_quality','inventory-submissions',
                          '--archive',member(root,plan['archive']).as_posix()]
        if part['after'] is not None:expected_command.extend(['--after',part['after']])
        expected_command.extend(['--through','CIK9999999999.json','--max-main',str(max_main),'--first',plan['first_filing_date'],'--output',path.as_posix()])
        if (not 1<=max_main<=10000 or command!=expected_command or guard['returncode']!=expected_return
            or guard['status']!=('failed' if expected_return else 'completed') or guard['cap_hit'] or guard.get('stop_reason')
            or not 0<guard['job_limit_gb']<=1 or guard['native_peak_job_memory_bytes']>guard['job_cap_bytes']):
            raise ValueError('original guarded native execution differs')
        exceptions.extend(receipt['exceptions']);histories+=receipt['counts']['history_members']
        n=add(path,'prior',(part['after'],part['through_observed']))
        if n!=part['counts']['filings']:raise ValueError('original complete partition count differs')
        counts+=n
    if exceptions!=prior['exceptions'] or histories!=prior['counts']['history_members']:
        raise ValueError('original complete exception/history counts differ')
    if counts!=prior['counts']['filings']:raise ValueError('full original inventory count differs')
    if con.execute("SELECT count(*) FROM (SELECT cik,accession FROM _ownership_exception_rows WHERE origin='prior' GROUP BY 1,2 HAVING count(*)>1)").fetchone()[0] or counts!=prior['unique_cik_accessions']:
        raise ValueError('prior original inventory duplicate population differs')
    added=add(supplement,'supplement')
    if con.execute('SELECT count(*) FROM (SELECT cik,accession FROM _ownership_exception_rows GROUP BY 1,2 HAVING count(DISTINCT row_sha256)>1)').fetchone()[0]:
        raise ValueError('original native inventory key/value disagreement')
    overlap=con.execute("SELECT count(*) FROM (SELECT cik,accession FROM _ownership_exception_rows GROUP BY 1,2 HAVING count(DISTINCT origin)>1)").fetchone()[0]
    if con.execute("SELECT count(*) FROM (SELECT cik,accession FROM _ownership_exception_rows WHERE origin='supplement' GROUP BY 1,2 HAVING count(*)>1)").fetchone()[0]:raise ValueError('duplicate original supplemental key')
    columns=','.join(f"CAST(json_extract_string(payload,'$.{name}') AS {kind}) AS {name}" for name,kind in FIELDS.items())
    con.execute(f'CREATE OR REPLACE TABLE {TABLE} AS SELECT {columns} FROM _ownership_exception_rows QUALIFY row_number() OVER(PARTITION BY cik,accession ORDER BY origin)=1')
    full=con.execute(f'SELECT count(*) FROM {TABLE}').fetchone()[0]
    if full!=counts+added-overlap:raise ValueError('full reconciled original population differs')
    C.copy_to_parquet(con,f'SELECT * FROM {TABLE} ORDER BY cik,accession',output/'submissions_inventory.parquet',row_group_size=4096)
    return {'prior_records':counts,'supplement_records':added,'exact_overlap_keys':overlap,'full_reconciled_records':full}


def _materialize(con,root,output,plan,expected):
    _bounded_connection(con)
    _check(root,plan,expected);prior,metadata,records=_chain(root,plan)
    output.mkdir(parents=True,exist_ok=True)
    ledger,exclusions=_supplement(root,plan,prior,records,output/'supplement.jsonl')
    population=_reconcile(con,root,plan,prior,output/'supplement.jsonl',output)
    _write(output/'original_members.jsonl',ledger)
    C.write_json_atomic(output/'source-plan.json',plan)
    files={name:identity(output/name) for name in ('supplement.jsonl','original_members.jsonl','submissions_inventory.parquet','source-plan.json')}
    result={'schema':SCHEMA,'plan_sha256':expected,'files':files,'population':population,
            'original_exceptions':prior['exceptions'],'directory_exclusions':exclusions,
            'inventory_reference_replay':True,'public_clocks_qualified':False,'source_qualified':False,
            'ownership_qualified':False,'gate_promotion':False,'code':plan['code'],'dependencies':plan['files']}
    _check(root,plan,expected);C.write_json_atomic(output/'manifest.json',result)
    return result


def build_exception_inventory(con,source_root,output_root,plan,expected_plan_sha256):
    """Caller owns externally guarded file-backed128MB/one-thread connection."""
    root=Path(source_root).resolve();output=Path(output_root).resolve()
    if output.exists():raise ValueError('preserve immutable supplemental output')
    stage=output.with_name(output.name+'.partial')
    if stage.exists():raise ValueError('preserve interrupted supplemental output')
    _materialize(con,root,stage,plan,expected_plan_sha256)
    read_exception_inventory(con,root,stage,identity(stage/'manifest.json')['sha256'],expected_plan_sha256)
    if output.exists():raise ValueError('preserve concurrently published supplemental output')
    stage.rename(output)
    return read_exception_inventory(con,root,output,identity(output/'manifest.json')['sha256'],expected_plan_sha256)


def read_exception_inventory(con,source_root,generation,expected_manifest_sha256,expected_plan_sha256):
    """Complete original member/kernel/99partition replay before bounded consumption."""
    _bounded_connection(con)
    root=Path(source_root).resolve();generation=Path(generation).resolve()
    pin=identity(generation/'manifest.json')
    if pin['sha256']!=expected_manifest_sha256:raise ValueError('independent supplemental manifest differs')
    manifest=_json(generation/'manifest.json',8*1024*1024)
    plan=_json(generation/'source-plan.json',8*1024*1024)
    _check(root,plan,expected_plan_sha256)
    if {p.name for p in generation.iterdir()}!=set(manifest['files'])|{'manifest.json'}:raise ValueError('orphan/incomplete supplemental generation')
    for name,expected in manifest['files'].items():
        if identity(member(generation,name))!=expected:raise ValueError('supplemental payload changed')
    with tempfile.TemporaryDirectory(prefix='ownership-original-replay-') as temp:
        replay=_materialize(con,root,Path(temp),plan,expected_plan_sha256)
        if replay!=manifest:raise ValueError('complete original supplemental replay differs')
    _check(root,plan,expected_plan_sha256)
    if {p.name for p in generation.iterdir()}!=set(manifest['files'])|{'manifest.json'}:
        raise ValueError('orphan supplemental generation during replay')
    for name,expected in manifest['files'].items():
        if identity(member(generation,name))!=expected:raise ValueError('supplemental payload changed during replay')
    if identity(generation/'manifest.json')!=pin:raise ValueError('supplemental manifest changed during replay')
    return {'generation':str(generation),'source_root':str(root),'manifest_sha256':expected_manifest_sha256,
            'plan_sha256':expected_plan_sha256,'table':TABLE,'products':{'submissions_inventory.parquet':generation/'submissions_inventory.parquet'},
            'schemas':{'submissions_inventory.parquet':FIELDS},'population':manifest['population'],
            'product_pins':manifest['files'],
            'dependency_pins':plan['files'],'code':plan['code'],'qualification':False,
            'validation':{'inventory_reference_replay':True,'public_clocks_qualified':False,'ownership_qualified':False}}


def iter_exception_rows(admission,batch_size=32):
    """Authenticate one bounded consumption transaction before and after it.

    Completion, explicit close and errors all perform the final integrity check.
    An interrupted iterator produces no successful complete-consumption proof.
    Whole originals are hashed twice per transaction, never per output batch.
    """
    import pyarrow.parquet as pq
    if not 1<=batch_size<=64:raise ValueError('bounded public batch required')
    # Keep the externally pinned binding stable throughout suspended iteration;
    # mutation of the caller's admission cannot replace the final expectations.
    bound=copy.deepcopy(admission)
    generation=Path(bound['generation'])
    def check():
        if identity(generation/'manifest.json')['sha256']!=bound['manifest_sha256']:raise ValueError('stale supplemental admission')
        manifest=_json(generation/'manifest.json',8*1024*1024)
        if {p.name for p in generation.iterdir()}!=set(manifest['files'])|{'manifest.json'}:
            raise ValueError('orphan supplemental generation during consumption')
        for name,pin in manifest['files'].items():
            if identity(member(generation,name))!=pin:raise ValueError('changed supplemental payload')
        plan=_json(generation/'source-plan.json',8*1024*1024)
        if plan['files']!=bound['dependency_pins'] or plan['code']!=bound['code']:raise ValueError('changed supplemental dependencies')
        _check(bound['source_root'],plan,bound['plan_sha256'])
    check()
    try:
        for batch in pq.ParquetFile(generation/'submissions_inventory.parquet').iter_batches(batch_size=batch_size):
            yield from batch.to_pylist()
    finally:check()
