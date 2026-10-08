"""Private Task1 dated primitive qualification; no static directory decisions.

The pure classification kernel is separate from physical source admission.
Its input is reconstructed native evidence, never the legacy panel's flags.
"""
from __future__ import annotations

import datetime as dt
import math
import contextlib
import copy
import json
from pathlib import Path
from dataclasses import dataclass
from . import security_master as S, sec_filings as F
from .private_index_pit import mark

VERSION='index-dated-sources-v2'


@contextlib.contextmanager
def market_inputs(specs, con):
    """Hold all four fixed native source bindings for one index replay.

    No caller row loader, borrowed source PASS or per-session full derivation.
    The shared independently reviewed native reader supplies the complete source
    and code authority. Until that implementation and its actual originals are
    admitted, this conditional dependency cannot publish an index generation.
    """
    from . import private_tier1_market_sources as M
    if not all(callable(getattr(M,name,None)) for name in ('admit','consuming')):
        raise RuntimeError('shared native market admission/consumption implementation pending')
    specs=copy.deepcopy(specs)
    required={'prices':{'price_quality.parquet','line_history.parquet','calendar_realized_quality.parquet'},
              'planned_calendar':{'planned_calendar.parquet'},
              'shares':{'shares_quality.parquet'},
              'liquidity':{'liquidity_quality.parquet'}}
    if set(specs)!=set(required):
        raise ValueError('fixed prices/planned_calendar/shares/liquidity source bindings required')
    admissions={name:M.admit(name,specs[name],con) for name in required}
    if any(not products<=set(admissions[name]['products']) for name,products in required.items()):
        raise ValueError('complete native market and qualified-cell inventories required')
    with contextlib.ExitStack() as stack:
        for name,admission in admissions.items():
            products=stack.enter_context(M.consuming(admission,con))
            if products!=admission['products']:
                raise ValueError('native market consumption binding differs')
        yield admissions


@contextlib.contextmanager
def fundamental_inputs(spec, con):
    """Fixed full all85/R23 native foundation, separately pinned for Task1.

    Preserve the independent256MB native proof connection; the index consumer
    remains128MB/one thread. No flat financial manifest or caller product path
    can replace the complete original preparation/extract/native leaf replay.
    """
    from . import private_tier1_accounting as K, private_tier1_admission as A
    from . import fund_batch_release as B, fund_release_runtime as R, common as C
    spec=copy.deepcopy(spec)
    A.guard(con)
    code={str(Path(module.__file__).resolve()):A.pin(module.__file__)
          for module in (K,A,B,R,C,__import__(__name__,fromlist=['']))}
    if code!=spec['expected_index_foundation_code']:
        raise ValueError('independently reviewed index foundation reader code differs')
    review_path=A.absolute(spec['independent_review']['path'])
    if A.pin(review_path)!=spec['independent_review']['pin']:
        raise ValueError('independent index foundation review identity differs')
    binding={'lake_root':spec['lake_root'],'fundamentals':spec['fundamentals']}
    if json.loads(review_path.read_text())!={
            'schema':'atx.index-fundamental-reader-review/v1','status':'scoped_pass',
            'binding_sha256':A.digest(binding),'code':code}:
        raise ValueError('independent full native foundation review prerequisite pending')
    A.check_pins(spec['dependency_pins'])
    leaf,snapshot,dependencies=K.fundamental(spec)
    dependencies.update(code)
    dependencies[str(review_path)]=spec['independent_review']['pin']
    if dependencies!=spec['dependency_pins']:
        raise ValueError('complete independently expected index foundation inventory differs')
    A.check_pins(dependencies)
    try:
        yield {'qualified_fundamental_leaf':leaf,'snapshot':snapshot,
               'dependency_pins':dependencies,'source_quality_certified':False}
    finally:
        try:
            A.check_pins(dependencies)
        finally:
            A.guard(con)


def _native_relation(admission, required, forbidden=()):
    """Unique typed role from a fixed native admission, never caller files.

    Shared market basenames are still being finalized. Semantics are explicit
    columns of the native validated quality/primitive products; ambiguity fails.
    This helper is internal to source replay and cannot admit arbitrary Parquet.
    """
    matches=[key for key,schema in admission['schemas'].items()
             if set(required)<=set(schema) and not set(forbidden)&set(schema)]
    if len(matches)!=1:
        raise ValueError('required unique fixed native source schema unavailable: '+','.join(required))
    path=Path(admission['products'][matches[0]])
    return "read_parquet('"+path.as_posix().replace("'","''")+"',hive_partitioning=false)"


def prepare_market_relations(market, con):
    """Fixed original native inputs; source scopes must surround every use.

    Never reconstruct source qualification from directory flags or dates alone.
    Native schemas must expose explicit qualifiers/clock/source lineage. Draft
    or unsupported schemas refuse the bridge until their real reader is ready.
    """
    roles={
        'index_price':('prices',('security_id','session_date','raw_close','ticker','is_index',
                                'available_at','qualified','reason','source_sha256')),
        'index_lines':('prices',('security_id','ticker','last_session','exit_reason',
                                'exit_available_at','vendor_data_end')),
        'index_realized':('prices',('session_date','available_at','qualified','reason')),
        'index_planned':('planned_calendar',('calendar_date','session_date','is_session','vintage_id',
                                            'covered_start','covered_end','available_at','qualified','reason')),
        'index_shares':('shares',('security_id','session_date','shrout','available_at',
                                  'shrout_qualified','reason','source_sha256')),
        'index_finra':('shares',('security_id','observation_date','dissemination_date','issue_name',
                                 'symbol','market_class','available_at','mapping_qualified','reason','source_sha256')),
        'index_identity':('shares',('security_id','cik','link_tier','identity_status','identity_cutoff',
                                    'ticker','valid_from','valid_to',
                                    'available_at','mapping_qualified','reason','source_sha256')),
        'index_liquidity':('liquidity',('security_id','session_date','adv_63','available_at',
                                        'qualified','reason','source_sha256'))}
    relations={name:_native_relation(market[family],fields,
                ('security_id','calendar_date') if name=='index_realized' else ())
               for name,(family,fields) in roles.items()}
    for name,relation in relations.items():
        con.execute(f'CREATE OR REPLACE TEMP VIEW {name} AS SELECT * FROM {relation}')
    return relations


def calendar_bindings(con, rank_dates, *, through):
    """Fixed native calendar roles, preserving a separate plan per rank.

    Consume only inside market_inputs. A complete original calendar vintage
    supplies the daily axis, including explicit non-session days; filtering
    qualified sessions first would hide unresolved closures and weekday rules.
    Later revisions never rewrite an earlier rank's planned effective start.
    """
    realized=con.execute('SELECT session_date,available_at,qualified,reason '
        'FROM index_realized ORDER BY session_date').fetchall()
    if (not realized or len({row[0] for row in realized})!=len(realized) or
            any(not row[2] or row[1] is None for row in realized)):
        raise ValueError('complete independently qualified realized calendar unavailable')
    calendar=[row[0] for row in realized]
    if through>calendar[-1]:
        raise ValueError('requested vendor endpoint exceeds admitted realized calendar')
    plans={}
    for rank in sorted(set(rank_dates)):
        stamp=mark(rank)
        unresolved=con.execute('SELECT count(*) FROM index_planned '
            'WHERE available_at IS NULL AND covered_start<=? AND covered_end>=?',
            [through,rank]).fetchone()[0]
        if unresolved:
            raise ValueError('original planned calendar publication clock unresolved; no stale fallback')
        # Do not let unsupported visible revisions fall back to older plans.
        versions=con.execute('SELECT vintage_id,min(available_at),max(available_at),'
            'min(covered_start),max(covered_end),bool_and(qualified),count(*) '
            'FROM index_planned WHERE available_at<=? GROUP BY vintage_id', [stamp]).fetchall()
        if not versions:
            raise ValueError('independently public planned calendar unavailable at rank')
        newest=max(row[2] for row in versions)
        latest=[row for row in versions if row[2]==newest]
        if len(latest)!=1 or latest[0][1]!=newest or not latest[0][5]:
            raise ValueError('ambiguous/unqualified latest planned calendar vintage')
        version=latest[0]
        rows=con.execute('SELECT calendar_date,session_date,is_session,available_at,'
            'covered_start,covered_end,qualified FROM index_planned '
            'WHERE vintage_id=? ORDER BY calendar_date',[version[0]]).fetchall()
        start,end=version[3:5]
        if start is None or end is None or start>end:
            raise ValueError('planned daily-axis coverage missing')
        expected=(end-start).days+1
        if len(rows)!=expected or any(row[0]!=start+dt.timedelta(days=i) or
            row[2] is None or row[3]!=newest or row[4]!=start or row[5]!=end or
            not row[6] or (row[1]!=row[0] if row[2] else row[1] is not None)
            for i,row in enumerate(rows)):
            raise ValueError('complete original planned daily axis/population differs')
        plans[rank]=[row[0] for row in rows if row[2]]
    return calendar,plans


def _fundamental_facts(con, admission, cik, rank_date):
    """Original native event values for conditional candidate arithmetic only.

    The caller holds fundamental_inputs for the whole consumption. Its full
    all85/R23 replay authenticates the immutable native values, not their
    economic completeness or source-clock accuracy. Those original independent
    gates remain separate prerequisites to any public acceptance.
    """
    path=Path(admission['qualified_fundamental_leaf'])/'events.parquet'
    expected=admission['dependency_pins'].get(str(path.resolve()))
    if expected is None:
        raise ValueError('native event product absent from complete financial admission')
    quoted=path.as_posix().replace("'","''")
    fields=('available_at','period_end','accession','staleness_days','sic_in_force',
            'sale_ttm','ni_q','ni_ttm')
    # Bound the Python population by one issuer's latest event clock, retaining
    # all ties. Select vintage before checking NULL/staleness; no stale fallback.
    rows=con.execute('SELECT '+','.join(fields)+f" FROM read_parquet('{quoted}') "
        'WHERE cik=? AND available_at<=? QUALIFY available_at=max(available_at) OVER()',
        [cik,mark(rank_date).replace(tzinfo=None)]).fetchall()
    facts={}
    if rows:
        newest_period=max(row[1] or dt.date.min for row in rows)
        rows=[row for row in rows if (row[1] or dt.date.min)==newest_period]
        row=rows[0]
        stamp=row[0].replace(tzinfo=dt.timezone.utc)
        reasons=[]
        if len(rows)!=1:
            reasons.append('ambiguous_latest_original_native_financial_event')
        if row[1] is None or row[3] is None or not 0<=(rank_date-row[1]).days<=row[3]:
            reasons.append('original_native_financial_staleness_or_period_unresolved')
        facts={field:DatedFact(value,stamp,expected['sha256'],source_accession=row[2],
                    period=row[1],exclusions=tuple(reasons))
               for field,value in zip(('sale_ttm','ni_q','ni_ttm'),row[5:])}
    # Original panel SIC is a separately clocked native SIC event with550-day
    # age from its public clock, not the financial statement period end.
    sic_path=Path(admission['qualified_fundamental_leaf'])/'sic_events.parquet'
    sic_pin=admission['dependency_pins'].get(str(sic_path.resolve()))
    if sic_pin is None:
        raise ValueError('complete independently admitted native SIC product missing')
    quoted=sic_path.as_posix().replace("'","''")
    rows=con.execute(f"SELECT clock_utc,accession,sic FROM read_parquet('{quoted}') "
        'WHERE cik=? AND clock_utc<=? QUALIFY clock_utc=max(clock_utc) OVER()',
        [cik,mark(rank_date).replace(tzinfo=None)]).fetchall()
    if rows:
        clock,accession,value=rows[0]
        stamp=clock.replace(tzinfo=dt.timezone.utc)
        facts['sic']=DatedFact(value,stamp,sic_pin['sha256'],source_accession=accession,
            period=stamp.date(),exclusions=() if len(rows)==1 else ('ambiguous_latest_native_SIC',))
    return facts


def rank_snapshot(con, foundation, rank_date):
    """Complete native rank-line census under the already held source scopes.

    Every original price line at this rank remains in the census, including
    exclusions; no future last-session, directory or legacy panel flag is read.
    Only 64 identities and one line's primitive vintages are held at a time.
    Financial values are conditional diagnostics; acceptance gates remain open.
    """
    from collections import Counter, OrderedDict
    selected=[];excluded=Counter();identity_states=Counter();total=0;last=None
    excluded_me=0.
    issuer_facts=OrderedDict()
    def facts_from(view, columns, date_column, qualifier, where, parameters):
        fields=columns+('available_at','source_sha256',date_column,qualifier,'reason')
        rows=con.execute('SELECT '+','.join(fields)+' FROM '+view+' WHERE '+where,
                         parameters).fetchall()
        out=[]
        for row in rows:
            values=dict(zip(fields,row))
            clock=values['available_at']
            if clock is not None and clock.tzinfo is None:
                raise ValueError('native market primitive public clock must be typed UTC')
            reasons=() if values[qualifier] else ('native_market_cell_unqualified:'+str(values['reason']),)
            out.extend((field,DatedFact(values[field],clock,values['source_sha256'],
                period=values[date_column],exclusions=reasons)) for field in columns)
        return out
    while True:
        where='session_date=?'+(' AND security_id>?' if last is not None else '')
        ids=con.execute('SELECT DISTINCT security_id FROM index_price WHERE '+where+
            ' ORDER BY security_id LIMIT 64',[rank_date]+([] if last is None else [last])).fetchall()
        if not ids:
            break
        for (sid,) in ids:
            total+=1
            population=facts_from('index_price',('raw_close','is_index'),'session_date','qualified',
                                  'security_id=? AND session_date=?',[sid,rank_date])
            population=[('close' if field=='raw_close' else field,fact) for field,fact in population]
            population+=facts_from('index_shares',('shrout',),'session_date','shrout_qualified',
                                  'security_id=? AND session_date=?',[sid,rank_date])
            population+=facts_from('index_finra',('issue_name','symbol','market_class'),
                'observation_date','mapping_qualified','security_id=? AND observation_date<=?',[sid,rank_date])
            population+=facts_from('index_identity',('cik','link_tier','identity_status','identity_cutoff'),
                'valid_from','mapping_qualified',
                'security_id=? AND valid_from<=? AND (valid_to IS NULL OR valid_to>=?)',
                [sid,rank_date,rank_date])
            primitives=latest_facts(population,rank_date)
            identity=primitives.get('cik');tier=primitives.get('link_tier')
            status=primitives.get('identity_status')
            identity_states[str(status.value) if _known(status,mark(rank_date)) else 'unavailable_or_ambiguous']+=1
            if (_known(status,mark(rank_date)) and status.value=='linked' and
                    _known(identity,mark(rank_date)) and _known(tier,mark(rank_date)) and
                    tier.value in ('strict','name')):
                if identity.value not in issuer_facts:
                    issuer=_fundamental_facts(con,foundation,identity.value,rank_date)
                    forms=con.execute('SELECT form,available_at,source_sha256,source_accession,'
                        'filing_date,qualified_clock,reason FROM index_sec_regime WHERE cik=? '
                        'AND filing_date<=?',[identity.value,rank_date]).fetchall()
                    issuer.update(latest_facts((('form',DatedFact(row[0],row[1],row[2],
                        source_accession=row[3],period=row[4],
                        exclusions=() if row[5] else ('native_SEC_clock_unqualified:'+row[6],)))
                        for row in forms),rank_date))
                    issuer_facts[identity.value]=issuer
                    if len(issuer_facts)>64:
                        issuer_facts.popitem(last=False)
                else:
                    issuer_facts.move_to_end(identity.value)
                primitives.update(issuer_facts[identity.value])
            classification=classify_rank(rank_date,primitives)
            if not classification['eligible']:
                excluded.update(classification['reason'])
                close,shares=primitives.get('close'),primitives.get('shrout')
                if (_known(close,mark(rank_date),max_age=0) and _known(shares,mark(rank_date)) and
                        isinstance(close.value,(int,float)) and isinstance(shares.value,(int,float)) and
                        math.isfinite(close.value) and math.isfinite(shares.value) and
                        close.value>0 and shares.value>0):
                    excluded_me+=close.value*shares.value
                continue
            liquidity=latest_facts(facts_from('index_liquidity',('adv_63',),'session_date',
                'qualified','security_id=? AND session_date=?',[sid,rank_date]),rank_date).get('adv_63')
            selected.append({'security_id':sid,'cik':classification['cik'],
                'me':classification['market_equity'],
                'ni_q':primitives['ni_q'].value if _known(primitives.get('ni_q'),mark(rank_date)) else None,
                'ni_ttm':primitives['ni_ttm'].value if _known(primitives.get('ni_ttm'),mark(rank_date)) else None,
                'adv_63':liquidity.value if _known(liquidity,mark(rank_date),max_age=0) else None})
        last=ids[-1][0]
    return selected,{'original_rank_price_lines':total,'eligible_lines':len(selected),
        'excluded_lines':total-len(selected),'exclusion_reasons':dict(excluded),
        'identity_states':dict(identity_states),'known_excluded_line_market_equity':excluded_me,
        'directory_decided':0,'backfill_cik':{'selected_company_keys':0,
        'excluded_only_is_operating':0},'source_quality_certified':False,
        'financial_source_economic_model_and_coverage_gates':'PENDING'}


@contextlib.contextmanager
def source_replay(spec, con):
    """Fixed physical native source composition for one full index replay.

    Expected identities are supplied independently of candidate manifests.
    Holding the contexts across all product writing/comparison avoids replaying
    the all85 foundation or hashing original market/SEC sources at every rank.
    This admits diagnostic engineering only; original acceptance gates remain.
    """
    from . import indexes as I, private_index_pit as P, private_index_release as L
    from . import private_tier1_admission as A, common as C
    from .. import universe_us_listed as U
    spec=copy.deepcopy(spec)
    if spec['interface_version']!='index-public-source-v2':
        raise ValueError('fixed reviewed index public source interface required')
    A.guard(con)
    plan_path=A.absolute(spec['source_plan_path'])
    L.check(plan_path,spec['source_plan_pin'])
    if plan_path.stat().st_size>16<<20:
        raise ValueError('bounded index source plan exceeded')
    plan=json.loads(plan_path.read_text())
    if plan['schema']!='atx.index-source-plan/v2' or L.digest(plan)!=spec['expected_source_plan_sha256']:
        raise ValueError('independently expected original index source plan differs')
    code={str(Path(module.__file__).resolve()):L.pin(module.__file__)
          for module in (I,P,L,C,S,F,U,A,__import__(__name__,fromlist=['']))}
    if code!=spec['expected_code']:
        raise ValueError('independently reviewed original index projection code differs')
    review_path=A.absolute(spec['independent_review']['path'])
    L.check(review_path,spec['independent_review']['pin'])
    if review_path.stat().st_size>1<<20:
        raise ValueError('bounded index review binding exceeded')
    if json.loads(review_path.read_text())!={
        'schema':'atx.index-source-review/v2','status':'scoped_pass',
        'source_plan_sha256':spec['expected_source_plan_sha256'],'code':code}:
        raise ValueError('independent index source/projection engineering review pending')
    dependencies={**code,str(plan_path):spec['source_plan_pin'],
                  str(review_path):spec['independent_review']['pin']}
    A.check_pins(spec['dependency_pins'])
    with market_inputs(plan['market'],con) as market, fundamental_inputs(plan['fundamentals'],con) as fund:
        prepare_market_relations(market,con)
        prepare_sec_regime(plan['sec'],con)
        for admission in [*market.values(),fund]:
            for path,item in admission['dependency_pins'].items():
                if path in dependencies and dependencies[path]!=item:
                    raise ValueError('incompatible native source dependency identities')
                dependencies[path]=item
        for path,item in plan['sec']['dependency_pins'].items():
            if path in dependencies and dependencies[path]!=item:
                raise ValueError('incompatible original SEC dependency identities')
            dependencies[path]=item
        if dependencies!=spec['dependency_pins']:
            raise ValueError('complete independently expected index native source/code inventory differs')
        A.check_pins(dependencies)
        try:
            yield {'plan':plan,'market':market,'foundation':fund,
                   'dependency_pins':dependencies,'spec':spec,'source_quality_certified':False}
        finally:
            try:
                A.check_pins(dependencies)
            finally:
                A.guard(con)


def replay_products(binding, con, *, prior_exits=()):
    """Original index kernels on complete admitted inputs; bounded daily stream.

    This is callable only within source_replay. Prior facts must come from the
    fixed independently admitted immutable previous generation, never a caller
    Parquet relation. Native engineering/source-economic statuses are distinct.
    """
    from . import indexes as I, private_index_pit as P
    plan=binding['plan']
    end=dt.date.fromisoformat(plan['data_end'])
    endpoints=con.execute('SELECT DISTINCT vendor_data_end FROM index_lines').fetchall()
    if endpoints!=[(end,)]:
        raise ValueError('complete independently admitted vendor endpoint differs')
    actual_end=con.execute('SELECT max(session_date) FROM index_price').fetchone()[0]
    if actual_end!=end or plan['schedule_last_year']<end.year+1:
        raise ValueError('complete price endpoint/independent next-reconstitution horizon unavailable')
    calendar=con.execute('SELECT session_date FROM index_realized ORDER BY session_date').fetchall()
    calendar=[row[0] for row in calendar]
    ranks=[]
    for rank,rebalance in I.RUSSELL:
        nominal=dt.date.fromisoformat(rank)
        if nominal<=end:
            observed=I.last_session_on_or_before(calendar,nominal)
            if observed is None:
                raise ValueError('complete original Russell rank calendar unavailable')
            ranks.append(('russell',observed,dt.date.fromisoformat(rebalance)))
    # Include a known rank even when its scheduled reconstitution is later than
    # the observed endpoint; never use the vendor extent to censor selections.
    for year in range(2018,end.year+1):
        for month in (3,6,9,12):
            nominal=dt.date(year,month,1)-dt.timedelta(days=1)
            if nominal<=end:
                observed=I.last_session_on_or_before(calendar,nominal)
                if observed is None:
                    raise ValueError('complete original quarterly rank calendar unavailable')
                ranks.append(('sp500',observed,I.third_friday(year,month)))
    calendar,planned=calendar_bindings(con,[row[1] for row in ranks],through=end)
    census={}
    class Snapshots:
        def __contains__(self,rank):
            return rank in planned
        def __getitem__(self,rank):
            rows,details=rank_snapshot(con,binding['foundation'],rank)
            if rank in census and census[rank]!=details:
                raise ValueError('same rank native exclusion census changed during consumption')
            census[rank]=details
            return rows
    recons=I.rank_reconstitutions(Snapshots(),ranks)
    line_states={}
    if con.execute('SELECT count(*) FROM (SELECT security_id FROM index_lines '
                   'GROUP BY security_id HAVING count(*)>1)').fetchone()[0]:
        raise ValueError('ambiguous complete native line history population')
    offset=None
    while True:
        rows=con.execute('SELECT security_id,last_session,exit_reason,exit_available_at '
            'FROM index_lines '+('WHERE security_id>? ' if offset is not None else '')+
            'ORDER BY security_id LIMIT 64', [] if offset is None else [offset]).fetchall()
        if not rows:
            break
        for sid,last,reason,clock in rows:
            if sid in line_states or last is None:
                raise ValueError('ambiguous/missing complete native line history')
            if reason is not None:
                if clock is None or clock!=P.mark(P.next_session(calendar,last)):
                    raise ValueError('native independently available exit boundary unavailable')
                if reason not in ('explicit_delisting_date','ceased_before_observed_vendor_end'):
                    raise ValueError('unsupported original native exit reason')
            # A missing source-qualified cessation clock cannot mint an exit.
            if last<end and clock is None:
                continue
            line_states[sid]=(last,last if reason=='explicit_delisting_date' else None)
        offset=rows[-1][0]
    schedule=I.scheduled_reconstitutions(2018,plan['schedule_last_year'])
    products=P.derive(recons,calendar,line_states,schedule,data_end=end,
                      prior_exits=prior_exits,planned_calendars=planned)
    receipt=P.receipt(products,recons,census)
    receipt['unresolved_exit_lines']=con.execute('SELECT count(*) FROM index_lines '
        'WHERE last_session<vendor_data_end AND exit_available_at IS NULL').fetchone()[0]
    gates={'russell':I.russell_gate(products['recon_summary']),
        'original_source_economic_model_coverage':'PENDING',
        'qualified_identity_A0_and_strict_name':'PENDING',
        'official_overlap':'GAP_NOT_MEASURED','consumer_acceptance':'PENDING'}
    products['membership']=P.iter_membership(products['constituents'],products['exits'],calendar,end)
    return products,receipt,gates


@dataclass(frozen=True)
class DatedFact:
    value: object
    available_at: dt.datetime | None
    source_sha256: str
    source_accession: str | None = None
    period: dt.date | None = None
    dependency_clocks: tuple[dt.datetime, ...] = ()
    exclusions: tuple[str, ...] = ()


def _known(fact, stamp, *, max_age=None):
    if (not isinstance(fact,DatedFact) or not isinstance(fact.available_at,dt.datetime)
        or fact.available_at.tzinfo is None):
        return False
    if (fact.available_at>stamp or len(fact.source_sha256)!=64 or
        any(c not in '0123456789abcdef' for c in fact.source_sha256) or fact.exclusions or
        any(clock.tzinfo is None or clock>stamp for clock in fact.dependency_clocks)):
        return False
    if max_age is not None and (fact.period is None or
                              not 0<=(stamp.date()-fact.period).days<=max_age):
        return False
    return fact.value is not None


def classify_rank(rank_date, primitives, *, directory=None):
    """Conditional original rule from dated primitives; retain every exclusion.

    Source admission must construct the DatedFacts from authenticated complete
    original source populations. Passing this pure function does not admit bytes.
    ``directory`` is deliberately never consulted, including its derived flags.
    """
    stamp=mark(rank_date)
    identity=primitives.get('identity_status')
    unlinked=_known(identity,stamp) and identity.value=='proven_unlinked'
    required=('issue_name','symbol','market_class','identity_status','is_index','close','shrout')
    required+=('identity_cutoff',) if unlinked else ('cik','link_tier','form','sic','sale_ttm')
    reasons=[]
    for key in required:
        limit=0 if key=='close' else (45 if key in ('issue_name','symbol','market_class') else (
              550 if key in ('sic','sale_ttm') else None))
        if not _known(primitives.get(key),stamp,max_age=limit):
            reasons.append('missing_or_ineligible_dated_'+key)
    result={'qualified':False,'eligible':False,'directory_decided':0,
            'reason':reasons,'security_type':None,'exchange':None,'cik':None,
            'is_etf':None,'is_adr':None,'is_adr_likely':None,'is_fpi':None,
            'is_lp':None,'is_royalty_trust':None,'is_spac':None,'is_operating':None,
            'is_index':None,'market_equity':None}
    if reasons:
        return result
    v={key:fact.value for key,fact in primitives.items() if _known(fact,stamp)}
    # P9 round3 R7.2(d): the same dated identity is used for grouping and
    # operating-company eligibility. Backfill cannot decide either outcome.
    if unlinked:
        if (v['identity_cutoff']!=stamp or identity.available_at!=stamp or
                primitives['identity_cutoff'].available_at!=stamp):
            result['reason']=['proven_unlinked_census_does_not_bind_exact_rank_clock']
            return result
        # These NULLs are original unlinked semantics established by full
        # strict/name census, not defaults from unavailable mapping evidence.
        v.update(cik=None,form=None,sic=None,sale_ttm=None)
    elif v['identity_status']!='linked' or v['link_tier'] not in ('strict','name'):
        result['reason']=['unsupported_identity_tier_for_grouping_and_operating']
        return result
    kind=S.classify_finra(v['issue_name'],v['symbol'])
    exchange=S.EXCHANGE_BY_CLASS.get(str(v['market_class']).upper())
    regime=F.classify_regime(v['form']) if not unlinked else None
    periodic=primitives['form'].period if not unlinked else None
    grace=550 if regime in ('fpi_20f','canadian_40f') else 400
    if not unlinked and (regime is None or periodic is None or not 0<=(rank_date-periodic).days<=grace):
        reasons.append('unresolved_or_stale_original_filer_regime')
    if exchange is None:
        reasons.append('unresolved_original_exchange')
    if not isinstance(v['is_index'],bool):
        reasons.append('unresolved_original_index_namespace')
    if not unlinked and (isinstance(v['cik'],bool) or not isinstance(v['cik'],int) or v['cik']<=0):
        reasons.append('unresolved_dated_company_identity')
    for key in (('close','shrout') if unlinked else ('close','shrout','sale_ttm')):
        if isinstance(v[key],bool) or not isinstance(v[key],(int,float)) or not math.isfinite(v[key]):
            reasons.append('invalid_native_'+key)
    if reasons:
        return result
    if not unlinked and (isinstance(v['sic'],bool) or not isinstance(v['sic'],(int,float)) or
        not math.isfinite(v['sic']) or int(v['sic'])!=v['sic']):
        reasons.append('invalid_native_sic')
        return result
    sic=int(v['sic']) if not unlinked else None
    etf=kind in ('ETF','fund','ETN') or regime=='fund'
    adr=kind=='ADR'
    fpi=regime in ('fpi_20f','canadian_40f')
    adr_likely=adr or (regime=='fpi_20f' and kind=='common_unverified')
    lp=kind=='LP'
    royalty=sic==6792 or 'ROYALTY TR' in str(v['issue_name']).upper()
    spac=kind=='spac' or (sic==6770 and v['sale_ttm']==0)
    operating=(not v['is_index'] and kind in ('common','common_unverified','ADR','REIT','LP')
               and not etf and not spac and (unlinked or regime=='domestic'))
    eligible=(kind in ('common','common_unverified','REIT') and not adr and not adr_likely
              and not fpi and not lp and not royalty and operating
              and exchange in ('XNYS','XNAS','XASE','ARCX','BATS')
              and v['close']>=1 and v['shrout']>0)
    result.update(qualified=True,eligible=eligible,security_type=kind,exchange=exchange,cik=v['cik'],
        is_etf=etf,is_adr=adr,is_adr_likely=adr_likely,is_fpi=fpi,is_lp=lp,
        is_royalty_trust=royalty,is_spac=spac,is_operating=operating,is_index=v['is_index'],
        market_equity=v['close']*v['shrout'],reason=[] if eligible else ['native_eligibility_screen'])
    return result


def latest_facts(population,rank_date):
    """Bounded per-line native evidence vintage choice before qualification.

    A visible unsupported amendment wins over an older supported observation.
    Equal-clock/period ambiguity is retained as an exclusion, never deduplicated.
    Physical admission supplies the complete original population upstream.
    """
    from dataclasses import replace
    best={};ties={};unresolved={}
    stamp=mark(rank_date)
    for field,fact in population:
        if not isinstance(fact,DatedFact):
            raise ValueError('native dated fact requires an authenticated public UTC clock')
        if fact.available_at is None:
            # An unclocked original dated record must not enable stale fallback.
            # A period strictly after this rank is not a historical blocker;
            # otherwise retain an explicit excluded field, never a fake clock.
            if fact.period is None or fact.period<=rank_date:
                unresolved.setdefault(field,fact)
            continue
        if fact.available_at.tzinfo is None:
            raise ValueError('native dated fact requires an authenticated public UTC clock')
        if fact.available_at<=stamp:
            key=(fact.available_at,fact.period or dt.date.min)
            previous=best.get(field)
            old_key=None if previous is None else (previous.available_at,previous.period or dt.date.min)
            if old_key is None or key>old_key:
                best[field]=fact;ties[field]=1
            elif key==old_key:
                ties[field]+=1
    selected={}
    for field,fact in best.items():
        if ties[field]!=1:
            selected[field]=replace(fact,value=None,
                exclusions=fact.exclusions+('ambiguous_latest_dated_fact',))
        else:
            selected[field]=fact
    for field,fact in unresolved.items():
        selected[field]=replace(fact,value=None,available_at=None,
            exclusions=fact.exclusions+('unresolved_original_public_clock_no_stale_fallback',))
    return selected


def sec_regime_rows(document,*,cik,source,source_sha256,proved_clocks,owner_accessions):
    """Minimal complete form projection through unchanged SEC flattening.

    This pure projection does not authenticate ``document``. Physical admission
    supplies the original pinned archive/member or original URL/receipt, complete
    main/history reference closure, index Accepted proofs and issuer-owner census.
    Raw submissions acceptance Z strings are comparison leads, never proof clocks.
    Every original regime-form row, including rejected clocks, is retained.
    """
    from zoneinfo import ZoneInfo
    from . import filing_text_native as T
    if source==0 and int(document.get('cik',-1))!=cik:
        raise ValueError('original SEC main CIK mismatch')
    block=F.columnar_block(document,source)
    count=len(block.get('accessionNumber') or [])
    for key in ('accessionNumber','form','filingDate'):
        if not isinstance(block.get(key),list) or len(block[key])!=count:
            raise ValueError('malformed complete original SEC regime census array')
    rows=F.flatten_filings(block,cik,source)
    # Native flattening supplies the exact original field names/types. Preserve
    # optional missing arrays as explicit row reasons, not fabricated timestamps.
    for index in range(count):
        form=rows['form'][index]
        regime=F.classify_regime(form)
        if regime is None:
            continue
        accession=block['accessionNumber'][index]
        filed=F._date10(block['filingDate'][index])
        clock=proved_clocks.get((cik,accession))
        reasons=[]
        if filed is None:
            reasons.append('missing_original_filing_date')
        if clock is None:
            reasons.append('missing_original_index_accepted_clock')
        if (cik,accession) not in owner_accessions:
            reasons.append('unproven_original_issuer_owner_chain')
        leads=block.get('acceptanceDateTime') or []
        lead=T.acceptance(leads[index]) if index<len(leads) else None
        if clock and lead:
            et=lead.replace(tzinfo=None).replace(tzinfo=ZoneInfo('America/New_York')).astimezone(dt.timezone.utc)
            if clock not in (lead,et):
                reasons.append('submission_clock_disagrees_with_original_index')
        yield {'cik':cik,'source_accession':accession,'form':form,'filer_regime':regime,
               'filing_date':filed,'available_at':clock,'source_sha256':source_sha256,
               'qualified_clock':bool(clock) and not reasons,'reason':reasons,
               'form_fact':DatedFact(form,clock,source_sha256,accession,filed,exclusions=tuple(reasons))}


def planned_sessions(facts,rank_date):
    """Conditional calendar proof clock check; no vendor-end substitution.

    The fixed shared planned-calendar reader must authenticate every original
    document and full schedule before constructing these DatedFacts. Rejecting
    any unsupported date prevents silently moving the effective first session.
    """
    stamp=mark(rank_date)
    sessions=[]
    for fact in facts:
        if not _known(fact,stamp) or not isinstance(fact.value,dt.date) or isinstance(fact.value,dt.datetime):
            raise ValueError('independent planned exchange calendar unavailable by rank mark')
        sessions.append(fact.value)
    if not sessions or sessions!=sorted(set(sessions)):
        raise ValueError('complete unique original planned exchange schedule required')
    return sessions


def iter_sec_regime(spec, con):
    """Complete independently bound original main/history regime projection.

    This reader supplies original form/clock diagnostics, not market membership
    or source-quality certification. The independent source plan must prove its
    complete issuer census; transport receipts alone cannot supply completeness.
    It uses the original native sixteen-MiB JSON bound and Accepted proof parser,
    with one complete physical check before/finally the whole bounded replay.
    """
    from . import common as C, filing_text as FT, filing_text_native as N
    from . import private_tier1_family_sources as G
    spec=copy.deepcopy(spec)
    root=Path(spec['source_root']).resolve(strict=True)
    G._guard(con,root)
    plan_path=N.member(root,spec['plan_path'])
    if N.pin(plan_path)!=spec['plan_pin']:
        raise ValueError('independent complete index SEC source plan differs')
    plan=json.loads(N.bounded_bytes(root,spec['plan_path'],16*1024*1024))
    code={str(Path(module.__file__).resolve()):N.pin(module.__file__)
          for module in (C,F,FT,N,G,G.R,__import__(__name__,fromlist=['']))}
    if (plan['schema']!='atx.index-sec-regime-source-plan/v1' or
            N.digest(plan)!=spec['expected_plan_sha256'] or
            plan['code']!=code or plan['runtime']!=N.runtime_identity()):
        raise ValueError('independent native SEC plan/code/runtime differs')
    review_path=N.member(root,spec['independent_review']['path'])
    if N.pin(review_path)!=spec['independent_review']['pin']:
        raise ValueError('independent original SEC review identity differs')
    review=json.loads(N.bounded_bytes(root,spec['independent_review']['path'],1_000_000))
    if review!={'schema':'atx.index-sec-regime-review/v1','status':'scoped_pass',
                 'source_plan_sha256':spec['expected_plan_sha256'],'code':code,
                 'census_evidence_sha256':plan['files'][plan['census_evidence']]['sha256']}:
        raise ValueError('complete original SEC census review prerequisite pending')
    dependencies={str(N.member(root,name)):expected for name,expected in plan['files'].items()}
    dependencies.update(code)
    dependencies[str(plan_path)]=spec['plan_pin']
    dependencies[str(review_path)]=spec['independent_review']['pin']
    if dependencies!=spec['dependency_pins']:
        raise ValueError('independently expected complete SEC source/code inventory differs')
    def check():
        for path,expected in dependencies.items():
            if N.pin(Path(path))!=expected:
                raise ValueError('changed independently pinned original SEC dependency')
        if N.runtime_identity()!=plan['runtime']:
            raise ValueError('native SEC runtime changed during replay')
    def original(record):
        path,receipt=record['path'],N.receipt_json(root,record['receipt'])
        identity=plan['files'][path]
        if (receipt.get('http_status')!=200 or receipt.get('error') or
                receipt.get('url')!=record['url'] or
                any(receipt.get(k)!=identity[k] for k in ('sha256','bytes'))):
            raise ValueError('original SEC submissions receipt mismatch')
        document=json.loads(N.bounded_bytes(root,path,16*1024*1024))
        block=F.columnar_block(document,record['source'])
        count=len(block.get('accessionNumber') or [])
        for field in ('accessionNumber','form','filingDate','reportDate','primaryDocument'):
            if not isinstance(block.get(field),list) or len(block[field])!=count:
                raise ValueError('incomplete original SEC filing census arrays')
        for field,_ in F.FILING_FIELDS:
            if field in block and (not isinstance(block[field],list) or len(block[field])!=count):
                raise ValueError('incomplete original SEC optional parallel array')
        return document
    check()
    try:
        proved=N.acceptance_proofs(root,plan)
        mains=[row for row in plan['submissions'] if row['source']==0]
        expected_ciks=plan['expected_ciks']
        if (expected_ciks!=sorted(set(expected_ciks)) or
                sorted(row['cik'] for row in mains)!=expected_ciks):
            raise ValueError('complete independently proven SEC issuer census differs')
        history={}
        for row in plan['submissions']:
            if row['source']!=0:
                key=(row['cik'],Path(row['path']).name)
                if key in history:
                    raise ValueError('duplicate original SEC history inventory')
                history[key]=row
        consumed=set()
        for record in mains:
            cik=record['cik']
            if isinstance(cik,bool) or not isinstance(cik,int) or cik<=0:
                raise ValueError('invalid original SEC issuer identity')
            if record['url']!=f'https://data.sec.gov/submissions/CIK{cik:010d}.json':
                raise ValueError('original SEC main URL differs')
            document=original(record)
            owner=document.get('cik')
            if (isinstance(owner,bool) or not (isinstance(owner,int) or
                    isinstance(owner,str) and owner.isascii() and owner.isdecimal()) or int(owner)!=cik):
                raise ValueError('original SEC main owner mismatch')
            references=(document.get('filings') or {}).get('files',[])
            if not isinstance(references,list):
                raise ValueError('invalid full original SEC history references')
            block=F.columnar_block(document,0)
            count=len(block.get('accessionNumber') or [])
            owners={(cik,acc) for acc in block.get('accessionNumber',[])}
            if count!=record['filing_count']:
                raise ValueError('complete original SEC main count differs')
            yield from sec_regime_rows(document,cik=cik,source=0,
                source_sha256=plan['files'][record['path']]['sha256'],
                proved_clocks=proved,owner_accessions=owners)
            del document,block,owners
            for reference in references:
                key=(cik,reference['name'])
                if key in consumed or key not in history:
                    raise ValueError('missing/duplicate full original SEC history reference')
                consumed.add(key)
                member=history[key]
                if member['url']!=f"https://data.sec.gov/submissions/{reference['name']}":
                    raise ValueError('original SEC history URL differs')
                if (isinstance(member['source'],bool) or not isinstance(member['source'],int) or
                        F.member_kind(reference['name'])!=(cik,member['source']) or member['source']<=0):
                    raise ValueError('original SEC history member identity differs')
                document=original(member)
                block=F.columnar_block(document,member['source'])
                count=len(block.get('accessionNumber') or [])
                if count!=reference['filingCount'] or count!=member['filing_count']:
                    raise ValueError('full referenced original SEC history count differs')
                dates=block.get('filingDate') or []
                if dates and (min(dates)!=reference['filingFrom'] or max(dates)!=reference['filingTo']):
                    raise ValueError('full referenced original SEC history range differs')
                owners={(cik,acc) for acc in block.get('accessionNumber',[])}
                yield from sec_regime_rows(document,cik=cik,source=member['source'],
                    source_sha256=plan['files'][member['path']]['sha256'],
                    proved_clocks=proved,owner_accessions=owners)
                del document,block,owners
        if consumed!=set(history):
            raise ValueError('orphan original SEC history source inventory')
    finally:
        try:
            check()
        finally:
            G._guard(con,root)


def prepare_sec_regime(spec, con):
    """Materialize every original regime vintage in the bounded consumer DB.

    The fixed physical reader is exhausted inside a closing scope; no caller
    table or prequalified rows can substitute for it. Missing clocks/reasons and
    duplicates remain visible for subsequent rank-relative qualification.
    """
    con.execute('CREATE OR REPLACE TEMP TABLE index_sec_regime('
        'cik BIGINT,source_accession VARCHAR,form VARCHAR,filer_regime VARCHAR,'
        'filing_date DATE,available_at TIMESTAMPTZ,source_sha256 VARCHAR,'
        'qualified_clock BOOLEAN,reason VARCHAR)')
    batch=[]
    total=0
    with contextlib.closing(iter_sec_regime(spec,con)) as rows:
        for row in rows:
            batch.append(tuple(row[key] for key in ('cik','source_accession','form','filer_regime',
                'filing_date','available_at','source_sha256','qualified_clock'))+(json.dumps(row['reason']),))
            total+=1
            if len(batch)==64:
                con.executemany('INSERT INTO index_sec_regime VALUES (?,?,?,?,?,?,?,?,?)',batch)
                batch.clear()
        if batch:
            con.executemany('INSERT INTO index_sec_regime VALUES (?,?,?,?,?,?,?,?,?)',batch)
    if con.execute('SELECT count(*) FROM index_sec_regime').fetchone()[0]!=total:
        raise ValueError('complete original SEC regime row population differs')
    return {'rows':total,'missing_or_unqualified_clock_rows':con.execute(
        'SELECT count(*) FROM index_sec_regime WHERE NOT qualified_clock').fetchone()[0],
        'source_quality_certified':False}
