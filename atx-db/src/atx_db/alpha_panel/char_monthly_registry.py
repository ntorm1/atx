"""Private frozen monthly registry; natural values, economic signs kept separately.

Definitions follow JKP Global Factor Data Documentation sections 6--8. Missing
inputs are never filled, including optional JKP zero fallbacks: this stricter
source-quality policy is an explicit coverage limitation, not a different value.
No warehouse IO, return comparison or candidate selection happens on import.
"""
from __future__ import annotations

import ast
from dataclasses import asdict, dataclass, replace
from .release_inputs import digest

REFERENCE = "https://jkpfactors.s3.amazonaws.com/documents/Documentation.pdf"
CATALOG_REFERENCE = "https://raw.githubusercontent.com/bkelly-lab/jkp-data/main/src/jkp/data/config.py"
PRIMITIVES={
    "sale":"funda.sale or source-backed funda.revt", "gp":"funda.gp or sale-funda.cogs",
    "opex":"funda.xopr or funda.cogs+funda.xsga", "ebitda":"funda.ebitda or funda.oibdp or sale-opex",
    "ebit":"funda.ebit or funda.oiadp or ebitda-funda.dp", "ni":"funda.ib or funda.ni-funda.xido",
    "nix":"funda.ni", "tax":"funda.txt", "cash":"funda.che", "inv":"funda.invt", "rec":"funda.rect",
    "ca":"funda.act", "cl":"funda.lct", "seq":"funda.seq or funda.ceq+funda.pstk or funda.at-funda.lt",
    "be":"seq+funda.txditc-funda.pstk", "debt":"funda.dltt+funda.dlc", "netdebt":"debt-cash",
    "bev":"funda.icapt+funda.dlc-cash or seq+netdebt+funda.mib", "ocf":"funda.oancf", "fcf":"ocf-funda.capx",
    "op":"ebitda+funda.xrd", "ope":"ebitda-funda.xint", "coa":"ca-cash", "col":"cl-funda.dlc",
    "cowc":"coa-col", "ncoa":"funda.at-ca-funda.ivao", "ncol":"funda.lt-cl-funda.dltt",
    "nncoa":"ncoa-ncol", "noa":"cowc+nncoa", "sti":"funda.ivst", "lti":"funda.ivao",
    "fnl":"debt+funda.pstk", "fna":"sti+lti", "nfna":"fna-fnl", "oacc":"ni-ocf", "cop":"op-oacc",
    "ppeg":"funda.ppegt", "ppeinv":"ppeg+inv", "lnoa":"funda.ppent+funda.intan+funda.ao-funda.lo+funda.dp",
    "aliq":"cash+.75*coa+.5*(funda.at-ca-funda.intan)", "eqbb":"funda.prstkc", "eqis":"funda.sstk",
    "eqnetis":"eqis-eqbb", "div":"funda.dvt or funda.dv or funda.dvc+funda.dvp, all supplied components certified",
    "eqpo":"div+eqbb", "eqnpo":"div-eqnetis", "eps_basic":"funda.epspi", "eps_diluted":"funda.epsfi",
    "shares_basic":"funda.cshpri", "shares_diluted":"funda.cshfd", "ic":"be+debt-cash", "capemp":"funda.at-cl",
    "me":"sum all qualified eligible issuer classes; every constituent positive finite USD with independent dated membership/cap proof",
    "fiscal_me":"same cap rule at last pinned session <= fiscal period end, never current cap",
    "me_l1":"same cap rule at last pinned session <= same calendar date previous year",
    "mev":"me+netdebt", "mat_l1":"at_l1-be_l1+me_l1", "tacc":"oacc+nfna-nfna_l1",
    "dbnetis":"funda.dltt-dltt_l1+funda.dlc-dlc_l1 (source-backed balance-change fallback)", "netis":"eqnetis+dbnetis"}
JKP153 = tuple("""age aliq_at aliq_mat ami_126d at_be at_gr1 at_me at_turnover be_gr1a be_me beta_60m beta_dimson_21d betabab_1260d betadown_252d bev_mev bidaskhl_21d capex_abn capx_gr1 capx_gr2 capx_gr3 cash_at chcsho_12m coa_gr1a col_gr1a cop_at cop_atl1 corr_1260d coskew_21d cowc_gr1a dbnetis_at debt_gr3 debt_me dgp_dsale div12m_me dolvol_126d dolvol_var_126d dsale_dinv dsale_drec dsale_dsga earnings_variability ebit_bev ebit_sale ebitda_mev emp_gr1 eq_dur eqnetis_at eqnpo_12m eqnpo_me eqpo_me f_score fcf_me fnl_gr1a gp_at gp_atl1 ival_me inv_gr1 inv_gr1a iskew_capm_21d iskew_ff3_21d iskew_hxz4_21d ivol_capm_21d ivol_capm_252d ivol_ff3_21d ivol_hxz4_21d kz_index lnoa_gr1a lti_gr1a market_equity mispricing_mgmt mispricing_perf ncoa_gr1a ncol_gr1a netdebt_me netis_at nfna_gr1a ni_ar1 ni_be ni_inc8q ni_ivol ni_me niq_at niq_at_chg1 niq_be niq_be_chg1 niq_su nncoa_gr1a noa_at noa_gr1a o_score oaccruals_at oaccruals_ni ocf_at ocf_at_chg1 ocf_me ocfq_saleq_std op_at op_atl1 ope_be ope_bel1 opex_at pi_nix ppeinv_gr1a prc prc_highprc_252d qmj qmj_growth qmj_prof qmj_safety rd_me rd_sale rd5_at resff3_12_1 resff3_6_1 ret_1_0 ret_12_1 ret_12_7 ret_3_1 ret_6_1 ret_60_12 ret_9_1 rmax1_21d rmax5_21d rmax5_rvol_21d rskew_21d rvol_21d sale_bev sale_emp_gr1 sale_gr1 sale_gr3 sale_me saleq_gr1 saleq_su seas_1_1an seas_1_1na seas_11_15an seas_11_15na seas_16_20an seas_16_20na seas_2_5an seas_2_5na seas_6_10an seas_6_10na sti_gr1a taccruals_at taccruals_ni tangibility tax_gr1a turnover_126d turnover_var_126d z_score zero_trades_126d zero_trades_21d zero_trades_252d""".split())


@dataclass(frozen=True)
class Feature:
    name: str
    family: str
    formula: str
    dependencies: tuple[str, ...]
    economic_sign: int
    history: str
    unit: str = "dimensionless"
    return_derived: bool = False
    implementation: str = "expression"
    unsupported_reason: str | None = None
    source_clock: str = "all primitive vintages strictly before month-end session 22:00 UTC"
    staleness: str = "annual 550 days; quarterly 400 days; prices exact month-end session"
    missingness: str = "unknown primitive/history/currency/quality -> NULL with keyed reason; never zero-fill"
    citation: str = REFERENCE

    def record(self):
        quarterly={"sale":"fundq.saleq", "ni":"fundq.ibq", "at":"fundq.atq",
                   "be":"fundq.seqq+fundq.txditcq-fundq.pstkq",
                   "ocf":"fundq.oancfy minus preceding same-fiscal-year oancfy unless fqtr=1",
                   "capx":"fundq.capxy minus preceding same-fiscal-year capxy unless fqtr=1",
                   "xrd":"fundq.xrdy minus preceding same-fiscal-year xrdy unless fqtr=1",
                   "fcf":"standalone fundq.oancfy minus standalone fundq.capxy, both YTD predecessors required",
                   "eps_basic":"fundq.epspiq", "eps_diluted":"fundq.epsfiq",
                   "shares_basic":"fundq.cshpriq", "shares_diluted":"fundq.cshfdq"}
        return {**asdict(self), "jkp_name": self.name if self.name in JKP153 or self.family != "owner" else None,
                "original153": self.name in JKP153, "stored_sign": "natural; economic_sign unapplied",
                "coverage_class":"price" if self.implementation=="market" else "accounting",
                "source":"explicit final compustat_std_v11 vintages / qualified pinned price, cap, factor, event products",
                "primitive_dependencies":{d:(quarterly.get(d,f"qualified fundq.{d}; exact fiscal/currency/available_at vintage") if self.implementation=="quarterly" or self.name.endswith(("_q_yoy","_ttm_yoy")) else PRIMITIVES.get(d.rsplit('_l',1)[0],f"source-backed standardized item {d}")) for d in self.dependencies},
                "sector_applicability": "generic operating/working-capital metrics structurally NULL for banks/insurers; REIT NI metrics retained but not FFO proxies"}


def _registry():
    rows: dict[str, Feature] = {}
    def add(name, formula, family="accounting", sign=1, history="current visible fiscal-year snapshot", **kw):
        dependencies = tuple(sorted({n.id for n in ast.walk(ast.parse(formula, mode="eval")) if isinstance(n, ast.Name)} - {"ratio", "growth", "abs", "log", "avg", "indicator", "exp"}))
        rows[name] = Feature(name, family, formula, dependencies, sign, history, **kw)
    # All inputs are canonical USD absolute amounts; ratios do not use Compustat's
    # million-dollar convention. The adapter explicitly normalizes units.
    for name, num, den, sign in (
        ("be_me","be","me",1),("at_me","at","me",1),("ni_me","ni","me",1),
        ("sale_me","sale","me",1),("ocf_me","ocf","me",1),("fcf_me","fcf","me",1),
        ("rd_me","xrd","me",1),("debt_me","debt","me",-1),("netdebt_me","netdebt","me",-1),
        ("bev_mev","bev","mev",1),("ebitda_mev","ebitda","mev",1),
        ("eqnpo_me","eqnpo","me",1),("eqpo_me","eqpo","me",1),
        ("gp_at","gp","at",1),("gp_atl1","gp","at_l1",1),("ni_be","ni","be",1),
        ("ope_be","ope","be",1),("ope_bel1","ope","be_l1",1),
        ("op_at","op","at",1),("op_atl1","op","at_l1",1),
        ("cop_at","cop","at",1),("cop_atl1","cop","at_l1",1),
        ("ocf_at","ocf","at",1),("opex_at","opex","at",1),("cash_at","cash","at",1),
        ("noa_at","noa","at",-1),("oaccruals_at","oacc","at",-1),
        ("oaccruals_ni","oacc","abs(nix)",-1),("taccruals_at","tacc","at",-1),
        ("taccruals_ni","tacc","abs(nix)",-1),("at_be","at","be",-1),
        ("at_turnover","sale","at",1),("ebit_bev","ebit","bev",1),
        ("ebit_sale","ebit","sale",1),("sale_bev","sale","bev",1),
        ("rd_sale","xrd","sale",1),("pi_nix","pi","nix",1),
        ("dbnetis_at","dbnetis","at",-1),("eqnetis_at","eqnetis","at",-1),
        ("netis_at","netis","at",-1),("aliq_at","aliq","at_l1",-1),
        ("aliq_mat","aliq","mat_l1",-1)):
        add(name, f"ratio({num},{den})", sign=sign)
    for name, primitive, lag in (("at_gr1","at",1),("sale_gr1","sale",1),("sale_gr3","sale",3),
                                 ("capx_gr1","capx",1),("capx_gr2","capx",2),("capx_gr3","capx",3),
                                 ("inv_gr1","inv",1),("debt_gr3","debt",3)):
        add(name, f"growth({primitive},{primitive}_l{lag})", sign=-1, history=f"exact {lag}-fiscal-year lag; positive prior base")
    for primitive in ("be","coa","col","cowc","fnl","ncoa","ncol","nfna","nncoa","noa","sti","lti","inv","tax"):
        add(f"{primitive}_gr1a", f"ratio({primitive}-{primitive}_l1,at)", sign=-1, history="exact fiscal-year lag")
    add("ppeinv_gr1a", "ratio(ppeinv-ppeinv_l1,at_l1)", sign=-1, history="exact fiscal-year lag")
    add("lnoa_gr1a", "ratio(lnoa-lnoa_l1,avg(at,at_l1))", sign=-1, history="exact fiscal-year lag; average assets (documentation table's subtraction is a known typographic inconsistency)")
    add("emp_gr1","ratio(emp-emp_l1,avg(emp,emp_l1))",sign=-1,history="exact fiscal-year lag")
    add("sale_emp_gr1","growth(ratio(sale,emp),ratio(sale_l1,emp_l1))",sign=1,history="exact fiscal-year lag")
    add("tangibility","ratio(cash+0.715*rec+0.547*inv+0.535*ppeg,at)")
    add("capex_abn","growth(ratio(capx,sale),avg(ratio(capx_l1,sale_l1),ratio(capx_l2,sale_l2),ratio(capx_l3,sale_l3)))",sign=-1,history="3 complete fiscal-year lags")
    add("rd5_at","ratio(xrd+0.8*xrd_l1+0.6*xrd_l2+0.4*xrd_l3+0.2*xrd_l4,at)",history="5 consecutive fiscal years")
    for name,a,b in (("dsale_dinv","sale","inv"),("dsale_drec","sale","rec"),("dsale_dsga","sale","xsga"),("dgp_dsale","gp","sale")):
        add(name, f"growth({a},avg({a}_l1,{a}_l2))-growth({b},avg({b}_l1,{b}_l2))", history="current vs mean of 2 prior fiscal years")
    add("ocf_at_chg1","ratio(ocf,at)-ratio(ocf_l1,at_l1)",history="exact fiscal-year lag")
    for name, formula, deps, history, sign in (
        ("f_score","9 indicator sum; NI/lagAT, OCF/lagAT, changeROA, OCF-NI, debt ratio, current ratio, zero equity issuance, gross margin, asset turnover",("ni","ocf","at","at_l1","at_l2","ni_l1","dltt","dltt_l1","ca","cl","ca_l1","cl_l1","eqis","gp","gp_l1","sale","sale_l1"),"3 fiscal years",1),
        ("o_score","-1.37-.407*log(AT/1e6)+6.03*debt/AT+1.43*(CA-CL)/AT+.076*CL/CA-1.72*I(LT>AT)-2.37*NIX/AT-1.83*(PI+DP)/LT+.285*I(NIX<0 and lagNIX<0)-.52*(NIX-lagNIX)/(|NIX|+|lagNIX|)",("at","debt","ca","cl","lt","nix","nix_l1","pi","dp"),"2 fiscal years; corrected Ohlson size log",-1),
        ("z_score","1.2*(CA-CL)/AT+1.4*RE/AT+3.3*EBITDA/AT+.6*fiscalME/LT+SALE/AT",("ca","cl","at","re","ebitda","fiscal_me","lt","sale"),"current fiscal-year cap certified at fiscal date",1),
        ("kz_index","-1.002*(NI+DP)/lagPPENT+.283*(AT+fiscalME-BE)/AT+3.139*debt/(debt+SEQ)-39.368*DIV/lagPPENT-1.315*cash/lagPPENT",("ni","dp","ppent_l1","at","fiscal_me","be","debt","seq","div","cash"),"2 fiscal years and fiscal cap",-1),
        ("ival_me","(BE+(ROE-.12)/1.12*BE+(ROE-.12)/(1.12*.12)*forecastBE)/ME; ROE=NIX/averageBE, forecastBE=BE*(1+(1-payout)*ROE), payout=DIV/NIX or DIV/(AT*.06)",("be","be_l1","nix","div","at","me"),"2 fiscal years; discount .12 frozen",1),
    ):
        rows[name]=Feature(name,"accounting",formula,deps,sign,history,implementation="special")
    # Quarterly kernels use true standalone quarters, never YTD-as-quarter.
    for name, definition, deps, history in (
        ("niq_at","latest IBQ / previous-quarter AT",("ibq","atq"),"2 consecutive quarters"),
        ("niq_be","latest IBQ / previous-quarter BE",("ibq","seqq","txditcq","pstkq"),"2 consecutive quarters"),
        ("niq_at_chg1","NIQ/previousAT minus same fiscal quarter prior year",("ibq","atq"),"6 consecutive quarters"),
        ("niq_be_chg1","NIQ/previousBE minus same fiscal quarter prior year",("ibq","seqq","txditcq","pstkq"),"6 consecutive quarters"),
        ("saleq_gr1","SALEQ / year-ago SALEQ - 1",("saleq",),"5 consecutive quarters"),
        ("niq_su","(IBQ-yearagoIBQ-mean previous8 seasonal differences) / sample std previous8 differences",("ibq",),"13 consecutive quarters"),
        ("saleq_su","(SALEQ-yearagoSALEQ-mean previous8 seasonal differences) / sample std previous8 differences",("saleq",),"13 consecutive quarters"),
        ("ni_inc8q","consecutive positive year-on-year IBQ increases, capped 8",("ibq",),"12 consecutive quarters"),
        ("ocfq_saleq_std","sample std of 16 standalone-quarter OCF/SALEQ",("oancfy","saleq"),"16 consecutive quarters; YTD differenced within fiscal year")):
        rows[name]=Feature(name,"accounting",definition,deps,1,history,implementation="quarterly")
    # Public catalog includes these but detailed source/training definitions are
    # unresolved. Keep columns and denominator; never substitute a plausible proxy.
    for name, reason in (("eq_dur","unresolved source horizon and mean-reversion parameter contract"),):
        rows[name]=Feature(name,"accounting","JKP section 8; unresolved inputs/parameters",(),1,"60 monthly fiscal states",implementation="unsupported",unsupported_reason=reason)
    for name,formula in (("ni_ar1","OLS slope NI/AT on its 12-month lag, 60 complete monthly states"),
                         ("ni_ivol","sqrt(OLS residual sum squares/(n-1)), same NI/AT AR1 sample"),
                         ("earnings_variability","sample std NI/lagAT divided by sample std OCF/lagAT, 60 monthly states")):
        rows[name]=Feature(name,"accounting",formula,("ni","ocf","at","at_l1"),1,"72 exact historical month states rebuilt through each month's strict publication cutoff",implementation="accounting_history")
    market_defs = {
        "age":("months since first certified security observation","value",False),
        "market_equity":("issuer all-eligible-class ME, USD","value",False),
        "prc":("unadjusted close, USD/share","price",False),
        "chcsho_12m":("split-adjusted shares(t)/shares(t-12)-1","issuance",False),
        "eqnpo_12m":("net equity payout: 1-(ME(t)/ME(t-12))/(1+ret_12_0)","issuance",True),
        "div12m_me":("12-month cash dividend dollars divided by issuer ME","value",False),
        "prc_highprc_252d":("adjusted close divided by maximum adjusted high in 252 sessions","price",False),
        "bidaskhl_21d":("mean Corwin-Schultz 2-day high-low spread; overnight adjustment required","price",False),
        "beta_60m":("OLS excess-return beta on 60 monthly market excess returns; min 36","risk",True),
        "corr_1260d":("Pearson correlation of overlapping 3-day excess returns, 1260 sessions; min750","risk",True),
        "betabab_1260d":("corr_1260d * rvol_252d / market_vol_252d","risk",True),
        "betadown_252d":("OLS beta conditional on negative market excess return; min120 total","risk",True),
        "beta_dimson_21d":("sum coefficients of market excess lag/current/lead within trailing21; min15","risk",True),
        "coskew_21d":("mean(CAPMresidual*market_demeaned^2)/sqrt(mean(residual^2)*mean(market_demeaned^2)^2)","risk",True),
        "ami_126d":("mean(abs(total return)/dollar volume)*1e6; min60","liquidity",True),
        "rmax1_21d":("largest trailing21 daily total return; min15","risk",True),
        "rmax5_21d":("mean 5 largest trailing21 daily total returns; min15","risk",True),
        "rmax5_rvol_21d":("rmax5_21d / sample excess-return std252d; denominator min120; every denominator price/return independently certified, no dropping missing cells","risk",True),
        "rskew_21d":("mean cubed demeaned trailing21 excess returns divided by sample std cubed; min15","risk",True),
        "rvol_21d":("sample standard deviation trailing21 daily excess returns; min15","risk",True),
    }
    for n in (21,126,252):
        market_defs[f"zero_trades_{n}d"]=(f"(zero-volume days + 1/(11000*sum daily turnover))*21/{n}; min{15 if n==21 else 60 if n==126 else 120}","liquidity",False)
    for stem in ("dolvol","turnover"):
        market_defs[f"{stem}_126d"]=(f"mean daily {'close*volume' if stem=='dolvol' else 'volume/absolute_shares'}; min60","liquidity",False)
        market_defs[f"{stem}_var_126d"]=(f"sample std daily {stem} divided by mean; min60","liquidity",False)
    for model in ("capm","ff3","hxz4"):
        for stat in ("ivol","iskew"):
            market_defs[f"{stat}_{model}_21d"]=(f"{'sample standard deviation' if stat=='ivol' else 'mean demeaned cube / sample std cubed'} OLS {model} residuals trailing21; intercept; full rank; min15","risk",True)
    market_defs["ivol_capm_252d"]=("sample std CAPM residual trailing252; intercept; min120","risk",True)
    for a,b in ((1,0),(12,1),(12,7),(3,1),(6,1),(60,12),(9,1)):
        market_defs[f"ret_{a}_{b}"]=(f"product(1+total_return) months t-{a}+1 through t-{b}, minus1; complete month/session grid","momentum",True)
    for a in (6,12):
        market_defs[f"resff3_{a}_1"]=(f"(product(1+FF3 residual) lag1 through lag{a})-1, divided by fit residual sample std; trailing36-month fit min24; no future rows","momentum",True)
    for a,b in ((1,1),(2,5),(6,10),(11,15),(16,20)):
        for kind in ("an","na"):
            market_defs[f"seas_{a}_{b}{kind}"]=(f"mean monthly returns lag{12*a if kind=='an' else 12*(a-1)+1}..{12*b} {'multiples12' if kind=='an' else 'excluding multiples12'}; current t at index-1; lag i index-(i+1); maximum lag+1 observations","seasonality",True)
    for name,(formula,family,returns) in market_defs.items():
        sign = -1 if name.startswith(("ivol","rvol","rmax","ami","dolvol","turnover","zero","beta","betabab","chcsho","ret_1_","ret_60_")) else 1
        rows[name]=Feature(name,family,formula,("prices","calendar","issuer_cap_quality","market_factors" if returns else "price_basis"),sign,"see formula; exact trailing windows",return_derived=returns,implementation="market",unit="USD" if name in ("market_equity","dolvol_126d") else "USD/share" if name=="prc" else "dimensionless")
    composite = {
        "mispricing_mgmt":("mean signed cross-sectional percentile ranks, all6 required",("chcsho_12m","eqnpo_12m","oaccruals_at","noa_at","at_gr1","ppeinv_gr1a")),
        "mispricing_perf":("mean signed cross-sectional percentile ranks, all4 required",("o_score","ret_12_1","gp_at","niq_at")),
        "qmj_prof":("z(mean component zscores); all6 required",("gp_at","ni_be","ni_at","ocf_at","gp_sale","oaccruals_at")),
        "qmj_growth":("z(mean component zscores); all5 required",("gpoa_ch5","roe_ch5","roa_ch5","cfoa_ch5","gmar_ch5")),
        "qmj_safety":("z(mean signed component zscores); all5 required",("betabab_1260d","debt_at","o_score","z_score","earnings_variability")),
        "qmj":("mean(qmj_prof,qmj_growth,qmj_safety); all3 required",("qmj_prof","qmj_growth","qmj_safety"))}
    for name,(formula,deps) in composite.items():
        rows[name]=Feature(name,"quality",formula,deps,1,"same-date eligible all-common issuer cross section; >=10 distinct issuers",return_derived=name in ("mispricing_perf","qmj_safety","qmj"),implementation="cross_section")
    # Additional JKP characteristics are explicitly outside the original153 count.
    for num in ("gp","ebitda","ebit","ni","nix","ocf","fcf","cop","debt","cash"):
        for den in ("at","be","bev","me","sale"):
            name=f"{num}_{den}"
            if name not in rows:
                add(name,f"ratio({num},{den})",sign=-1 if num=="debt" else 1)
    for name,num,den in (("gpoa_ch5","gp","at"),("roe_ch5","ni","be"),("roa_ch5","ni","at"),("cfoa_ch5","ocf","at"),("gmar_ch5","gp","sale")):
        add(name,f"ratio({num},{den})-ratio({num}_l5,{den}_l5)",history="6 complete fiscal years")
    # Owner primitive/formula matrix. Extras have no invented JKP identity.
    for primitive in ("sale","ni","ocf","fcf","at","be","capx","xrd","eps_basic","eps_diluted","shares_basic","shares_diluted"):
        for lag in (1,3):
            name=f"owner_{primitive}_gr{lag}"
            add(name,f"growth({primitive},{primitive}_l{lag})","owner",sign=-1 if primitive in ("at","capx","shares_basic","shares_diluted") else 1,history=f"exact {lag} fiscal-year lag; negative/zero base NULL")
    for name,formula in {
        "owner_net_margin":"ratio(nix,sale)","owner_cash_margin":"ratio(ocf,sale)",
        "owner_roa":"ratio(nix,avg(at,at_l1))","owner_roe":"ratio(nix,avg(be,be_l1))",
        "owner_roic":"ratio(ebit*(1-ratio(tax,pi)),avg(ic,ic_l1))",
        "owner_roce":"ratio(ebit,avg(capemp,capemp_l1))",
        "owner_cash_conversion":"ratio(ocf,nix)",
        "owner_wc_growth":"ratio(cowc-cowc_l1,at_l1)",
        "owner_gross_margin_change":"ratio(gp,sale)-ratio(gp_l1,sale_l1)",
        "owner_operating_margin_change":"ratio(ebit,sale)-ratio(ebit_l1,sale_l1)",
        "owner_net_margin_change":"ratio(nix,sale)-ratio(nix_l1,sale_l1)"}.items():
        add(name,formula,"owner",history="2 fiscal-year snapshots; averages and tax rate require all primitives")
    for primitive in ("sale","ni","ocf","fcf","at","be","capx","xrd","eps_basic","eps_diluted","shares_basic","shares_diluted"):
        for frequency in ("q_yoy","ttm_yoy"):
            name=f"owner_{primitive}_{frequency}"
            unsupported=frequency=="ttm_yoy" and primitive in ("eps_basic","eps_diluted","shares_basic","shares_diluted")
            rows[name]=Feature(name,"owner",f"{primitive} {frequency} growth, prior positive base only",(primitive,),1,"5 consecutive quarters for q; 8 consecutive quarters for TTM; YTD flows additionally require preceding within-year quarter; exact fiscal/currency vintages",implementation="unsupported" if unsupported else "quarterly",unsupported_reason="TTM weighted earnings and shares basis not independently admitted" if unsupported else None)
    extras={"chs_proxy":"Campbell-Hilscher-Szilagyi NIMTA,TLMTA,EXRET,SIGMA,RSIZE,CASHMTA,MB,PRICE failure-probability score; exact coefficients and market basis unresolved",
            "insider_net_buy_6m":"six-month net split-adjusted insider open-market shares / dated issuer shares",
            "insider_buyers_6m":"distinct buying insiders in six months; zero only after complete issuer filing-cohort proof",
            "insider_opportunistic_buy":"CMP source-classified opportunistic net split-adjusted shares; classification known before month cutoff",
            "seo_recent":"indicator of source-backed follow-on offering in last252 pinned sessions; alias seo_recent_252",
            "seo_recent_252":"indicator of source-backed follow-on offering in last252 pinned sessions",
            "nonreliance_402":"indicator of accepted 8-K Item4.02 nonreliance within252 pinned sessions"}
    for name,formula in extras.items():
        rows[name]=Feature(name,"owner",formula,(),-1 if name.startswith(("chs","seo","nonreliance")) else 1,"source-dependent; exact trailing session/month cohort",return_derived=name=="chs_proxy",implementation="unsupported",unsupported_reason="independently clocked complete source cohort and exact source-quality definition not admitted")
    rows["ceq_iss_5y"]=Feature("ceq_iss_5y","owner","log(issuerME(t)/issuerME(t-60)) - log(product60 complete monthly total returns)",("monthly_market.issuer_me","monthly_market.ret"),-1,"61 consecutive months, dated issuer cap and complete split/delisting-adjusted return basis",return_derived=True,implementation="market")
    assert len(JKP153)==153 and len(set(JKP153))==153
    assert not set(JKP153)-set(rows),set(JKP153)-set(rows)
    negative=set("""cowc_gr1a oaccruals_at oaccruals_ni taccruals_at taccruals_ni capex_abn debt_gr3 fnl_gr1a ncol_gr1a noa_at aliq_at at_gr1 be_gr1a capx_gr1 capx_gr2 capx_gr3 coa_gr1a col_gr1a emp_gr1 inv_gr1 inv_gr1a lnoa_gr1a ncoa_gr1a nncoa_gr1a noa_gr1a ppeinv_gr1a ret_60_12 sale_gr1 sale_gr3 saleq_gr1 seas_2_5na age aliq_mat at_be netdebt_me beta_60m beta_dimson_21d betabab_1260d betadown_252d earnings_variability ivol_capm_21d ivol_capm_252d ivol_ff3_21d ivol_hxz4_21d ocfq_saleq_std rmax1_21d rmax5_21d rvol_21d seas_6_10na turnover_126d dsale_drec dolvol_var_126d o_score turnover_var_126d corr_1260d coskew_21d dbnetis_at lti_gr1a seas_11_15na seas_16_20an dolvol_126d market_equity prc iskew_capm_21d iskew_ff3_21d iskew_hxz4_21d ret_1_0 rmax5_rvol_21d rskew_21d chcsho_12m eq_dur eqnetis_at netis_at""".split())
    # Explicit complete original-paper directions from JKP table9, never selected
    # by observing returns. Natural output values remain unchanged.
    for name in JKP153:rows[name]=replace(rows[name],economic_sign=-1 if name in negative else 1)
    return tuple(rows.values())


REGISTRY = _registry()
BY_NAME = {f.name:f for f in REGISTRY}
FREEZE = {"version":"char-monthly-v1-frozen-r18", "original153":list(JKP153),
          "primitive_definitions":PRIMITIVES,
          "registry":[f.record() for f in REGISTRY], "coverage_years":list(range(2020,2026)),
          "coverage_populations":["member_equity","univ_all_common","univ_r3000p"],
          "gates":{"implemented_original153":150,"accounting_coverage":0.90,"price_coverage":0.97,
                   "jkp_rho":0.70,"jkp_matched_share":0.70,"jkp_sign_share":0.90,"osap_median_rho":0.60,"ratios_rho":0.99},
          "holdout":{"return_statistics_end_exclusive":"2023-01-01","complete_label_window":True},
          "trials":{"primary":"Task16 predeclared family unchanged; this whole registry exploratory", "no_outcome_selection":True},
          "normalization":"natural raw values; composite signs from predeclared economics; no winsorization",
          "ratio_comparison":"exact admitted final monthly inventory and original72dates; every original eligible member must have compatible independently pinned annual/fiscal/currency/cap/vintage/definition proof or gate FAIL",
          "formation_audit":"fixed key/cik alignment; independent source and evidence clocks; matching issuer cap; qualifications only, protected values never overwritten",
          "replication":{"tercile_breakpoints":[1/3,2/3],"nonmicro_nyse_percentile":0.20,"cap_nyse_percentile":0.80,"minimum_names_per_leg":5,"minimum_months":60,"original_matched_factors":list(JKP153),"holding_month_start":"2013-01","holding_month_end":"2022-12","formation_month_start":"2012-12","formation_month_end":"2022-11","requested_months":120,"period_end_exclusive":"2023-01-01","osap_overlap":"nonempty independently admitted fixed metadata mapping before returns; all mapped members denominator","all_outcomes_reported":True},
          "growth":"positive previous base only; zero/negative prior -> explicit NULL",
          "history":"exact fiscal year/quarter and currency continuity; never row-number lags across gaps"}
FREEZE_SHA256 = digest(FREEZE)
