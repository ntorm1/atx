"""Task8 native admission; no warehouse reads on import or post2022 statistics."""
from __future__ import annotations
import calendar as cal
import csv
import datetime as dt
import io
import json
import zipfile
from pathlib import Path
import pyarrow.parquet as pq
from . import char_monthly as M
from . import char_monthly_evidence as E
from . import char_monthly_sources as S
from . import char_monthly_kernels as K
from . import release_inputs as R
from .char_monthly_registry import FREEZE_SHA256,JKP153

BOUNDARY=dt.date(2023,1,1)
JKP_COLUMNS=('month_end','date','location','name','freq','weighting','direction','n_stocks','n_stocks_min','ret')
RAW_JKP_COLUMNS=('location','name','freq','weighting','direction','n_stocks','n_stocks_min','date','ret')


def finite(value):return not isinstance(value,bool) and K.finite(value)


def identity_pin(value):
    if not isinstance(value,dict) or set(value)!={'sha256','bytes'} or type(value['bytes']) is not int or value['bytes']<0 or not isinstance(value['sha256'],str) or len(value['sha256'])!=64 or any(c not in '0123456789abcdef' for c in value['sha256']):raise ValueError('exact typed SHA256/byte identity required')
    return value


def path(root,value):
    if not isinstance(value,str) or '..' in Path(value).parts:raise ValueError('unambiguous input path required')
    return Path(value).resolve() if Path(value).is_absolute() else R.safe_path(root,value)


def reviewed(root,pin,version,config_sha=None):
    if not isinstance(pin,dict) or set(pin)!={'path','identity','expected_sha256'}:raise ValueError('caller reviewed metadata pin required')
    target=path(root,pin['path'])
    identity_pin(pin['identity'])
    if pin['expected_sha256']!=pin['identity'].get('sha256') or R.identity(target)!=pin['identity']:raise ValueError('independent metadata identity mismatch')
    doc=E.read_metadata(target)
    if doc.get('version')!=version or doc.get('registry_sha256')!=FREEZE_SHA256:raise ValueError('metadata version/registry mismatch')
    if config_sha is not None and doc.get('config_sha256')!=config_sha:raise ValueError('Task8 semantic configuration mismatch')
    return doc


def product(root,value):
    if not isinstance(value,dict) or set(value)!={'files','source','clock','admission'} or not value['files'] or any(not isinstance(value[k],str) or not value[k].strip() for k in ('source','clock','admission')):
        raise ValueError('exact independently admitted native product required')
    selected=[]
    for relative,pin in value['files'].items():
        identity_pin(pin)
        target=path(root,relative)
        if R.identity(target)!=pin:raise ValueError('native product bytes changed')
        selected.append(target)
    if len(selected)!=len(set(selected)):raise ValueError('duplicate native product path')
    return selected


def mapping(root,pin,kind,product_value,config_sha):
    doc=reviewed(root,pin,'char_replication_'+kind+'_contract_v1',config_sha)
    if doc.get('product')!=product_value:raise ValueError('native mapping product mismatch')
    catalog=reviewed(root,doc.get('catalog'),'char_replication_'+kind+'_catalog_v1',config_sha)
    record=reviewed(root,doc.get('admission_record'),'char_replication_mapping_admission_v1',config_sha)
    entries=doc.get('entries')
    if not isinstance(entries,dict) or set(entries)!=set(JKP153) or entries!=catalog.get('entries'):raise ValueError('complete authoritative original153 mapping required')
    if record.get('phase')!='frozen_before_any_return_run' or record.get('mapping_sha256')!=R.digest(entries) or record.get('catalog_identity')!=doc['catalog']['identity']:
        raise ValueError('separate pre-return mapping admission required')
    mapped={}
    for name,item in entries.items():
        if item.get('status')=='excluded':
            if set(item)!={'status','reason','metadata_id'} or not isinstance(item['reason'],str) or not item['reason'].strip() or item['metadata_id'] not in catalog.get('metadata',{}):raise ValueError('independent explicit exclusion required')
            continue
        if set(item)!={'status','native_name','sign','weighting','metadata_id'} or item['status']!='mapped' or not isinstance(item['native_name'],str) or not item['native_name'].strip() or type(item['sign']) is not int or item['sign'] not in (-1,1) or item['metadata_id'] not in catalog.get('metadata',{}):raise ValueError('unknown native mapping convention')
        if kind=='jkp' and (item['sign']!=1 or item['weighting']!='vw_cap'):raise ValueError('JKP already signed vw_cap; no second direction')
        if kind=='osap' and item['weighting'] not in ('equal_weight','value_weight','vw_cap'):raise ValueError('source-documented OSAP weighting required')
        native=catalog.get('native',{}).get(item['native_name'])
        if not isinstance(native,dict) or native.get('weighting')!=item['weighting'] or type(native.get('sign_to_own')) is not int or native.get('sign_to_own')!=item['sign'] or native.get('unit')!=('decimal_return' if kind=='jkp' else 'percent') or (kind=='jkp' and (type(native.get('direction')) is not int or native['direction'] not in (-1,1))):
            raise ValueError('native authoritative sign/unit/weighting mismatch')
        mapped[name]=item
    if len({v['native_name'] for v in mapped.values()})!=len(mapped):raise ValueError('ambiguous native factor mapping')
    if kind=='osap' and not mapped:raise ValueError('nonempty independent OSAP overlap required')
    for relative,pin in catalog.get('metadata',{}).items():
        identity_pin(pin)
        if R.identity(path(root,relative))!=pin:raise ValueError('authoritative source metadata changed')
    if not catalog.get('metadata'):raise ValueError('authoritative source metadata inventory required')
    return doc,catalog,mapped


def admit(root,binding,con,config_sha):
    required={'version','registry_sha256','config_sha256','feature_generation','feature_manifest','products','jkp_contract','osap_contract','formation_contract','label_contract','label_source_contract'}
    if set(binding)!=required or binding['version']!='char_replication_bindings_v1' or binding['registry_sha256']!=FREEZE_SHA256 or binding['config_sha256']!=config_sha:
        raise ValueError('complete frozen Task8 bindings required')
    generation=R.safe_path(root,binding['feature_generation'])
    if generation.parent!=(root/'char_monthly/generations').resolve() or R.identity(generation/'manifest.json')!=binding['feature_manifest']:raise ValueError('explicit admitted feature generation required')
    feature=M.validate_generation(root,generation,con)
    if set(binding['products'])!={'jkp','osap','labels','formation_quality'}:raise ValueError('exact native evidence products required')
    products=binding['products']
    for name in products:
        paths=product(root,products[name])
        if name=='osap':
            if len(paths)!=1:raise ValueError('one explicit native PredictorLSretWide CSV required')
            continue
        relation=S.sql(paths)
        con.execute('CREATE OR REPLACE TEMP VIEW cr_'+name+' AS SELECT * FROM '+relation)
        fields={'labels':('formation','entry_date','exit_date'),'formation_quality':('formation',)}.get(name,())
        for field in fields:
            if con.execute(f'SELECT 1 FROM cr_{name} WHERE "{field}" IS NULL OR "{field}">=? LIMIT 1',[BOUNDARY]).fetchone():raise ValueError('complete evidence window exceeds pre2023 boundary')
    if tuple(r[0] for r in con.execute('DESCRIBE cr_jkp').fetchall())!=JKP_COLUMNS:raise ValueError('unsupported native JKP schema')
    for relation,key in (('labels','formation,cik'),('formation_quality','formation,security_id')):
        if con.execute(f'SELECT 1 FROM cr_{relation} GROUP BY {key} HAVING count(*)>1 LIMIT 1').fetchone():raise ValueError('duplicate native evidence key')
    jd,jc,jmap=mapping(root,binding['jkp_contract'],'jkp',products['jkp'],config_sha)
    od,oc,omap=mapping(root,binding['osap_contract'],'osap',products['osap'],config_sha)
    archive=jd.get('raw_archive');review_file(root,archive)
    if not isinstance(jd.get('zip_member'),str):raise ValueError('exact JKP archive member required')
    for selected in product(root,products['jkp']):
        footer=pq.ParquetFile(selected).metadata.metadata or {}
        try:entry=json.loads(footer[b'atx.benchmark'])
        except (KeyError,ValueError):raise ValueError('native JKP producer footer required') from None
        if entry.get('dataset')!='bench_jkp_us_factors' or entry.get('loader_version')!='x3-benchmarks-v1' or entry.get('info',{}).get('unit')!='decimal_return':raise ValueError('unsupported JKP native producer/unit')
        sources=entry.get('sources')
        if not isinstance(sources,list) or len(sources)!=1 or any(sources[0].get(k)!=archive['identity'][k] for k in ('sha256','bytes')):raise ValueError('JKP native archive lineage mismatch')
    formation=E.reviewed_metadata(root,binding['formation_contract'],'char_monthly_formation_classification_v1')
    labels=E.reviewed_metadata(root,binding['label_contract'],'char_monthly_label_contract_v1')
    if formation.get('product')!=products['formation_quality'] or labels.get('product')!=products['labels'] or not formation.get('classification_proofs'):raise ValueError('independent formation/window inventory mismatch')
    source=reviewed(root,binding['label_source_contract'],'char_replication_label_source_v1',config_sha)
    if source.get('convention')!='unadjusted_close_split_cash_per_prior_share_v1' or source.get('currency')!='USD':raise ValueError('unsupported native complete-label convention')
    for name in ('prices','actions'):product(root,source[name])
    calendar=[r[0] for r in con.execute('SELECT session_date FROM cm_calendar ORDER BY 1').fetchall()]
    if len(calendar)!=len(set(calendar)):raise ValueError('duplicate admitted calendar session')
    axes=E.requested_axes(calendar);formations={d for d in axes.values() if d is not None}
    for name in ('labels','formation_quality'):
        dates=con.execute(f'SELECT DISTINCT formation FROM cr_{name}').fetchall()
        if any(d not in formations for d, in dates):raise ValueError('evidence outside exact requested formation axis')
    for label_formation,entry,exit in con.execute('SELECT formation,entry_date,exit_date FROM cr_labels').fetchall():
        sessions=E.holding_sessions(calendar,label_formation)
        if not sessions or entry!=sessions[0] or exit!=sessions[-1]:raise ValueError('native label is not a complete actual holding month')
    label_columns={r[0] for r in con.execute('DESCRIBE cr_labels').fetchall()}
    if label_columns!={'formation','cik','security_id','entry_date','exit_date','ret','return_certified'}:raise ValueError('protected or unsupported native label columns')
    return {'feature':feature,'generation':generation,'jkp_doc':jd,'jkp_catalog':jc,'jkp_mapping':jmap,'osap_doc':od,'osap_catalog':oc,'osap_mapping':omap,'formation':formation,'labels':labels,'label_source':source,'calendar':calendar}


def review_file(root,pin):
    if not isinstance(pin,dict) or set(pin)!={'path','identity','expected_sha256'}:raise ValueError('explicit independently pinned actual source required')
    identity_pin(pin['identity'])
    if pin['expected_sha256']!=pin['identity'].get('sha256') or R.identity(path(root,pin['path']))!=pin['identity']:
        raise ValueError('explicit independently pinned actual source required')
    return path(root,pin['path'])


def benchmark_month(root,context,con,holding):
    year,month=map(int,holding.split('-'));end=dt.date(year,month,cal.monthrange(year,month)[1])
    if not (dt.date(2013,1,1)<=end<BOUNDARY):raise ValueError('benchmark outside exact replication axis')
    metadata=con.execute('SELECT date,location,freq,weighting,direction FROM cr_jkp WHERE month_end=?',[end]).fetchall()
    if any((d.year,d.month)!=(year,month) or d>=BOUNDARY or location!='usa' or frequency!='monthly' or weight!='vw_cap' or type(direction) is not int or direction not in (-1,1) for d,location,frequency,weight,direction in metadata):raise ValueError('native benchmark metadata exceeds requested convention/window before return read')
    native=S.records(con,'SELECT * FROM cr_jkp WHERE month_end=? ORDER BY name',[end])
    rows={}
    for row in native:
        if row['location']!='usa' or row['freq']!='monthly' or row['weighting']!='vw_cap' or row['month_end']!=end or (row['date'].year,row['date'].month)!=(year,month) or type(row['direction']) is not int or row['direction'] not in (-1,1):raise ValueError('native JKP country/month/weight/direction mismatch')
        if row['name'] in rows:raise ValueError('duplicate native JKP factor/month')
        catalog=context['jkp_catalog']['native'].get(row['name'])
        if not catalog or catalog.get('direction')!=row['direction']:raise ValueError('JKP factor outside authoritative native catalog')
        rows[row['name']]=row
    # Reconstruct the complete native month directly from independently pinned archive.
    doc=context['jkp_doc'];expected={}
    with zipfile.ZipFile(review_file(root,doc['raw_archive'])) as archive:
        if archive.namelist().count(doc['zip_member'])!=1:raise ValueError('ambiguous/missing JKP CSV member')
        with archive.open(doc['zip_member']) as raw:
            reader=csv.DictReader(io.TextIOWrapper(raw,encoding='utf-8-sig'))
            if tuple(reader.fieldnames or ())!=RAW_JKP_COLUMNS:raise ValueError('unsupported native JKP source columns')
            for item in reader:
                date=dt.date.fromisoformat(item['date'])
                if (date.year,date.month)!=(year,month):continue
                name=item['name']
                if name in expected:raise ValueError('duplicate native JKP source month')
                expected[name]={'month_end':end,'date':date,**{k:item[k] for k in ('location','name','freq','weighting')},
                    **{k:int(item[k]) for k in ('direction','n_stocks','n_stocks_min')},'ret':None if item['ret'] in ('','NA') else float(item['ret'])}
    if R.digest(expected)!=R.digest(rows):raise ValueError('native JKP month differs from complete source ZIP reconstruction')
    jkp={n:rows.get(v['native_name'],{}).get('ret') for n,v in context['jkp_mapping'].items()}
    file=next(iter(context['osap_doc']['product']['files']));observed=None
    with path(root,file).open(encoding='utf-8-sig',newline='') as stream:
        reader=csv.DictReader(stream)
        if not reader.fieldnames or reader.fieldnames[0]!='date' or len(reader.fieldnames)!=len(set(reader.fieldnames)) or set(reader.fieldnames[1:])!=set(context['osap_catalog']['native']):raise ValueError('native OSAP wide header differs from full authoritative signal catalog')
        for item in reader:
            date=dt.date.fromisoformat(item['date'])
            if (date.year,date.month)!=(year,month):continue
            if observed is not None:raise ValueError('duplicate native OSAP holding month')
            if date>=BOUNDARY:raise ValueError('heldout native OSAP before conversion')
            observed=item
    osap={n:None if observed is None or observed[v['native_name']] in ('','NA') else float(observed[v['native_name']])/100*v['sign'] for n,v in context['osap_mapping'].items()}
    return jkp,osap,{'jkp_native_rows':len(rows),'osap_native_rows':int(observed is not None)}


def native_label(root,label,calendar,windows,source,con):
    """Reuse reviewed Task7 complete-window admission, then read actual economic cells."""
    admitted=E.qualify_label(root,label,calendar,windows)
    if admitted.get('label_reason'):return admitted
    if admitted.get('return_certified') is not True or not finite(admitted.get('ret')):return {**admitted,'ret':None,'label_reason':'unqualified_native_label'}
    proof=E.reviewed_metadata(root,windows[f"{label['formation']}/{label['cik']}"],'char_monthly_complete_window_v1')
    prior=None
    for i,item in enumerate([proof['start_mark']]+proof['observations']):
        if not finite(item.get('price' if i==0 else 'total_return')):return {**admitted,'ret':None,'return_certified':False,'label_reason':'untyped_native_complete_window_proof'}
        day=dt.date.fromisoformat(item['session_date'])
        if day>=BOUNDARY:raise ValueError('heldout native price/action before return arithmetic')
        selected={}
        for kind,product_name in (('price_source','prices'),('corporate_action_source','actions')):
            pin=item[kind];target=review_file(root,pin)
            matches=[p for p,q in source[product_name]['files'].items() if path(root,p)==target and q==pin['identity']]
            if len(matches)!=1:raise ValueError('window native source outside independently admitted product')
            fields='close' if product_name=='prices' else 'split_multiplier,cash_dividend,terminal_treatment'
            values=con.execute(f'SELECT {fields} FROM {S.sql([target])} WHERE security_id=? AND session_date=?',[label['security_id'],day]).fetchall()
            if len(values)>1:raise ValueError('ambiguous native price/action observation')
            if not values:return {**admitted,'ret':None,'return_certified':False,'label_reason':'missing_native_complete_price_or_action'}
            selected[product_name]=values[0]
        close=selected['prices'][0];split,cash,terminal=selected['actions']
        if not finite(close) or close<=0 or not finite(split) or split<=0 or not finite(cash) or cash<0 or terminal!='none':return {**admitted,'ret':None,'return_certified':False,'label_reason':'unsupported_native_price_action_or_terminal'}
        if i==0:
            if abs(close-item['price'])>1e-12*max(1,abs(close)):raise ValueError('start mark differs from actual native close')
        else:
            actual=(close*split+cash)/prior-1
            if abs(actual-item['total_return'])>1e-12*max(1,abs(actual)):raise ValueError('complete label differs from native price/split/dividend economics')
        prior=close
    return admitted


def month(root,binding,context,con,holding):
    axes=E.requested_axes(context['calendar']);year,number=map(int,holding.split('-'));formation=axes[year,number]
    jkp,osap,counters=benchmark_month(root,context,con,holding)
    missing='missing_formation_or_holding_calendar' if formation is None or not E.holding_sessions(context['calendar'],formation) else None
    rows=[];labels=[];proof_pins={}
    def track(pin):
        target=review_file(root,pin);proof_pins[str(target)]=pin['identity']
    if missing is None:
        if formation>=BOUNDARY:raise ValueError('heldout formation before portfolios')
        relative=f'month={formation:%Y-%m}/char_monthly.parquet'
        if relative not in context['feature']['files']:missing='missing_requested_feature_history'
        else:
            rows=S.records(con,'SELECT * FROM '+S.sql([context['generation']/relative])+' WHERE session_date=?',[formation])
            quality=S.records(con,'SELECT * FROM cr_formation_quality WHERE formation=?',[formation])
            audits={r['security_id']:r for r in quality}
            for i,row in enumerate(rows):
                audit=audits.get(row['security_id'],{});key=audit.get('classification_proof');proofs={}
                if key in context['formation']['classification_proofs']:
                    pin=context['formation']['classification_proofs'][key];track(pin)
                    proofs[key]=E.reviewed_metadata(root,pin,'char_monthly_classification_proof_v1')
                rows[i]=E.qualify_formation(row,audit,proofs)
                if rows[i].get('classification')=='primary':
                    proof=proofs.get(key,{})
                    if type(proof.get('nyse')) is not bool or proof.get('nyse') is not audit.get('nyse') or not finite(proof.get('issuer_me')) or proof.get('issuer_me')!=audit.get('me'):
                        rows[i].update(certified=False,nyse=None,formation_reason='missing_independent_NYSE_size_cap_proof')
            labels=S.records(con,'SELECT * FROM cr_labels WHERE formation=?',[formation])
            qualified=[]
            for label in labels:
                pin=context['labels'].get('windows',{}).get(f"{formation}/{label['cik']}")
                if pin:
                    track(pin);proof=E.reviewed_metadata(root,pin,'char_monthly_complete_window_v1')
                    for observation in [proof.get('start_mark',{})]+proof.get('observations',[]):
                        for field in ('price_source','corporate_action_source'):
                            if observation.get(field):track(observation[field])
                qualified.append(native_label(root,label,context['calendar'],context['labels'].get('windows',{}),context['label_source'],con))
            labels=qualified
    population=E.formation_population(rows);blocked=population['unknown_rows'] or population['unresolved_issuers']
    invalid_cap=any(r.get('classification')=='primary' and (r.get('certified') is not True or not finite(r.get('me')) or r['me']<=0 or type(r.get('nyse')) is not bool) for r in rows if r.get('univ_all_common') is True)
    by_label={r['cik']:r for r in labels};primaries=[]
    for row in rows:
        if row.get('univ_all_common') is True and row.get('classification')=='primary':
            label=by_label.get(row['cik'],{'entry_date':None,'exit_date':None,'ret':None,'return_certified':False})
            if label.get('security_id',row['security_id'])!=row['security_id']:raise ValueError('label differs from independently proved primary')
            primaries.append({**row,**label})
    outcomes={}
    for name in JKP153:
        value,reason=(None,missing) if missing else (None,'unknown_independent_formation_population') if blocked else (None,'invalid_independent_issuer_size_cap') if invalid_cap else E.tercile(primaries,name,formation)
        outcomes[name]={'own':value,'jkp':jkp.get(name) if finite(jkp.get(name)) else None,'osap':osap.get(name) if finite(osap.get(name)) else None,'reason':reason,
            'jkp_mapping_status':context['jkp_doc']['entries'][name]['status'],'osap_mapping_status':context['osap_doc']['entries'][name]['status']}
    for file,pin in proof_pins.items():
        if R.identity(Path(file))!=pin:raise ValueError('classification/window/native source changed during monthly evaluation')
    return {'holding_month':holding,'formation':str(formation) if formation else None,'population':population,'outcomes':outcomes,'benchmark_counts':counters,'proof_pins':proof_pins}
