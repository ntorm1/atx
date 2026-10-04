"""Explicit native v11 adapters. No implicit stage, current pointer or legacy fallback."""
from __future__ import annotations
import datetime as dt
from pathlib import Path
from . import common as C
from . import release_inputs as R
from . import universe_release as U

PRODUCTS=("calendar","universe","fundq","funda","ratios_state","prices","member_equity",
          "primitive_quality","cap_quality","price_quality","daily_factors","monthly_factors",
          "monthly_market","sector")


def sql(paths):
    if not paths:raise ValueError("explicit nonempty source product required")
    quoted=",".join("'"+Path(p).as_posix().replace("'","''")+"'" for p in paths)
    return f"read_parquet([{quoted}],hive_partitioning=false,union_by_name=false)"


def admit(root,bindings,con):
    """Authenticate full upstream generations and exact reader product files.

    All quality products require independent admission strings and explicit files;
    a producer's own manifest does not establish economic/currency certification.
    Unknown products are supplied as explicit typed empty source publications.
    """
    if bindings.get("version")!="char_monthly_bindings_v1":raise ValueError("unsupported monthly input bindings")
    if set(bindings.get("products",{}))!=set(PRODUCTS):raise ValueError("incomplete native input contract")
    universe=R.safe_path(root,bindings["universe_generation"])
    if universe.parent!=root/"universe_release_v1"/"generations":raise ValueError("explicit universe_release_v1 generation required")
    if R.identity(universe/"manifest.json")!=bindings["universe_manifest"]:raise RuntimeError("universe manifest pin differs")
    universe_manifest=U.validate_generation(root,universe,con)
    fund=R.safe_path(root,bindings["fundamentals_generation"])
    if fund.parent!=root/"fundamentals_release_v1"/"generations":raise ValueError("explicit fundamentals_release_v1 generation required")
    if R.identity(fund/"manifest.json")!=bindings["fundamentals_manifest"]:raise RuntimeError("fundamentals manifest pin differs")
    from . import fund_batch_release as F
    fm=F.validate_generation(root/"fundamentals_release_v1",fund,con)
    if fm.get("semantic_producer")!="fundamentals_v11":raise RuntimeError("unqualified semantic fundamental producer")
    products=bindings["products"]
    expected_universe={str((universe/relative).relative_to(root)).replace("\\","/"):row for relative,row in universe_manifest["files"].items()}
    if products["universe"]["files"]!=expected_universe:raise RuntimeError("universe reader differs from complete admitted generation")
    # Static schema/producer identity is preserved, but concrete paths/bytes are
    # supplied only after final producer completion by the controller.
    for name,stage in (("fundq","compustat_std_v11"),("funda","compustat_std_v11"),("ratios_state","ratios_v11")):
        if products[name].get("manifest")!=f"{stage}/manifest.json":raise RuntimeError("v11 manifest name required")
        if any(not Path(p).parts or Path(p).parts[0]!=stage for p in products[name]["files"]):raise RuntimeError("legacy accounting source rejected")
    std=C.read_json(root/"compustat_std_v11"/"manifest.json")
    ratio=C.read_json(root/"ratios_v11"/"manifest.json")
    expected_fund=(fund/"fundamentals_v11").relative_to(root).as_posix()
    if std.get("fund_stage")!=expected_fund:raise RuntimeError("standardized source fundamental generation not pinned")
    if ratio.get("input_manifests_sha256",{}).get("compustat_std_v11")!=C.sha256_file(root/"compustat_std_v11"/"manifest.json"):raise RuntimeError("ratio standardized producer mismatch")
    if std.get("input_manifests_sha256",{}).get(f"{expected_fund}/manifest.json")!=C.sha256_file(fund/"fundamentals_v11"/"manifest.json"):raise RuntimeError("standardized underlying fundamental manifest mismatch")
    contract={name:{"pattern":row.get("pattern"),"manifest":row.get("manifest")} for name,row in products.items()}
    if any(not isinstance(row["pattern"],str) or ".." in Path(row["pattern"]).parts for row in contract.values()):raise ValueError("explicit safe reader pattern required")
    frozen=R.snapshot(root,contract,products,code_paths(),external={"universe_manifest":universe/"manifest.json","fundamentals_manifest":fund/"manifest.json"})
    for name,row in products.items():con.execute(f"CREATE OR REPLACE TEMP VIEW cm_{name} AS SELECT * FROM {sql([R.safe_path(root,p) for p in row['files']])}")
    check_relations(con)
    return frozen


def code_paths():
    here=Path(__file__).parent
    return [here/(n+".py") for n in ("char_monthly","char_monthly_registry","char_monthly_kernels","char_monthly_sources","char_monthly_evidence","release_inputs","universe_release","fund_batch_release","common")]


def check_relations(con):
    for relation,keys in (("universe","session_date,security_id"),("prices","session_date,security_id"),
                          ("calendar","session_date"),("member_equity","session_date,security_id"),
                          ("funda","cik,datadate,accession,available_at"),("fundq","cik,datadate,accession,available_at"),
                          ("monthly_market","security_id,month_end,available_at"),("daily_factors","session_date,available_at"),
                          ("monthly_factors","month_end,available_at"),("cap_quality","security_id,session_date,available_at"),
                          ("price_quality","security_id,session_date,available_at"),
                          ("primitive_quality","cik,datadate,accession,item,snapshot_available_at,available_at"),
                          ("sector","cik,available_at")):
        if con.execute(f"SELECT 1 FROM cm_{relation} GROUP BY {keys} HAVING count(*)<>1 LIMIT 1").fetchone():raise RuntimeError(f"ambiguous native {relation} keys")
    for name in ("funda","fundq","cap_quality","primitive_quality","price_quality","sector","member_equity"):
        cols=dict((r[0],r[1]) for r in con.execute(f"DESCRIBE cm_{name}").fetchall())
        if cols.get("available_at") not in ("TIMESTAMP WITH TIME ZONE","TIMESTAMP"):raise RuntimeError(f"missing native clock:{name}")


def records(con,query,params=()):
    cur=con.execute(query,params)
    names=[r[0] for r in cur.description]
    return [dict(zip(names,row)) for row in cur.fetchall()]


def clock(date):return dt.datetime.combine(date,dt.time(22),dt.UTC)


def months(con,start,end):
    if start>end:raise ValueError("reversed monthly range")
    return [r[0] for r in con.execute("SELECT max(session_date) FROM cm_calendar GROUP BY date_trunc('month',session_date) HAVING max(session_date) BETWEEN ? AND ? AND max(session_date)<=(SELECT max(session_date) FROM cm_universe) ORDER BY 1",[start,end]).fetchall()]


def population(con,date):
    # Universe's row publication is precisely 22:00; it supplies the independent
    # key axis. Its primitive membership/cap clocks are audited separately.
    return records(con,"""SELECT u.*, m.member_equity FROM cm_universe u LEFT JOIN cm_member_equity m
        ON u.security_id=m.security_id AND u.session_date=m.session_date AND m.available_at<? AND m.source_available_at<?
        WHERE u.session_date=? ORDER BY u.security_id""",[clock(date),clock(date),date])


def accounting(con,cik,date,kind,all_vintages=False):
    if cik is None:return [],[]
    tau=clock(date)
    latest="" if all_vintages else "QUALIFY row_number() OVER(PARTITION BY cik,datadate ORDER BY available_at DESC,accession DESC)=1"
    rows=records(con,f"""SELECT * FROM cm_{kind} WHERE cik=? AND available_at<? AND datadate<=?
       {latest} ORDER BY datadate DESC,available_at DESC LIMIT 501""",[cik,tau,date])
    if len(rows)>500:raise RuntimeError("issuer accounting snapshot bound exceeded; history not silently truncated")
    # Only certified source-backed primitives enter kernels. Source zero-fill,
    # unsupported units or a delayed component all remain explicit unknowns.
    clocks=[]
    for row in rows:
        proofs=records(con,"""SELECT * FROM cm_primitive_quality WHERE cik=? AND datadate=? AND accession=?
            AND available_at<? AND snapshot_available_at=? QUALIFY row_number() OVER(PARTITION BY item ORDER BY available_at DESC)=1""",
            [cik,row["datadate"],row["accession"],tau,row["available_at"]])
        by_item={r["item"]:r for r in proofs}
        for key,value in list(row.items()):
            if key in ("cik","fyear","fyearq","fqtr","datadate","accession","available_at","curcd","sic"):continue
            if not isinstance(value,(int,float)):continue
            proof=by_item.get(key)
            expected="shares" if key.startswith(("csho","cshpri","cshfd")) else "USD/share" if key.startswith("eps") else "thousand persons" if key=="emp" else "USD"
            if proof is None or proof.get("certified") is not True or proof.get("unit")!=expected or proof.get("source_available_at") is None or proof["source_available_at"]>=tau or proof["source_available_at"]>row["available_at"] or proof.get("snapshot_available_at")!=row["available_at"]:
                row[key]=None
            else:clocks.append(proof["source_available_at"])
        clocks.append(row["available_at"])
    return rows,clocks


def cap(con,cik,date,fiscal_date=None):
    from .char_monthly_kernels import finite
    tau=clock(date)
    def at(day):
        rows=records(con,"""SELECT u.*,q.* EXCLUDE(security_id,session_date) FROM cm_universe u LEFT JOIN (
      SELECT * FROM cm_cap_quality WHERE available_at<? QUALIFY row_number() OVER(PARTITION BY security_id,session_date ORDER BY available_at DESC)=1
      ) q USING(security_id,session_date) WHERE u.cik=? AND u.session_date=? AND u.univ_all_common""",[tau,cik,day])
        good=bool(rows) and all(finite(r.get("me")) and r["me"]>0 and r.get("certified") is True
         and r.get("membership_certified") is True and r.get("currency")=="USD"
         and r.get("source_available_at") is not None and r["source_available_at"]<tau for r in rows)
        value=sum(r["me"] for r in rows) if good else None
        return value,[t for r in rows if good for t in (r["source_available_at"],r["available_at"])]
    value,clocks=at(date)
    prior_target=date.replace(year=date.year-1) if (date.month,date.day)!=(2,29) else dt.date(date.year-1,2,28)
    prior=con.execute("SELECT max(session_date) FROM cm_calendar WHERE session_date<=?",[prior_target]).fetchone()[0]
    me_prior,pc=at(prior) if prior else (None,[])
    fiscal=con.execute("SELECT max(session_date) FROM cm_calendar WHERE session_date<=?",[fiscal_date]).fetchone()[0] if fiscal_date else None
    fiscal_me,fc=at(fiscal) if fiscal else (None,[])
    return {"me":value,"cap_reason":None if value is not None else "invalid_or_uncertified_eligible_issuer_cap","fiscal_me":fiscal_me,"me_l1":me_prior},clocks+pc+fc


def price_history(con,sid,date):
    tau=clock(date)
    quality_columns={r[0] for r in con.execute("DESCRIBE cm_price_quality").fetchall()}
    first_sql="q.first_observation" if "first_observation" in quality_columns else "CAST(NULL AS DATE)"
    # Latest visible quality revocation wins. Missing sessions survive the left
    # join and force missingness rather than shortening a trailing window.
    bars=records(con,f"""WITH grid AS (SELECT session_date FROM cm_calendar WHERE session_date<=? ORDER BY session_date DESC LIMIT 1260),
        qual AS (SELECT * FROM cm_price_quality WHERE security_id=? AND available_at<?
          QUALIFY row_number() OVER(PARTITION BY session_date ORDER BY available_at DESC)=1),
        factors AS (SELECT * FROM cm_daily_factors WHERE session_date<DATE '2023-01-01' AND available_at<?
          QUALIFY row_number() OVER(PARTITION BY session_date ORDER BY available_at DESC)=1)
        SELECT g.session_date,p.close,p.volume,q.shares,q.close_adjusted,q.high_adjusted,
          q.price_certified,q.return_certified,q.corwin_schultz_spread,q.source_available_at,q.available_at AS quality_available_at,{first_sql} AS first_observation,
          CASE WHEN g.session_date<DATE '2023-01-01' THEN p.ret END AS ret,
          CASE WHEN g.session_date<DATE '2023-01-01' THEN p.ret-f.rf END AS ret_exc,
          f.mkt_rf,f.smb,f.hml,f.q_size,f.q_ia,f.q_roe,f.available_at AS factor_available_at
        FROM grid g LEFT JOIN cm_prices p ON p.security_id=? AND p.session_date=g.session_date
        LEFT JOIN qual q ON q.session_date=g.session_date LEFT JOIN factors f ON f.session_date=g.session_date ORDER BY g.session_date""",[date,sid,tau,tau,sid])
    monthly=records(con,"""WITH factors AS (SELECT * FROM cm_monthly_factors WHERE month_end<DATE '2023-01-01' AND available_at<?
        QUALIFY row_number() OVER(PARTITION BY month_end ORDER BY available_at DESC)=1),
      data AS (SELECT * FROM cm_monthly_market WHERE security_id=? AND month_end<=? AND available_at<?
        QUALIFY row_number() OVER(PARTITION BY security_id,month_end ORDER BY available_at DESC)=1 ORDER BY month_end DESC LIMIT 241)
      SELECT d.month_end,d.shares_adjusted,d.issuer_me,d.dividend_dollars,d.available_at,d.source_available_at,
        CASE WHEN d.month_end<DATE '2023-01-01' AND d.return_certified THEN d.ret END AS ret,
        CASE WHEN d.month_end<DATE '2023-01-01' AND d.return_certified THEN d.ret-f.rf END AS ret_exc,
        f.mkt_rf,f.smb,f.hml,f.available_at AS factor_available_at FROM data d LEFT JOIN factors f USING(month_end) ORDER BY month_end""",[tau,sid,date,tau])
    # A sequence of observed rows is not an exact month grid.
    calendar_months=set(months(con,dt.date(date.year-21,1,1),date))
    verified=[]
    for row in reversed(monthly):
        if row["month_end"] not in calendar_months:break
        verified.append(row)
    monthly=list(reversed(verified))
    for older,newer in zip(monthly,monthly[1:]):
        if (newer["month_end"].year-older["month_end"].year)*12+newer["month_end"].month-older["month_end"].month!=1:
            monthly=[];break
    clocks=[]
    for bar in bars:
        if bar.get("source_available_at") is None or bar["source_available_at"]>=tau:
            bar["price_certified"]=False;bar["return_certified"]=False
        else:clocks.extend(t for t in (bar["source_available_at"],bar["quality_available_at"],bar["factor_available_at"]) if t is not None)
    for row in monthly:
        if row.get("source_available_at") is None or row["source_available_at"]>=tau:row.update(ret=None,ret_exc=None,shares_adjusted=None,issuer_me=None,dividend_dollars=None)
        else:clocks.extend(t for t in (row["available_at"],row["source_available_at"],row["factor_available_at"]) if t is not None)
    return bars,monthly,clocks
