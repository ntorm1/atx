"""Connection-taking Task6 replay; unchanged native portfolio and source kernels.

The caller admits the full twelve-role original source contract before setup.
This module never calls the native350MB publisher or substitutes its globals.
Every monthly unit includes its original daily and completed monthly population.
"""
import datetime as dt
import math
import re
from pathlib import Path
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from . import _factor_native as N, _factor_sources as S, factors as F
from . import risk_inputs as R
from . import private_tier1_admission as A


def bounded_rows(iterator,maximum=20000):
    """Enforce the original cross-section bound during consumption."""
    result=[]
    for row in iterator:
        if len(result)>=maximum:raise ValueError('bounded native cross-section exceeded')
        result.append(row)
    return result


def compare_unit(con,rows,schema,actual,target):
    """Retain a bounded expected unit and compare full typed populations."""
    target=Path(target);target.parent.mkdir(parents=True,exist_ok=True)
    if target.exists():raise ValueError('native replay attempt already exists')
    if not pq.read_schema(actual).equals(schema,check_metadata=False):
        raise ValueError('native factor/risk physical schema differs')
    buffer=[];count=0
    with pq.ParquetWriter(target,schema,compression='zstd') as writer:
        for row in rows:
            buffer.append(row)
            if len(buffer)==64:
                writer.write_table(pa.Table.from_pylist(buffer,schema=schema));count+=len(buffer);buffer.clear()
        if buffer:writer.write_table(pa.Table.from_pylist(buffer,schema=schema));count+=len(buffer)
    expected=A.sql([target]);published=A.sql([actual])
    if con.execute(f'SELECT 1 FROM ((SELECT * FROM {expected} EXCEPT ALL SELECT * FROM {published}) UNION ALL (SELECT * FROM {published} EXCEPT ALL SELECT * FROM {expected})) LIMIT 1').fetchone():
        raise ValueError('complete native factor/risk unit replay differs')
    return {'rows':count,'expected':A.pin(target),'published':A.pin(actual)}


def replay_factor_outputs(con,spec,generation,manifest,scratch,source_identity):
    """All original monthly units, including failed cells and daily populations.

    The public adapter must independently admit every original source before
    calling this phase; native manifest integrity alone is not source admission.
    """
    N.require_factor_lineage(manifest,spec,source_identity)
    construction=manifest.get('construction_asof')
    construction=dt.date.fromisoformat(construction) if construction else None
    dates=N.sessions(con,construction);months=required_months(dates,construction)
    files={f'month={year:04d}-{month:02d}.parquet' for year,month in months}
    if set(manifest['files'])!=files:raise ValueError('incomplete native factor month inventory')
    measured={}
    for year,month in months:
        leaf=f'month={year:04d}-{month:02d}.parquet'
        measured[leaf]=compare_unit(con,month_rows(con,spec,dates,year,month,construction),
            N.FACTOR_SCHEMA,A.member(generation,leaf),Path(scratch)/leaf)
    return {'required_months':len(months),'units':measured,'source_quality_certified':False}


def replay_risk_outputs(con,generation,manifest,scratch,start,end):
    """Replay every required native calendar/identity session population.

    The public adapter separately measures the unchanged native coverage gate.
    """
    all_dates=[row[0] for row in con.execute('SELECT DISTINCT session_date FROM (SELECT session_date FROM t6_calendar UNION ALL SELECT session_date FROM t6_universe UNION ALL SELECT session_date FROM t6_member_equity) ORDER BY session_date').fetchall()]
    dates=[date for date in all_dates if start<=date<=end]
    files={'session='+str(date)+'.parquet' for date in dates}
    if set(manifest['files'])!=files:raise ValueError('incomplete native risk session inventory')
    measured={}
    for date in dates:
        leaf='session='+str(date)+'.parquet'
        measured[leaf]=compare_unit(con,risk_day_rows(con,date,all_dates),risk_schema(),
            A.member(generation,leaf),Path(scratch)/leaf)
    return {'required_sessions':len(dates),'units':measured,'source_quality_certified':False}


def benchmark_validation(con,spec,own):
    """Original120-month benchmark gate using the admitted connection.

    Native validate_benchmarks opens its own250MB connection; only its explicit
    source projection is sequenced here, with unchanged F.benchmark_gates.
    """
    paths=[A.member(Path(spec['own_factors']['manifest']).parent,leaf)
        for leaf in own['files'] if leaf.endswith('.parquet')]
    data=bounded_rows(N.rows(con,f"SELECT * FROM {S.parquet_relation(paths)} WHERE freq='M' AND session_date>=DATE '2013-01-01' AND session_date<DATE '2023-01-01' ORDER BY session_date"),120)
    merged={}
    for item in spec['benchmarks']:
        manifest=Path(item['manifest']);native=F.bind_snapshot(manifest,item['sha256'],item['stage'])
        if item['file'] not in native['files'] or item['units'] not in ('decimal','percent'):
            raise ValueError('benchmark output/units not explicitly bound')
        mapping=item['columns']
        if any(not re.fullmatch('[A-Za-z_][A-Za-z0-9_]*',value) for value in [item['date_column'],*mapping.values()]):
            raise ValueError('benchmark mapping requires column identifiers')
        if any(key not in F.FACTORS for key in mapping):raise ValueError('unknown canonical factor mapping')
        factor=1 if item['units']=='decimal' else .01
        selected=','.join(f'{value}*{factor} AS {key}' for key,value in mapping.items());date=item['date_column']
        query=f"SELECT CAST({date} AS DATE) AS session_date,{selected} FROM {S.parquet_relation([A.member(manifest.parent,item['file'])])} WHERE CAST({date} AS DATE)>=DATE '2013-01-01' AND CAST({date} AS DATE)<DATE '2023-01-01'"
        seen=set()
        for row in N.rows(con,query):
            key=row['session_date'].year,row['session_date'].month
            if key in seen:raise ValueError('duplicate benchmark source month')
            seen.add(key);target=merged.setdefault(key,{'session_date':row['session_date']})
            if any(column in target for column in mapping):raise ValueError('overlapping benchmark source definitions')
            target.update({column:row[column] for column in mapping})
    return {'window':'2013-01..2022-12','required_months':120,
        'gates':F.benchmark_gates(data,list(merged.values())),
        'own_manifest_sha256':spec['own_factors']['sha256'],
        'benchmark_bindings':spec['benchmarks'],'config_sha256':F.config_hash()}


def month_rows(con,spec,dates,year,month,construction_asof=None):
    ends={}
    for day in dates:ends[day.year,day.month]=day
    first=dt.date(year,month,1);last=ends.get((year,month))
    prior=max((day for day in dates if day<first),default=None)
    june_year=year if month>=7 else year-1;june=ends.get((june_year,6))
    annual_labels={};q_ia_labels={};monthly_labels={};roe_labels={};eg_labels={};beta_values={}
    if june:
        annual={row['cik']:row for row in N.rows(con,S.annual_sql(june,N._cutoff(june)))}
        formation=N.formation_issuers(con,june);december=ends.get((june_year-1,12))
        prior_cap={row['cik']:row['me'] for row in N.formation_issuers(con,december) if row['certified']} if december else {}
        signals={'bm':{key:(row['be']/prior_cap[key] if row['be'] and row['be']>0 and prior_cap.get(key,0)>0 else None) for key,row in annual.items()},
            'op':{key:row['op'] for key,row in annual.items()},'investment':{key:row['investment'] for key,row in annual.items()}}
        annual_labels={key:N.labels(formation,values,key) for key,values in signals.items()}
        q_ia_labels=N.labels(formation,{key:row['investment'] if row['be'] is not None and row['be']>0 and
            not (row['sic'] is not None and 6000<=row['sic']<=6999) else None for key,row in annual.items()},'investment')
    if prior:
        pd=N.formation_issuers(con,prior)
        for key,length,skip in [('momentum',11,1),('strev',1,0),('ltrev',48,12)]:
            monthly_labels[key]=N.labels(pd,N.signal_history(con,prior,length,skip),key)
        quarterly={row['cik']:row for row in N.rows(con,S.quarterly_sql(N._cutoff(prior)))}
        roe_labels=N.labels(pd,{key:row['roe'] for key,row in quarterly.items()},'roe')
        training=(N.native_training(con,prior,dates) if spec['sources']['eg_training'] is None else
            N.rows(con,f"SELECT * FROM t6_eg_training WHERE fit_cutoff<TIMESTAMPTZ '{N._cutoff(prior).isoformat()}' ORDER BY fit_month,cik"))
        predictors=bounded_rows(N.rows(con,N.eg_predictors_sql(prior,N._cutoff(prior))))
        eg_labels=N.labels(pd,F.eg_forecast(training,predictors,N._cutoff(prior)),'eg')
        beta_values=N.native_betas(con,prior,dates)
    complete_last=con.execute('SELECT max(session_date) FROM t6_calendar WHERE year(session_date)=? AND month(session_date)=?',[year,month]).fetchone()[0]
    monthly=[] if construction_asof and (complete_last is None or complete_last>=construction_asof) else [('M',first,last)]
    periods=monthly+[('D',day,day) for day in dates if day.year==year and day.month==month]
    cutoff=N._cutoff(max((day for day in dates if day<construction_asof),default=F.START)) if construction_asof else None
    rf_cols={row[0] for row in con.execute('DESCRIBE t6_rf').fetchall()}
    for freq,start,end in periods:
        stamp=end or dt.date(year,month,28);lagday=prior if freq=='M' else max((day for day in dates if day<start),default=None)
        data=N.issuer_period(con,start,end,lagday,construction_asof=construction_asof) if end else []
        match=(f"date_trunc('month',session_date)=date_trunc('month',DATE '{stamp}')" if freq=='M' else f"session_date=DATE '{stamp}'")
        clock_guard=f" AND available_at<TIMESTAMPTZ '{cutoff.isoformat()}'" if construction_asof and 'available_at' in rf_cols else ' AND false' if construction_asof else ''
        rf_select='rf,CAST(available_at AS TIMESTAMPTZ) AS available_at' if construction_asof and 'available_at' in rf_cols else 'rf'
        rf_rows=list(N.rows(con,f"SELECT {rf_select} FROM t6_rf WHERE freq='{freq}' AND {match}"+clock_guard))
        rf=rf_rows[0]['rf'] if len(rf_rows)==1 else None
        eligible=[F.Stock(row['cik'],row['me'],row['ret'],row['nyse'],{},N._cutoff(lagday),True) for row in data if row['certified']]
        values=dict.fromkeys(F.FACTORS);market=F.vw(eligible)
        values['MktRF']=market-rf if market is not None and rf is not None else None;smb=[]
        for name,signal,sign in [('HML','bm',1),('RMW','op',1),('CMA','investment',-1),('UMD','momentum',1),('STREV','strev',-1),('LTREV','ltrev',-1)]:
            held=(annual_labels if signal in annual_labels else monthly_labels).get(signal,{})
            values[name],size=N.held_factor(data,held,sign)
            if name in ('HML','RMW','CMA'):smb.append(size)
        values['SMB']=F.mean(smb);values.update(N.held_q(data,q_ia_labels,roe_labels));values['q_EG'],_=N.held_factor(data,eg_labels)
        current={row['cik']:row for row in data if row['certified']}
        bab=[F.Stock(row['cik'],current.get(row['cik'],row)['me'],current.get(row['cik'],{}).get('ret'),row['nyse'],
            {'beta':beta_values.get(row['cik'])},N._cutoff(prior),True) for row in (pd if prior else []) if row['certified']]
        values['BAB']=F.bab(bab,rf)
        available=max(N._cutoff(stamp),rf_rows[0]['available_at']) if construction_asof and len(rf_rows)==1 else N._cutoff(stamp)
        yield {'freq':freq,'session_date':stamp,'available_at':available,'n_universe':len(data),'n_qualified':len(eligible),
            'missing_reason':None if all(value is not None for value in values.values()) else 'missing_qualified_history_or_required_portfolio_leg',**values}


def required_months(dates,construction_asof=None):
    return sorted({(day.year,day.month) for day in dates if day>=F.START}) if construction_asof else [(year,month) for year in range(2013,2023) for month in range(1,13)]


def risk_schema():
    return pa.schema([('session_date',pa.date32()),('security_id',pa.int64()),('cik',pa.int64()),
        ('available_at',pa.timestamp('us',tz='UTC')),('univ_all_common',pa.bool_()),('univ_r3000p',pa.bool_()),
        ('beta_252',pa.float64()),*[(f'beta_ff5_{key}',pa.float64()) for key in R.FF5],
        ('ivol_capm_252',pa.float64()),('ivol_ff5_252',pa.float64()),('ff49',pa.int64()),
        *[(key,pa.float64()) for key in R.STYLES],
        *[(key,pa.float64()) for key in ('adv_21','adv_63','spread_cs_21','si_to_io','si_shares','ftd_quantity')],
        ('on_threshold_list',pa.bool_()),('cap_qualified',pa.bool_()),('risk_missing_reason',pa.string())])


def risk_day_rows(con,day,all_dates):
    previous=max((date for date in all_dates if date<day),default=None)
    if previous is None:raise ValueError('native risk day has no complete prior calendar session')
    cutoff=N._cutoff(previous)
    primitive=bounded_rows(N.rows(con,S.risk_primitives_sql(day,cutoff)))
    exposures={};keep=[date for date in all_dates if date<=previous][-252:]
    if keep:
        rf_cols={row[0] for row in con.execute('DESCRIBE t6_rf').fetchall()}
        rf_guard=f" AND rf.available_at<TIMESTAMPTZ '{cutoff.isoformat()}'" if 'available_at' in rf_cols else ' AND false' if day.year>=2023 else ''
        query=f"""SELECT p.security_id,p.session_date,CASE WHEN p.ret_guarded THEN NULL ELSE p.ret END AS ret,rf.rf AS RF,
            f.MktRF,f.SMB,f.HML,f.RMW,f.CMA,f.available_at AS factor_available_at FROM t6_prices p
            LEFT JOIN t6_own_factors f ON f.session_date=p.session_date AND f.available_at<TIMESTAMPTZ '{cutoff.isoformat()}'
            LEFT JOIN t6_rf rf ON rf.session_date=p.session_date AND rf.freq='D'{rf_guard}
            WHERE p.session_date BETWEEN DATE '{keep[0]}' AND DATE '{previous}' AND p.session_date<DATE '{day}'
            AND p.security_id IN(SELECT security_id FROM t6_universe WHERE session_date=DATE '{day}')
            ORDER BY p.security_id,p.session_date"""
        active=None;history=[]
        def flush():
            if active is None:return
            clean=[{key:(value if value is not None or key.endswith('available_at') else float('nan')) for key,value in row.items()} for row in history]
            ff=R.exposure(day,clean,cutoff=cutoff)
            capm=R.regress([row['ret'] for row in clean],[[row['MktRF']] for row in clean],[row['RF'] for row in clean])
            exposures[active]={**ff,'ivol_capm_252':capm['idio_vol'],
                'volatility':float(np.std([row['ret'] for row in clean[-21:]],ddof=1)) if len(clean)>=21 and all(math.isfinite(row['ret']) for row in clean[-21:]) else None}
        for row in N.rows(con,query):
            if row['security_id']!=active:flush();active=row['security_id'];history=[]
            history.append(row)
            if len(history)>252:raise ValueError('duplicate price session or oversized exposure window')
        flush()
    output=[];momentum=N.signal_history(con,previous,11,1)
    for row in primitive:
        ex=exposures.get(row['security_id'],{});qualified=bool(row['cap_qualified'] and row['me'] is not None and row['me']>0)
        value=row['be_q0']/row['me'] if row['be_q0'] is not None and row['me'] and row['me']>0 else None
        style={'size':math.log(row['me']) if row['me'] and row['me']>0 else None,'value':value,
            'momentum':momentum.get(row['cik']),'volatility':ex.get('volatility'),
            'liquidity':-row['amihud_21'] if row['amihud_21'] is not None else None,
            'growth':row['at_q0']/row['at_q4']-1 if row['at_q0'] is not None and row['at_q4'] and row['at_q4']>0 else None,
            'leverage':row['debt_at'],'quality':row['gprof']}
        if not qualified:style=dict.fromkeys(R.STYLES)
        result={key:row.get(key) for key in ('session_date','security_id','cik','univ_all_common','univ_r3000p','ff49','adv_21','adv_63','spread_cs_21','si_to_io','si_shares','ftd_quantity','on_threshold_list')}
        result.update(style,available_at=N._cutoff(day),cap_qualified=qualified,beta_252=ex.get('beta_252'),
            ivol_ff5_252=ex.get('ivol_ff5_252'),ivol_capm_252=ex.get('ivol_capm_252'),risk_missing_reason=None if ex.get('beta_252') is not None else 'missing_own_factor_history')
        result.update({f'beta_ff5_{key}':value for key,value in zip(R.FF5,ex.get('ff5_betas',[None]*5))});output.append(result)
    for style in R.STYLES:
        normalized=R.zscores([row[style] if row['univ_all_common'] and row['cap_qualified'] else None for row in output])
        for row,value in zip(output,normalized):row[style]=value
    yield from output
