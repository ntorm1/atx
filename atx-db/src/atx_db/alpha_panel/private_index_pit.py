"""Task1/P9 v2 pure selection, exit and daily-membership vintages.

Source admission is separate. This kernel accepts already admitted dated facts;
it never turns a static directory flag or a vendor endpoint into a rank fact.
"""
from __future__ import annotations

import bisect
import datetime as dt
from collections import defaultdict

UTC=dt.timezone.utc
RULES=('russell-proxy-v2','sp500-proxy-v2')


def mark(day):
    return dt.datetime.combine(day,dt.time(22),UTC)


def next_session(calendar,day):
    index=bisect.bisect_right(calendar,day)
    if index==len(calendar):
        raise ValueError('independent exchange calendar lacks required next session')
    return calendar[index]


def _calendar(calendar):
    if not calendar or calendar!=sorted(set(calendar)):
        raise ValueError('complete sorted unique independent exchange calendar required')
    return calendar


def _state(value):
    if isinstance(value,dt.date):
        return value,None
    if isinstance(value,tuple) and len(value)==2:
        return value
    return value['last_session'],value.get('delisting_date')


def selection_rows(recons,calendar,schedule,*,planned_calendars=None):
    """Rank rows use only rank members and independent scheduled bounds."""
    _calendar(calendar)
    rows=[]
    identities=set()
    for recon in sorted(recons,key=lambda r:(r['index_id'],r['recon_date'])):
        index=recon['index_id']
        dates=schedule[index]
        if dates!=sorted(set(dates)) or recon['recon_date'] not in dates:
            raise ValueError('independent complete index schedule required')
        position=dates.index(recon['recon_date'])
        if position+1==len(dates):
            raise ValueError('next scheduled reconstitution is missing; no vendor-end fallback')
        rank_calendar=_calendar(planned_calendars[recon['rank_date']]) if planned_calendars is not None else calendar
        start=next_session(rank_calendar,recon['recon_date'])
        end=dates[position+1]
        if recon['rank_date']>recon['recon_date'] or start>end:
            raise ValueError('invalid rank/effective/scheduled bounds')
        total=sum(value[1] for value in recon['members'].values())
        if recon['members'] and total<=0:
            raise ValueError('rank-date total market equity must be positive')
        for sid,(cik,me,rank,basis) in sorted(recon['members'].items()):
            if not isinstance(me,(int,float)) or me<=0:
                raise ValueError('each selected native market equity must be positive')
            key=(index,sid,recon['recon_date'])
            if key in identities:
                raise ValueError('rank selection identity collision')
            identities.add(key)
            rows.append({'index_id':index,'security_id':sid,'cik':cik,
                'effective_from':start,'effective_to':end,'weight':me/total,
                'company_rank':rank,'rank_date':recon['rank_date'],'recon_date':recon['recon_date'],
                'basis':basis,'available_at':mark(recon['rank_date'])})
    return rows


def exit_rows(selections,calendar,line_states,data_end):
    """R3: separate house-clock facts; an alive endpoint never triggers exit."""
    rows=[]
    for selection in selections:
        sid=selection['security_id']
        if sid not in line_states:
            continue
        last,delisting=_state(line_states[sid])
        if last is None or (delisting is None and last>=data_end):
            continue
        if delisting is not None and delisting!=last:
            raise ValueError('native delisting date differs from observed last session')
        # A later cessation belongs to a later selection interval. Pre-effective
        # exits still have rows and never erase the rank-date member selection.
        if last>=selection['effective_to']:
            continue
        stamp=mark(next_session(calendar,last))
        if stamp<=selection['available_at']:
            # Source-qualified selection should not choose a line already known
            # to have left. Do not hide the conflicting population by deduping.
            raise ValueError('rank member had an independently visible prior exit')
        rows.append({'index_id':selection['index_id'],'security_id':sid,
            'recon_date':selection['recon_date'],'last_session':last,
            'available_at':stamp,'exit_basis':'explicit_delisting_date' if delisting is not None else
            'ceased_before_observed_vendor_end'})
    return sorted(rows,key=lambda r:(r['index_id'],r['recon_date'],r['security_id']))


def summary_rows(recons,selections,exits):
    rows=[]
    by_index=defaultdict(list)
    for recon in recons:
        by_index[recon['index_id']].append(recon)
    for index,reconstitutions in sorted(by_index.items()):
        previous=None
        for recon in sorted(reconstitutions,key=lambda r:r['recon_date']):
            stamp=mark(recon['rank_date'])
            prior=set(previous['members']) if previous else set()
            if previous:
                departed={row['security_id'] for row in exits if row['index_id']==index
                    and row['recon_date']==previous['recon_date'] and row['available_at']<stamp}
                prior-=departed
            current=set(recon['members'])
            added,deleted=current-prior,prior-current
            total=sum(value[1] for value in recon['members'].values()) or 1.
            selected=next((row for row in selections if row['index_id']==index and
                           row['recon_date']==recon['recon_date']),None)
            start=selected['effective_from'] if selected else recon['effective_from']
            rows.append({'index_id':index,'rank_date':recon['rank_date'],'recon_date':recon['recon_date'],
                'effective_from':start,'lines':len(current),
                'companies':len({value[0] if value[0] is not None else -sid for sid,value in recon['members'].items()}),
                'additions':len(added) if previous else None,'deletions':len(deleted) if previous else None,
                'turnover_oneway':len(added)/len(current) if previous and current else None,
                'turnover_cap':sum(recon['members'][sid][1] for sid in added)/total if previous else None,
                'p1000_cum_share':recon.get('p1000'),'eligible_companies':recon.get('eligible_companies'),
                'available_at':stamp})
            previous=recon
    return rows


def iter_membership(selections,exits,calendar,data_end):
    """Bounded day-wise R5 replay; no full daily population collected in Python."""
    _calendar(calendar)
    by_index=defaultdict(list)
    for row in selections:
        by_index[row['index_id']].append(row)
    exits_by_key={(row['index_id'],row['security_id'],row['recon_date']):row for row in exits}
    position=bisect.bisect_right(calendar,data_end)
    # The next session's flags depend only on its prior-session close and already
    # public vintages, so it is derivable in the exact as-of fixture as well.
    through=calendar[position] if position<len(calendar) else data_end
    flags={'R1000P':'r1000','R2000P':'r2000','R3000P':'r3000','SP500P':'sp500'}
    # Sweep the calendar once: a row joins the active set on its effective_from session and leaves after its
    # effective_to, so each day visits only the rows in force (cells do not depend on visiting order).
    starts=defaultdict(list)
    for index,rows in by_index.items():
        for row in rows:
            starts[bisect.bisect_left(calendar,row['effective_from'])].append((index,row))
    active=[]
    for offset,day in enumerate(calendar):
        active=[(index,row) for index,row in active+starts.pop(offset,[]) if row['effective_to']>=day]
        if offset==0 or day>through:
            continue
        daily=mark(calendar[offset-1])
        cells={}
        for index,row in active:
            if row['available_at']>daily:
                continue
            stamp=max(daily,row['available_at'])
            departure=exits_by_key.get((index,row['security_id'],row['recon_date']))
            if departure and departure['available_at']<=stamp and day>departure['last_session']:
                continue
            cell=cells.setdefault(row['security_id'],{'session_date':day,'security_id':row['security_id'],
                'cik':None,'r1000':False,'r2000':False,'r3000':False,'sp500':False,
                'available_at':stamp,'_identity_clock':row['available_at']})
            if row['available_at']>cell['_identity_clock']:
                cell['cik']=row['cik'];cell['_identity_clock']=row['available_at']
            elif row['available_at']==cell['_identity_clock']:
                if cell['cik'] is not None and row['cik'] is not None and cell['cik']!=row['cik']:
                    raise ValueError('equal-clock rank company identity collision')
                if row['cik'] is not None:
                    cell['cik']=row['cik']
            cell[flags[index]]=True
            cell['available_at']=max(cell['available_at'],stamp)
        for _,cell in sorted(cells.items()):
            cell.pop('_identity_clock')
            yield cell


def preserve_exits(selections, calendar, exits, prior_exits):
    """Retain independently admitted prior facts when later trading resumes.

    The physical source reader must bind the prior immutable publication. This
    pure projection never authenticates a caller's historical exit assertions.
    An inconsistent replacement is refused, rather than erasing a published
    historical fact or manufacturing a correction clock.
    """
    selection_keys={(r['index_id'],r['security_id'],r['recon_date']):r for r in selections}
    retained={}
    for row in prior_exits:
        key=(row['index_id'],row['security_id'],row['recon_date'])
        selection=selection_keys.get(key)
        if (selection is None or row['exit_basis'] not in
                ('explicit_delisting_date','ceased_before_observed_vendor_end') or
                row['last_session']>=selection['effective_to'] or
                row['available_at']!=mark(next_session(calendar,row['last_session'])) or
                row['available_at']<=selection['available_at'] or key in retained):
            raise ValueError('prior immutable exit population/clock conflicts with rank vintage')
        retained[key]=dict(row)
    for row in exits:
        key=(row['index_id'],row['security_id'],row['recon_date'])
        if key in retained and retained[key]!=row:
            raise ValueError('new line history would replace an immutable prior exit')
        retained[key]=row
    return [retained[key] for key in sorted(retained)]


def derive(recons,calendar,line_states,schedule,*,data_end,prior_exits=(),planned_calendars=None):
    """All non-daily outputs are bounded by original reconstitution populations."""
    selections=selection_rows(recons,calendar,schedule,planned_calendars=planned_calendars)
    exits=exit_rows(selections,calendar,line_states,data_end)
    exits=preserve_exits(selections,calendar,exits,prior_exits)
    # Preserve empty selected populations in summaries and gates explicitly.
    enriched=[{**row,'effective_from':next_session(
        planned_calendars[row['rank_date']] if planned_calendars is not None else calendar,
        row['recon_date'])} for row in recons]
    summaries=summary_rows(enriched,selections,exits)
    return {'constituents':selections,'exits':exits,'recon_summary':summaries}


def receipt(products,recons,eligibility):
    selected=defaultdict(int)
    for row in products['constituents']:
        selected[f"{row['index_id']} {row['recon_date']}"]+=1
    counts={f"{row['index_id']} {row['recon_date']}":len(row['members']) for row in recons}
    selected={key:selected[key] for key in counts}
    if selected!=counts:
        raise ValueError('rank selection population differs from reconstitution counts')
    departed=defaultdict(int)
    for row in products['exits']:
        departed[row['index_id']]+=1
    directory={str(rank):details['directory_decided'] for rank,details in eligibility.items()}
    if any(directory.values()):
        raise ValueError('static directory decided a dated rank')
    backfill={str(rank):details['backfill_cik'] for rank,details in eligibility.items()}
    if any(set(pair)!={'selected_company_keys','excluded_only_is_operating'} or
           any(value!=0 for value in pair.values()) for pair in backfill.values()):
        raise ValueError('backfill identity decided grouping or operating eligibility')
    return {'rank_vintage_rows_on_recon':selected,'counts_on_recon':counts,
            'exit_rows':{index:departed[index] for index in sorted({r['index_id'] for r in recons})},
            'directory_decided_on_rank':directory,'backfill_cik_on_rank':backfill,
            'eligibility_on_rank':eligibility}
