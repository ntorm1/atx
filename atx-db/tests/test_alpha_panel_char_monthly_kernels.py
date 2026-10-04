import datetime as dt
import math
import pytest
from atx_db.alpha_panel import char_monthly_kernels as K
from atx_db.alpha_panel.char_monthly_registry import REGISTRY,JKP153,BY_NAME


def annual(year=2021,scale=1):
    fields={key:10.0*scale for key in "cogs xsga xrd xint dp pi txt capx oancf dltt dlc che act lct rect invt ivst ivao pstk txditc mib re intan ppent ppegt ao lo icapt sstk prstkc dvc dv emp epspi epsfi cshpri cshfd".split()}
    fields.update(at=100*scale,lt=40*scale,seq=60*scale,sale=80*scale,ni=20*scale,ib=15*scale,oancf=20*scale,oibdp=30*scale,oiadp=25*scale,
                  datadate=dt.date(year,12,31),available_at=dt.datetime(year+1,2,1,tzinfo=dt.UTC),fyear=year,accession=str(year),curcd="USD")
    return fields


def test_full_denominator_and_scalar_registry_executes():
    assert len(JKP153)==153
    assert sum(BY_NAME[n].implementation!="unsupported" for n in JKP153)>=150
    a=[annual(2021-i,1-i*.05) for i in range(6)]
    values,reason=K.yearly_values(a,dt.date(2022,3,31),{"me":200,"me_l1":180,"fiscal_me":190})
    assert reason is None
    assert K.expression(BY_NAME["gp_at"].formula,values)==pytest.approx(.7)
    assert K.expression(BY_NAME["ni_be"].formula,values)==pytest.approx(.25)
    assert K.expression(BY_NAME["be_me"].formula,values)==pytest.approx(.3)
    assert K.expression(BY_NAME["at_gr1"].formula,values)==pytest.approx(1/.95-1)
    for feature in REGISTRY:
        if feature.implementation=="expression":
            assert math.isfinite(K.expression(feature.formula,values)),feature.name


@pytest.mark.parametrize("base",[0,-1,None,float("nan"),float("inf")])
def test_unknown_negative_zero_base_never_zero_fill(base):
    with pytest.raises(K.Missing):K.growth(10,base)


def test_gap_currency_staleness_and_no_current_cap_substitution():
    current=annual();prior=annual(2019)
    values,_=K.yearly_values([current,prior],dt.date(2022,3,31),{"me":200})
    with pytest.raises(K.Missing):K.expression(BY_NAME["at_gr1"].formula,values)
    with pytest.raises(K.Missing):K.special("z_score",values)
    current["curcd"]="EUR"
    assert K.yearly_values([current],dt.date(2022,3,31),{})[1]=="unsupported_accounting_currency"
    current["curcd"]="USD"
    assert K.yearly_values([current],dt.date(2024,3,31),{})[1]=="missing_or_stale_annual"


def test_holdout_product_null_and_full_unchanged_fields():
    values,reasons=K.compute([annual()],[],[],[],dt.date(2023,1,31),{"me":100})
    assert set(values)=={f.name for f in REGISTRY}
    assert values["be_me"]==.6
    for f in REGISTRY:
        if f.return_derived:
            assert values[f.name] is None
            assert reasons[f.name]=="holdout_return_product_prohibited"


def test_quarter_exact_calendar_ytd_and_sue():
    rows=[]
    for i in range(20):
        y=2022-i//4;fq=4-i%4
        month=fq*3;end=dt.date(y,month,1)
        rows.append({"datadate":end,"fyearq":y,"fqtr":fq,"curcd":"USD","saleq":100-i*i,
                     "ibq":50-i*.5,"atq":100,"seqq":60,"txditcq":0,"pstkq":0,"oancfy":fq*20,"capxy":fq*5,"xrdy":fq*2})
    assert K.quarterly("niq_at",rows)==.5
    assert K.quarterly("owner_ocf_q_yoy",rows)==0
    assert K.quarterly("owner_ocf_ttm_yoy",rows)==0
    changes=[rows[i]["saleq"]-rows[i+4]["saleq"] for i in range(1,9)]
    import statistics
    expected=(rows[0]["saleq"]-rows[4]["saleq"]-statistics.mean(changes))/statistics.stdev(changes)
    assert K.quarterly("saleq_su",rows)==pytest.approx(expected)
    broken=rows[:1]+rows[2:]
    assert len(K.quarters(broken,dt.date(2022,12,31)))==1


def test_return_series_regression_momentum_missing_session():
    date=dt.date(2022,12,30)
    bars=[{"session_date":date-dt.timedelta(days=20-i),"ret":.01+i*.001,"ret_exc":.009+i*.001,
           "mkt_rf":.002*i,"price_certified":True,"return_certified":True,"close":10,"volume":100,"shares":1000} for i in range(21)]
    assert K.market("rvol_21d",bars,[],date,{})>0
    assert K.market("ivol_capm_21d",bars,[],date,{})<1e-12
    bars[0]["return_certified"]=False
    with pytest.raises(K.Missing,match="uncertified_total_return"):K.market("rvol_21d",bars,[],date,{})
    monthly=[{"ret":.1,"month_end":date if i==11 else dt.date(2022,i+1,28)} for i in range(12)]
    assert K.market("ret_12_1",bars,monthly,date,{})==pytest.approx(1.1**11-1)
    monthly[0]["ret"]=None
    with pytest.raises(K.Missing):K.market("ret_12_1",bars,monthly,date,{})


def test_accounting_month_states_pit_restatement():
    rows=[annual(y,1+(y-2010)*.03) for y in range(2010,2023)]
    for i,row in enumerate(rows):row["ib"]+=i*i*.2
    date=dt.date(2022,12,31)
    calendar=[]
    for year in range(2016,2023):
        for month in range(1,13):
            end=(dt.date(year+1,1,1) if month==12 else dt.date(year,month+1,1))-dt.timedelta(days=1)
            while end.weekday()>=5:end-=dt.timedelta(days=1)
            calendar.append(end)
    baseline=K.accounting_history("ni_ar1",rows,date,calendar)
    future={**annual(2015,99),"available_at":dt.datetime(2023,2,1,tzinfo=dt.UTC),"accession":"future"}
    assert K.accounting_history("ni_ar1",rows+[future],date,calendar)==baseline
    assert math.isfinite(K.accounting_history("ni_ivol",rows,date,calendar))


def test_cross_section_issuer_deduplication_and_missing_component():
    rows=[]
    for i in range(12):
        values={d:float(i+1) for d in BY_NAME["mispricing_mgmt"].dependencies}
        rows.append({"security_id":i,"cik":i,"univ_all_common":True,"values":values,"reasons":{}})
    duplicate={**rows[0],"security_id":999,"values":dict(rows[0]["values"]),"reasons":{}}
    K.cross_section(rows+[duplicate])
    assert duplicate["values"]["mispricing_mgmt"]==rows[0]["values"]["mispricing_mgmt"]
    assert rows[0]["values"]["mispricing_mgmt"]>rows[-1]["values"]["mispricing_mgmt"]


def test_all_152_supported_original_formulas_produce_qualified_fixture_values():
    date=dt.date(2022,12,30)
    a=[annual(y,1+(y-2010)*.03) for y in range(2010,2023)]
    for i,r in enumerate(a):
        r["ib"]+=i*i*.2;r["oancf"]+=i*i*.1
    q=[]
    for i in range(20):
        y=2022-i//4;fq=4-i%4;month=fq*3
        end=(dt.date(y+1,1,1) if month==12 else dt.date(y,month+1,1))-dt.timedelta(days=1)
        q.append({"datadate":end,"fyearq":y,"fqtr":fq,"curcd":"USD","saleq":1000-i*i,"ibq":50-i*i*.02,
                  "atq":100,"seqq":60,"txditcq":0,"pstkq":0,"oancfy":fq*(20+i*.1)})
    bars=[]
    for i in range(1260):
        m=.01*math.sin(i*.17)+.003*math.cos(i*.37);s=.005*math.cos(i*.23);h=.004*math.sin(i*.33)
        r=1.2*m+.3*s-.2*h+.001*math.cos(i*.71)
        bars.append({"session_date":date-dt.timedelta(days=1259-i),"ret":r+.0001,"ret_exc":r,"mkt_rf":m,
                     "smb":s,"hml":h,"q_size":s,"q_ia":h,"q_roe":.005*math.cos(i*.53),
                     "close":10+i*.01,"close_adjusted":10+i*.01,"high_adjusted":11+i*.01,
                     "volume":100+i%50,"shares":10000,"price_certified":True,"return_certified":True,
                     "corwin_schultz_spread":.01})
    monthly=[];calendar=[]
    for i in range(240):
        n=2003*12+i;y,m=divmod(n,12);month=m+1
        end=(dt.date(y+1,1,1) if month==12 else dt.date(y,month+1,1))-dt.timedelta(days=1)
        while end.weekday()>=5:end-=dt.timedelta(days=1)
        mkt=.05*math.sin(i*.17);smb=.04*math.cos(i*.23);hml=.04*math.sin(i*.33)
        ret=mkt+.3*smb+.2*hml+.001*math.cos(i*.71)
        monthly.append({"month_end":end,"ret":ret,"ret_exc":ret-.0001,"mkt_rf":mkt,"smb":smb,"hml":hml,
                        "shares_adjusted":100+i,"issuer_me":200+i,"dividend_dollars":1})
        calendar.append(end)
    cap={"me":500,"me_l1":450,"fiscal_me":480,"first_observation":dt.date(2000,1,1)}
    values,reasons=K.compute(a,q,bars,monthly,date,cap,annual_vintages=a,month_ends=calendar)
    supported=[n for n in JKP153 if BY_NAME[n].implementation not in ("unsupported","cross_section")]
    assert all(values[n] is not None for n in supported),{n:reasons.get(n) for n in supported if values[n] is None}
    rows=[]
    for i in range(12):
        varied={name:(v*(1+.01*i)+.0001*i*i if v is not None else None) for name,v in values.items()}
        rows.append({"cik":i,"univ_all_common":True,"values":varied,"reasons":dict(reasons)})
    K.cross_section(rows)
    assert sum(rows[0]["values"][n] is not None for n in JKP153)==152
