import datetime as dt
import pytest
from atx_db.alpha_panel import private_index_pit as P

D=dt.date


def test_rank_planned_calendar_is_not_replaced_by_later_realized_closure():
    recon={'index_id':'SP500P','rank_date':D(2021,5,28),'recon_date':D(2021,6,18),
           'members':{1:(10,100.,1,'entry')}}
    # Source-shaped known schedule versus separately clocked later closure.
    # Physical document/UTC/complete-update admission belongs to market reader.
    early=[D(2021,6,18),D(2021,6,21),D(2021,6,22)]
    realized=[D(2021,6,18),D(2021,6,22),D(2021,6,23)]
    result=P.derive([recon],realized,{},
        {'SP500P':[D(2021,6,18),D(2021,9,17)]},data_end=D(2021,6,22),
        planned_calendars={D(2021,5,28):early})
    assert result['constituents'][0]['effective_from']==D(2021,6,21)
    assert result['recon_summary'][0]['effective_from']==D(2021,6,21)
    assert result['constituents'][0]['effective_to']==D(2021,9,17)
    daily=list(P.iter_membership(result['constituents'],[],realized,D(2021,6,22)))
    assert daily[0]['session_date']==D(2021,6,22)
    assert daily[0]['available_at']==P.mark(D(2021,6,18))


def test_prior_exit_survives_later_resumption_without_rewriting_vintage():
    recon={'index_id':'R2000P','rank_date':D(2021,5,7),'recon_date':D(2021,6,25),
           'members':{1:(10,100.,1001,'rank')}}
    cal=[D(2021,6,25),D(2021,6,28),D(2021,6,29),D(2021,6,30),D(2021,7,1)]
    schedule={'R2000P':[D(2021,6,25),D(2022,6,24)]}
    prior=P.derive([recon],cal,{1:D(2021,6,28)},schedule,data_end=D(2021,6,29))
    later=P.derive([recon],cal,{1:D(2021,6,30)},schedule,
                   data_end=D(2021,6,30),prior_exits=prior['exits'])
    assert later['constituents']==prior['constituents']
    assert later['exits']==prior['exits']
    assert not [row for row in P.iter_membership(later['constituents'],later['exits'],cal,D(2021,6,30))
                if row['session_date']>=D(2021,6,30)]
    with pytest.raises(ValueError,match='replace an immutable prior exit'):
        P.derive([recon],cal,{1:D(2021,6,29)},schedule,
                 data_end=D(2021,6,30),prior_exits=prior['exits'])


def test_missing_next_schedule_is_explicit_not_vendor_end():
    recon={'index_id':'R2000P','rank_date':D(2021,5,7),'recon_date':D(2021,6,25),
           'members':{1:(10,100.,1001,'rank')}}
    with pytest.raises(ValueError,match='next scheduled'):
        P.derive([recon],[D(2021,6,25),D(2021,6,28)],{},
                 {'R2000P':[D(2021,6,25)]},data_end=D(2021,6,25))


def test_alive_endpoint_never_creates_exit_and_receipt_keeps_zero_denominator():
    recon={'index_id':'R2000P','rank_date':D(2021,5,7),'recon_date':D(2021,6,25),
           'members':{1:(10,100.,1001,'rank')}}
    cal=[D(2021,6,25),D(2021,6,28),D(2021,6,29)]
    result=P.derive([recon],cal,{1:D(2021,6,28)},
                   {'R2000P':[D(2021,6,25),D(2022,6,24)]},data_end=D(2021,6,28))
    assert result['exits']==[]
    assert result['constituents'][0]['effective_to']==D(2022,6,24)
    receipt=P.receipt(result,[recon],{'2021-05-07':{'directory_decided':0,
        'backfill_cik':{'selected_company_keys':0,'excluded_only_is_operating':0}}})
    assert receipt['rank_vintage_rows_on_recon']==receipt['counts_on_recon']
    assert receipt['exit_rows']=={'R2000P':0}
    assert list(P.iter_membership(result['constituents'],[],cal,D(2021,6,28)))[0]['available_at']==P.mark(D(2021,6,25))


def test_rank_phase_keeps_known_selection_before_future_recon():
    from atx_db.alpha_panel import indexes as X
    rank=D(2021,5,7)
    snapshots={rank:[{'security_id':sid,'cik':sid,'me':1e12/sid,'ni_ttm':1.,'ni_q':1.,'adv_63':1e12}
                     for sid in range(1,3002)]}
    recons=X.rank_reconstitutions(snapshots,[('russell',rank,D(2021,6,25))])
    assert {row['index_id']:len(row['members']) for row in recons}=={'R1000P':1000,'R2000P':2000,'R3000P':3000}
    assert X.russell_gate([{'index_id':row['index_id'],'recon_date':row['recon_date'],
                          'companies':len(row['members']),'lines':len(row['members'])} for row in recons])['pass']
    assert X.scheduled_reconstitutions(2026,2027)['R2000P'][:2]==[D(2026,6,26),D(2026,12,11)]
