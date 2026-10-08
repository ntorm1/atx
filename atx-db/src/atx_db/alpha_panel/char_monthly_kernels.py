"""Bounded scalar/one-security monthly characteristic kernels.

Synthetic fixtures may call these directly. Production inputs must first pass
char_monthly_sources admission. No source clock is inferred from fiscal dates.
"""
from __future__ import annotations
import ast
import datetime as dt
import math
import statistics as S
from typing import Any
import numpy as np
from .char_monthly_registry import REGISTRY


class Missing(ValueError):
    pass


def finite(x):
    return x is not None and isinstance(x,(int,float,np.number)) and math.isfinite(float(x))


def ratio(a,b):
    if not finite(a) or not finite(b):
        raise Missing("missing_or_nonfinite_primitive")
    if b <= 0:
        raise Missing("nonpositive_denominator")
    return a/b


def growth(a,b):
    return ratio(a,b)-1


def expression(formula, values):
    """Small arithmetic DSL, no eval, attribute lookup, calls or code injection."""
    def visit(n):
        if isinstance(n,ast.Constant) and isinstance(n.value,(int,float)):
            return n.value
        if isinstance(n,ast.Name):
            x=values.get(n.id)
            if not finite(x):
                raise Missing(f"missing_primitive:{n.id}")
            return float(x)
        if isinstance(n,ast.UnaryOp) and isinstance(n.op,ast.USub):
            return -visit(n.operand)
        if isinstance(n,ast.BinOp):
            a,b=visit(n.left),visit(n.right)
            if isinstance(n.op,ast.Add): return a+b
            if isinstance(n.op,ast.Sub): return a-b
            if isinstance(n.op,ast.Mult): return a*b
            if isinstance(n.op,ast.Div): return ratio(a,b)
        if isinstance(n,ast.Call) and isinstance(n.func,ast.Name):
            args=[visit(a) for a in n.args]
            if n.func.id=="ratio": return ratio(*args)
            if n.func.id=="growth": return growth(*args)
            if n.func.id=="abs": return abs(*args)
            if n.func.id=="avg": return S.mean(args)
            if n.func.id=="log":
                if args[0]<=0: raise Missing("nonpositive_log_argument")
                return math.log(args[0])
        raise ValueError("unsupported registry expression")
    result=visit(ast.parse(formula,mode="eval").body)
    if not finite(result): raise Missing("nonfinite_result")
    return result


def canonical(row):
    """JKP primitives with strictly source-backed alternatives, never plugs."""
    out=dict(row)
    def v(name,*alternatives):
        for value in alternatives:
            if finite(value):
                out[name]=float(value); return
        out[name]=None
    def calc(formula):
        try:return expression(formula,out)
        except Missing:return None
    v("sale",row.get("sale"),row.get("revt"))
    v("cash",row.get("che"));v("inv",row.get("invt"));v("rec",row.get("rect"))
    v("ca",row.get("act"));v("cl",row.get("lct"))
    v("gp",row.get("gp"),calc("sale-cogs"))
    v("opex",row.get("xopr"),calc("cogs+xsga"))
    v("ebitda",row.get("ebitda"),row.get("oibdp"),calc("sale-opex"))
    v("ebit",row.get("ebit"),row.get("oiadp"),calc("ebitda-dp"))
    v("ni",row.get("ib"),calc("ni-xido"))
    v("nix",row.get("ni"));v("tax",row.get("txt"))
    v("seq",row.get("seq"),calc("ceq+pstk"),calc("at-lt"))
    v("be",calc("seq+txditc-pstk"))
    v("debt",calc("dltt+dlc"));v("netdebt",calc("debt-cash"))
    v("bev",calc("icapt+dlc-cash"),calc("seq+netdebt+mib"));v("ocf",row.get("oancf"))
    v("fcf",calc("ocf-capx"));v("op",calc("ebitda+xrd"));v("ope",calc("ebitda-xint"))
    v("coa",calc("ca-cash"));v("col",calc("cl-dlc"));v("cowc",calc("coa-col"))
    v("ncoa",calc("at-ca-ivao"));v("ncol",calc("lt-cl-dltt"))
    v("nncoa",calc("ncoa-ncol"));v("noa",calc("cowc+nncoa"))
    v("sti",row.get("ivst"));v("lti",row.get("ivao"))
    v("fnl",calc("debt+pstk"));v("fna",calc("sti+lti"));v("nfna",calc("fna-fnl"))
    v("oacc",calc("ni-ocf"));v("cop",calc("op-oacc"))
    v("ppeg",row.get("ppegt"));v("ppeinv",calc("ppeg+inv"));v("lnoa",calc("ppent+intan+ao-lo+dp"))
    v("aliq",calc("cash+0.75*coa+0.5*(at-ca-intan)"))
    v("eqbb",row.get("prstkc"));v("eqis",row.get("sstk"));v("eqnetis",calc("eqis-eqbb"))
    v("div",row.get("dvt"),row.get("dv"),calc("dvc+dvp"))
    v("eqpo",calc("div+eqbb"));v("eqnpo",calc("div-eqnetis"))
    v("eps_basic",row.get("epspi"));v("eps_diluted",row.get("epsfi"))
    v("shares_basic",row.get("cshpri"));v("shares_diluted",row.get("cshfd"))
    v("ic",calc("be+debt-cash"));v("capemp",calc("at-cl"))
    return out


def yearly_values(rows, date, cap):
    """Rows already latest visible vintage per period, with exact fiscal years."""
    eligible=[r for r in rows if r["datadate"]<=date and (date-r["datadate"]).days<=550]
    if not eligible:return {},"missing_or_stale_annual"
    current=max(eligible,key=lambda r:r["datadate"])
    currency=current.get("curcd")
    if currency!="USD":return {},"unsupported_accounting_currency"
    values=canonical(current)
    for lag in range(1,6):
        candidates=[r for r in rows if r.get("fyear")==current.get("fyear",-999)-lag
                    and abs((current["datadate"]-r["datadate"]).days-365.25*lag)<=35
                    and r.get("curcd")==currency]
        if len(candidates)==1:
            prior=canonical(candidates[0])
            values.update({f"{k}_l{lag}":v for k,v in prior.items() if finite(v)})
    values.update(cap)
    for name,formula in (("mev","me+netdebt"),("mat_l1","at_l1-be_l1+me_l1"),
                         ("tacc","oacc+nfna-nfna_l1"),("dbnetis","dltt-dltt_l1+dlc-dlc_l1"),
                         ("netis","eqnetis+dbnetis")):
        try:values[name]=expression(formula,values)
        except Missing:values[name]=None
    return values,None


def special(name,v):
    def e(formula):return expression(formula,v)
    if name=="f_score":
        terms=[e("ratio(ni,at_l1)"),e("ratio(ocf,at_l1)"),e("ratio(ni,at_l1)-ratio(ni_l1,at_l2)"),
               e("ocf-ni"),-e("ratio(dltt,at)-ratio(dltt_l1,at_l1)"),
               e("ratio(ca,cl)-ratio(ca_l1,cl_l1)"),-abs(e("eqis")),
               e("ratio(gp,sale)-ratio(gp_l1,sale_l1)"),e("ratio(sale,at_l1)-ratio(sale_l1,at_l2)")]
        return sum(int(x>0) for i,x in enumerate(terms) if i!=6)+int(terms[6]==0)
    if name=="o_score":
        return e("-1.37-0.407*log(ratio(at,1000000))+6.03*ratio(debt,at)+1.43*ratio(ca-cl,at)+0.076*ratio(cl,ca)-2.37*ratio(nix,at)-1.83*ratio(pi+dp,lt)-0.52*ratio(nix-nix_l1,abs(nix)+abs(nix_l1))")-1.72*int(e("lt-at")>0)+0.285*int(e("nix")<0 and e("nix_l1")<0)
    if name=="z_score":return e("1.2*ratio(ca-cl,at)+1.4*ratio(re,at)+3.3*ratio(ebitda,at)+0.6*ratio(fiscal_me,lt)+ratio(sale,at)")
    if name=="kz_index":return e("-1.002*ratio(ni+dp,ppent_l1)+0.283*ratio(at+fiscal_me-be,at)+3.139*ratio(debt,debt+seq)-39.368*ratio(div,ppent_l1)-1.315*ratio(cash,ppent_l1)")
    if name=="ival_me":
        roe=e("ratio(nix,avg(be,be_l1))")
        payout=e("ratio(div,nix)") if e("nix")>0 else e("ratio(div,at*0.06)")
        intrinsic=e("be")+(roe-.12)/1.12*e("be")+(roe-.12)/(1.12*.12)*e("be")*(1+(1-payout)*roe)
        if intrinsic<=0:raise Missing("nonpositive_intrinsic_value")
        return ratio(intrinsic,e("me"))
    raise ValueError(f"unknown special kernel:{name}")


def quarters(rows,date):
    rows=sorted((r for r in rows if r["datadate"]<=date),key=lambda r:r["datadate"],reverse=True)
    if not rows or (date-rows[0]["datadate"]).days>400: return []
    out=[]
    for r in rows:
        if (r.get("curcd")!="USD" or not finite(r.get("fyearq"))
                or r.get("fqtr") not in (1,2,3,4)):break
        if out and (not 60<=(out[-1]["datadate"]-r["datadate"]).days<=120
                    or ((int(out[-1]["fyearq"])*4+int(out[-1]["fqtr"]))-(int(r["fyearq"])*4+int(r["fqtr"])))!=1):break
        out.append(r)
    return out


def quarterly(name,q):
    def get(i,key):
        if i>=len(q):raise Missing("insufficient_consecutive_quarter_history")
        x=q[i].get(key)
        if not finite(x):raise Missing(f"missing_quarter_primitive:{key}")
        return float(x)
    def be(i):return get(i,"seqq")+get(i,"txditcq")-get(i,"pstkq")
    def profitability(i,kind):return ratio(get(i,"ibq"),get(i+1,"atq") if kind=="at" else be(i+1))
    if name.startswith("owner_"):
        frequency="ttm_yoy" if name.endswith("ttm_yoy") else "q_yoy"
        primitive=name[len("owner_"):-len(frequency)-1]
        def standalone(i,key):
            value=get(i,key)
            if q[i]["fqtr"]!=1:
                if i+1>=len(q) or q[i+1]["fyearq"]!=q[i]["fyearq"]:raise Missing("missing_prior_ytd")
                value-=get(i+1,key)
            return value
        def value(i):
            if primitive=="be":return be(i)
            if primitive=="ocf":return standalone(i,"oancfy")
            if primitive=="capx":return standalone(i,"capxy")
            if primitive=="xrd":return standalone(i,"xrdy")
            if primitive=="fcf":return standalone(i,"oancfy")-standalone(i,"capxy")
            return get(i,{"sale":"saleq","ni":"ibq","at":"atq","eps_basic":"epspiq","eps_diluted":"epsfiq","shares_basic":"cshpriq","shares_diluted":"cshfdq"}[primitive])
        if frequency=="q_yoy":return growth(value(0),value(4))
        if primitive in ("eps_basic","eps_diluted","shares_basic","shares_diluted"):
            raise Missing("TTM_per_share_weighted_earnings_and_shares_not_admitted")
        if primitive in ("at","be"):return growth(value(0),value(4))
        return growth(sum(value(i) for i in range(4)),sum(value(i) for i in range(4,8)))
    if name in ("niq_at","niq_be"):return profitability(0,name[-2:])
    if name in ("niq_at_chg1","niq_be_chg1"):return profitability(0,name[4:6])-profitability(4,name[4:6])
    if name=="saleq_gr1":return growth(get(0,"saleq"),get(4,"saleq"))
    if name in ("niq_su","saleq_su"):
        key="ibq" if name=="niq_su" else "saleq"
        changes=[get(i,key)-get(i+4,key) for i in range(1,9)]
        surprise=get(0,key)-get(4,key)-S.mean(changes)
        return ratio(surprise,S.stdev(changes))
    if name=="ni_inc8q":
        changes=[get(i,"ibq")-get(i+4,"ibq") for i in range(8)]
        return next((i for i,x in enumerate(changes) if x<=0),8)
    if name=="ocfq_saleq_std":
        vals=[]
        for i in range(16):
            ocf=get(i,"oancfy")
            if q[i]["fqtr"]!=1:
                if i+1>=len(q) or q[i+1]["fyearq"]!=q[i]["fyearq"]:raise Missing("missing_prior_ytd")
                ocf-=get(i+1,"oancfy")
            vals.append(ratio(ocf,get(i,"saleq")))
        return S.stdev(vals)
    raise ValueError(name)


def std(x):
    if len(x)<2:raise Missing("insufficient_history")
    return S.stdev(x)


def skew(x):
    n=len(x);s=std(x)
    if n<3 or s<=0:raise Missing("zero_variance")
    mean=S.mean(x)
    return sum(((v-mean)/s)**3 for v in x)/n


def regression(y,x,min_n):
    if len(y)<min_n:raise Missing("insufficient_regression_history")
    a=np.column_stack([np.ones(len(y)),np.asarray(x,dtype=float)])
    if np.linalg.matrix_rank(a)!=a.shape[1]:raise Missing("rank_deficient_regression")
    beta=np.linalg.lstsq(a,np.array(y),rcond=None)[0]
    return beta,np.array(y)-a@beta


def compound(values):
    if len(values)==0 or any(not finite(x) or x< -1 for x in values):raise Missing("incomplete_total_return_history")
    return math.prod(1+x for x in values)-1


def market(name,bars,monthly,date,cap):
    """bars exact trailing calendar grid, ascending, missing rows preserved."""
    if name=="market_equity":return ratio(cap.get("me"),1)
    if name=="age":
        first=cap.get("first_observation")
        if first is None:raise Missing("missing_certified_first_observation")
        if first>date:raise Missing("future_first_observation")
        return (date.year-first.year)*12+date.month-first.month
    def get(rows,key):
        if not rows or any(not finite(r.get(key)) for r in rows):raise Missing(f"missing_market_primitive:{key}")
        return [float(r[key]) for r in rows]
    if not bars or bars[-1]["session_date"]!=date:raise Missing("missing_month_end_price")
    if name=="prc":
        if bars[-1].get("price_certified") is not True:raise Missing("uncertified_price_basis")
        return get(bars[-1:],"close")[0]
    if name in ("div12m_me","chcsho_12m","eqnpo_12m","ceq_iss_5y","beta_60m") or name.startswith(("ret_","seas_","resff3_")):
        if not monthly or monthly[-1].get("month_end")!=date:raise Missing("missing_current_month_market_observation")
    if name=="div12m_me":return ratio(sum(get(monthly[-12:],"dividend_dollars")),cap.get("me")) if len(monthly)>=12 else (_ for _ in ()).throw(Missing("insufficient_monthly_history"))
    if name=="chcsho_12m":
        if len(monthly)<13:raise Missing("insufficient_monthly_history")
        return growth(get(monthly[-1:],"shares_adjusted")[0],get(monthly[-13:-12],"shares_adjusted")[0])
    if name=="eqnpo_12m":
        if len(monthly)<13:raise Missing("insufficient_monthly_history")
        return 1-ratio(growth(cap.get("me"),get(monthly[-13:-12],"issuer_me")[0])+1,compound(get(monthly[-12:],"ret"))+1)
    if name=="ceq_iss_5y":
        if len(monthly)<61:raise Missing("insufficient_monthly_history")
        size=ratio(cap.get("me"),get(monthly[-61:-60],"issuer_me")[0])
        gross=compound(get(monthly[-60:],"ret"))+1
        if gross<=0:raise Missing("nonpositive_cumulative_gross_return")
        return math.log(size)-math.log(gross)
    if name.startswith("ret_"):
        a,b=map(int,name.split("_")[1:])
        if len(monthly)<a:raise Missing("insufficient_monthly_history")
        return compound(get(monthly[-a:len(monthly)-b if b else None],"ret"))
    if name.startswith("seas_"):
        _,a,bkind=name.split("_");a=int(a);b=int(bkind[:-2]);annual=bkind[-2:]=="an"
        lags=[i for i in range(12*(a-1)+1,12*b+1) if (i%12==0)==annual]
        if len(monthly)<max(lags)+1:raise Missing("insufficient_monthly_history")
        return S.mean(get([monthly[-i-1] for i in lags],"ret"))
    if name.startswith("resff3_"):
        a=int(name.split("_")[1]);window=monthly[-36:]
        y=get(window,"ret_exc");x=list(zip(get(window,"mkt_rf"),get(window,"smb"),get(window,"hml")))
        _,res=regression(y,x,24)
        if len(res)<a+1:raise Missing("insufficient_monthly_history")
        return ratio(compound(res[-a-1:-1]),std(res))
    if name=="beta_60m":
        window=monthly[-60:]
        beta,_=regression(get(window,"ret_exc"),get(window,"mkt_rf"),36)
        return float(beta[1])
    n=1260 if name in ("corr_1260d","betabab_1260d") else int(name.rsplit("_",1)[-1][:-1])
    window=bars[-n:]
    minimum={21:15,126:60,252:120,1260:750}[n]
    if len(window)<minimum:raise Missing("insufficient_session_history")
    # An absent row is not a nontrading zero. All used cells must be independently
    # certified. Registry minimum observations does not authorize silent dropping.
    if any(r.get("price_certified") is not True for r in window):raise Missing("uncertified_price_basis")
    if name=="prc_highprc_252d":return ratio(get(window[-1:],"close_adjusted")[0],max(get(window,"high_adjusted")))
    if name=="bidaskhl_21d":
        # Published precomputed per-day source-backed estimator, independently
        # pinned; native code does not fabricate overnight/split adjustments.
        return S.mean(get(window,"corwin_schultz_spread"))
    if name.startswith(("dolvol","turnover","zero_trades")):
        volumes=get(window,"volume");close=get(window,"close");shares=get(window,"shares")
        vals=[v*p for v,p in zip(volumes,close)] if name.startswith("dolvol") else [ratio(v,s) for v,s in zip(volumes,shares)]
        if name.startswith("zero_trades"):return (sum(v==0 for v in volumes)+ratio(1,11000*sum(vals)))*21/n
        return ratio(std(vals),S.mean(vals)) if "_var_" in name else S.mean(vals)
    if any(r.get("return_certified") is not True for r in window):raise Missing("uncertified_total_return_or_delisting_basis")
    raw=get(window,"ret");y=get(window,"ret_exc")
    if name=="ami_126d":return S.mean([ratio(abs(r),p*v) for r,p,v in zip(raw,get(window,"close"),get(window,"volume"))])*1e6
    if name=="rvol_21d":return std(y)
    if name=="rskew_21d":return skew(y)
    if name=="rmax1_21d":return max(raw)
    if name=="rmax5_21d":return S.mean(sorted(raw,reverse=True)[:5])
    if name=="rmax5_rvol_21d":
        denominator=bars[-252:]
        if len(denominator)<120:raise Missing("insufficient_denominator_session_history")
        if any(r.get("price_certified") is not True or r.get("return_certified") is not True for r in denominator):
            raise Missing("uncertified_denominator_total_return_or_price_basis")
        return ratio(S.mean(sorted(raw,reverse=True)[:5]),std(get(denominator,"ret_exc")))
    m=get(window,"mkt_rf")
    if name in ("corr_1260d","betabab_1260d"):
        y3=[sum(y[i-2:i+1]) for i in range(2,len(y))];m3=[sum(m[i-2:i+1]) for i in range(2,len(m))]
        if std(y3)<=0 or std(m3)<=0:raise Missing("zero_variance")
        correlation=float(np.corrcoef(y3,m3)[0,1])
        return correlation if name=="corr_1260d" else correlation*ratio(std(get(bars[-252:],"ret_exc")),std(get(bars[-252:],"mkt_rf")))
    if name=="betadown_252d":
        mask=[i for i,x in enumerate(m) if x<0]
        beta,_=regression([y[i] for i in mask],[m[i] for i in mask],3);return float(beta[1])
    if name=="beta_dimson_21d":
        beta,_=regression(y[1:-1],list(zip(m[:-2],m[1:-1],m[2:])),15);return float(sum(beta[1:]))
    model="ff3" if "ff3" in name else "hxz4" if "hxz4" in name else "capm"
    x=m if model=="capm" else list(zip(m,get(window,"smb"),get(window,"hml"))) if model=="ff3" else list(zip(m,get(window,"q_size"),get(window,"q_ia"),get(window,"q_roe")))
    _,res=regression(y,x,minimum)
    if name=="coskew_21d":
        dm=np.array(m)-S.mean(m)
        return ratio(float(np.mean(res*dm**2)),math.sqrt(float(np.mean(res**2)))*float(np.mean(dm**2)))
    return skew(res) if name.startswith("iskew") else std(res)


def accounting_history(name,vintages,date,month_ends=None):
    """Rebuild historical states at their own clocks, including restatements."""
    states=[]
    calendar=sorted(d for d in (month_ends or []) if d<=date)[-72:]
    if len(calendar)!=72:raise Missing("insufficient_pinned_month_end_calendar")
    for older,newer in zip(calendar,calendar[1:]):
        if (newer.year-older.year)*12+newer.month-older.month!=1:raise Missing("gapped_month_end_calendar")
    for last in calendar:
        cutoff=dt.datetime.combine(last,dt.time(22),dt.UTC)
        visible={}
        for row in vintages:
            if row["available_at"]<cutoff and row["datadate"]<=last:
                old=visible.get(row["datadate"])
                if old is None or (row["available_at"],row["accession"])>(old["available_at"],old["accession"]):visible[row["datadate"]]=row
        values,reason=yearly_values(list(visible.values()),last,{})
        if reason:raise Missing("insufficient_monthly_accounting_history")
        states.append(values)
    if name=="earnings_variability":
        a=[expression("ratio(ni,at_l1)",v) for v in states[-60:]]
        b=[expression("ratio(ocf,at_l1)",v) for v in states[-60:]]
        return ratio(std(a),std(b))
    y=[expression("ratio(ni,at)",v) for v in states[-60:]]
    x=[expression("ratio(ni,at)",v) for v in states[:60]]
    beta,res=regression(y,x,60)
    return float(beta[1]) if name=="ni_ar1" else math.sqrt(float(np.sum(res**2))/(len(res)-1))


def compute(annual,quarter,bars,monthly,date,cap,sector=None,annual_vintages=None,month_ends=None):
    v,annual_reason=yearly_values(annual,date,cap)
    q=quarters(quarter,date)
    result,reasons={},{}
    for f in REGISTRY:
        try:
            if f.implementation=="unsupported":raise Missing(f.unsupported_reason)
            if f.implementation=="cross_section":raise Missing("pending_same_date_cross_section")
            if f.implementation in ("expression","special","quarterly") and sector in ("bank","insurer") and any(d in f.dependencies for d in ("gp","cogs","inv","ca","cl","cowc","ebit","ebitda","op","ope","cop")):
                raise Missing("sector_structurally_inapplicable")
            if f.implementation=="expression":
                if annual_reason:raise Missing(annual_reason)
                value=expression(f.formula,v)
            elif f.implementation=="special":value=special(f.name,v)
            elif f.implementation=="quarterly":value=quarterly(f.name,q)
            elif f.implementation=="accounting_history":value=accounting_history(f.name,annual_vintages or annual,date,month_ends)
            elif f.implementation=="market":value=market(f.name,bars,monthly,date,cap)
            else:raise ValueError(f.implementation)
            if not finite(value):raise Missing("nonfinite_result")
            result[f.name]=float(value)
        except Missing as ex:
            result[f.name]=None;reasons[f.name]=str(ex)
    return result,reasons


def cross_section(rows):
    """Same-month distinct issuer statistics; duplicates never overweight ranks."""
    if len({r["cik"] for r in rows if r.get("cik") is not None})<10:return
    issuer={}
    for row in rows:
        cik=row.get("cik")
        if cik is not None and row.get("univ_all_common") is True:
            issuer.setdefault(cik,row)
    if len(issuer)<10:return
    for f in REGISTRY:
        if f.implementation!="cross_section":continue
        pool=[r for r in issuer.values() if all(finite(r["values"].get(d)) for d in f.dependencies)]
        if len(pool)<10:continue
        components=[]
        for d in f.dependencies:
            from .char_monthly_registry import BY_NAME
            sign=BY_NAME[d].economic_sign
            vals=np.array([r["values"][d]*sign for r in pool])
            if f.name.startswith("mispricing"):
                ranked=np.array([(sum(vals<v)+(sum(vals==v)+1)/2-1)/(len(vals)-1) for v in vals])
            else:
                sigma=std(vals)
                if sigma<=0:break
                ranked=(vals-np.mean(vals))/sigma
            components.append(ranked)
        if len(components)!=len(f.dependencies):continue
        scores=np.mean(components,axis=0)
        if f.name.startswith("qmj_"):
            sigma=std(scores)
            if sigma<=0:continue
            scores=(scores-np.mean(scores))/sigma
        mapping={r["cik"]:float(score) for r,score in zip(pool,scores)}
        for row in rows:
            if row.get("cik") in mapping:
                row["values"][f.name]=mapping[row["cik"]];row["reasons"].pop(f.name,None)
