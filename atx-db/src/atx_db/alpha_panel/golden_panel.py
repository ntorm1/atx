"""Immutable bounded golden daily/year and monthly panels with public replay.

Diagnostic publication preserves every source failure. No producer certification
or consumer acceptance follows from a successful operational publication.
"""
from __future__ import annotations
import argparse
import datetime as dt
import json
import os
import random
import re
import uuid
from contextlib import contextmanager
from pathlib import Path
import pyarrow.parquet as pq
from . import common as C
from . import golden_sources as S, golden_registry as R

STAGE = "golden_panel"
SCHEMA = "atx.golden-panel/v1"
POPS = ("member_equity", "univ_all_common", "univ_r3000p")
AXES = ("session_date", "security_id", "cik", "issuer_id", *POPS, "exchcd_like", "available_at")


def ident(name):
    if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
        raise ValueError("invalid column identifier")
    return '"' + name + '"'


def sql(paths):
    if not paths:
        raise ValueError("empty source inventory")
    quoted = ",".join("'" + Path(p).as_posix().replace("'", "''") + "'" for p in paths)
    return f"read_parquet([{quoted}],hive_partitioning=false,union_by_name=false)"


@contextmanager
def writer_lock(stage):
    stage.mkdir(parents=True, exist_ok=True)
    path = stage / "writer.lock"
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        yield
    finally:
        os.close(fd)
        path.unlink()


def connect(scratch):
    return C.connect(memory="128MB", threads=1, temp_dir=Path(scratch), db_file="golden-"+uuid.uuid4().hex+".duckdb")


def admit_plan(plan, con):
    if plan.get("schema") != "atx.golden-bindings/v1" or not plan.get("independent_review"):
        raise ValueError("independently reviewed complete native plan required")
    if plan.get("code") != S.code_pins() or plan.get("registry_sha256") != S.digest(R.registry()):
        raise ValueError("stale code/registry")
    sources = plan.get("sources", {})
    if "universe_release_v1" not in sources:
        raise ValueError("native complete universe generation mandatory")
    for name in sources:
        if name in ("events_guidance_v2", "char_monthly") and not sources[name].get("independent_review"):
            raise ValueError("source generation independent review prerequisite")
    admitted = {name: S.admit(name, spec, con) for name, spec in sources.items()}
    exclusions = {name: reason for name, reason in S.UNSUPPORTED.items() if name not in sources}
    for name, row in admitted.items():
        if row["status"] == "excluded":
            exclusions[name] = row["reason"]
    for required in ("char_monthly", "events_guidance_v2", "fundamentals_release_v1"):
        if required not in admitted:
            exclusions[required] = "independently reviewed native generation not supplied"
    return admitted, exclusions


def bind_views(con, admitted):
    products = admitted["universe_release_v1"]["products"]
    universe = [v for k, v in products.items() if k.startswith("universe_")]
    panels = [p for k, v in products.items() if k.startswith("panel_") for p in v]
    con.execute(f"CREATE OR REPLACE TEMP VIEW gu AS SELECT * FROM {sql(universe)}")
    con.execute(f"CREATE OR REPLACE TEMP VIEW gp AS SELECT * FROM {sql(panels)}")
    con.execute(f"CREATE OR REPLACE TEMP VIEW gc AS SELECT session_date, lag(session_date) OVER(ORDER BY session_date) previous_session, max(session_date) OVER(PARTITION BY date_trunc('month',session_date)) month_end FROM {sql(products['calendar'])}")
    if "char_monthly" in admitted:
        products=admitted["char_monthly"]["products"]
        con.execute(f"CREATE OR REPLACE TEMP VIEW gm AS SELECT * FROM {sql([v for k,v in products.items() if not k.startswith('_')])}")
        for key in ("cap_quality","price_quality","daily_factors","prices"):
            con.execute(f"CREATE OR REPLACE TEMP VIEW g_{key} AS SELECT * FROM {sql(products['_'+key])}")
    for source,product,view in (("standardized_v11","fundq","gst_q"),("standardized_v11","funda","gst_a"),("ea_actuals","ann_features.parquet","gea")):
        if source in admitted and admitted[source]["status"]!="excluded" and product in admitted[source]["products"]:
            con.execute(f"CREATE OR REPLACE TEMP VIEW {view} AS SELECT * FROM {sql([admitted[source]['products'][product]])}")
    if 'standardized_v11' in admitted and admitted['standardized_v11']['status']!='excluded':
        leaf=Path(admitted['standardized_v11']['products']['qualified_fundamental_leaf'])
        con.execute('CREATE OR REPLACE TEMP VIEW g_fund_clock AS SELECT * FROM '+sql([leaf/'events.parquet']))
    if "ratios_v11" in admitted and admitted["ratios_v11"]["status"]!="excluded":
        native=[v for k,v in admitted["ratios_v11"]["products"].items() if k.startswith("monthly/")]
        if native:con.execute(f"CREATE OR REPLACE TEMP VIEW gr AS SELECT * FROM {sql(native)}")
    if "events_guidance_v2" in admitted:
        path = admitted["events_guidance_v2"]["products"]["guidance_features.parquet"]
        con.execute(f"CREATE OR REPLACE TEMP VIEW gg AS SELECT * FROM {sql([path])}")
    for source,product,view in (('text_features','text/features.parquet','gtxt'),('classification_tnic','classification_tnic/firms.parquet','gtnic')):
        if source in admitted and admitted[source]['status']!='excluded':
            con.execute(f'CREATE OR REPLACE TEMP VIEW {view} AS SELECT * FROM '+sql([admitted[source]['products'][product]]))
    if 'fundamentals_notes' in admitted and admitted['fundamentals_notes']['status']!='excluded':
        con.execute('CREATE OR REPLACE TEMP VIEW gnotes AS SELECT * FROM '+sql([admitted['fundamentals_notes']['products']['notes_cells.parquet']]))
    for source,view in (('short_interest','gsi'),('short_volume_ext','gsv')):
        if source in admitted and admitted[source]['status']!='excluded':
            paths=[value for key,value in admitted[source]['products'].items() if key.endswith('_cells.parquet')]
            if not paths:raise ValueError('complete native mapped short products required')
            con.execute('CREATE OR REPLACE TEMP VIEW '+view+' AS SELECT * FROM '+sql(paths))
    # Keys and population are independently verified before any join/ranking.
    for relation in ("gu", "gp"):
        if con.execute(f"SELECT 1 FROM {relation} GROUP BY session_date,security_id HAVING count(*)<>1 OR security_id IS NULL OR session_date IS NULL LIMIT 1").fetchone():
            raise ValueError("duplicate/null complete source keys")
    if con.execute("SELECT 1 FROM ((SELECT session_date,security_id FROM gu EXCEPT ALL SELECT session_date,security_id FROM gp) UNION ALL (SELECT session_date,security_id FROM gp EXCEPT ALL SELECT session_date,security_id FROM gu)) LIMIT 1").fetchone():
        raise ValueError("complete universe/panel membership mismatch")


def query(kind, start, end, admitted):
    """One bounded unit; strict daily preceding CALENDAR day cutoff per GC1."""
    if kind not in ("daily", "monthly"):
        raise ValueError("unsupported panel kind")
    cutoff = "CAST(u.session_date AS TIMESTAMPTZ) + INTERVAL '22 hours'"
    if kind == "daily":
        cutoff += " - INTERVAL '1 day'"
    where = f"u.session_date BETWEEN DATE '{start}' AND DATE '{end}' AND u.session_date>=DATE '2020-01-01'"
    if kind == "monthly":
        where += " AND u.session_date=c.month_end"
    fields, joins = [], []
    registry = R.registry()
    if "char_monthly" in admitted:
        comparison = "<=" if kind == "monthly" else "<"
        joins.append(f"LEFT JOIN LATERAL (SELECT * FROM gm WHERE security_id=u.security_id AND available_at{comparison}{cutoff} AND session_date<=u.session_date AND session_date>=u.session_date-INTERVAL '400 days' ORDER BY available_at DESC,session_date DESC LIMIT 1) m ON true")
        joins.append(f"LEFT JOIN LATERAL (SELECT * FROM g_cap_quality WHERE security_id=u.security_id AND session_date=cap.session_date AND available_at<{cutoff} ORDER BY available_at DESC LIMIT 1) capq ON true")
        joins.append(f"LEFT JOIN LATERAL (SELECT p.ret,q.return_certified,q.source_available_at,q.available_at,f.rf,f.available_at factor_at FROM g_prices p LEFT JOIN LATERAL (SELECT * FROM g_price_quality WHERE security_id=p.security_id AND session_date=p.session_date AND available_at<{cutoff} ORDER BY available_at DESC LIMIT 1) q ON true LEFT JOIN LATERAL (SELECT rf,available_at FROM g_daily_factors WHERE session_date=p.session_date AND available_at<{cutoff} ORDER BY available_at DESC LIMIT 1) f ON true WHERE p.security_id=u.security_id AND CAST(p.session_date AS TIMESTAMPTZ)+INTERVAL '22 hours'<{cutoff} ORDER BY p.session_date DESC LIMIT 1) retq ON true")
    if "events_guidance_v2" in admitted:
        # Different target/basis rows at the same latest clock are ambiguous.
        joins.append(f"LEFT JOIN LATERAL (SELECT available_at, CASE WHEN count(*)=1 THEN max(guid_surprise) END guid_surprise, CASE WHEN count(*)=1 THEN max(guid_revision) END guid_revision, CASE WHEN count(*)=1 THEN max(feature_reason) ELSE 'ambiguous_latest_guidance' END feature_reason FROM gg WHERE cik=u.cik AND (security_id=u.security_id OR security_id IS NULL) AND available_at<{cutoff} AND available_at>={cutoff}-INTERVAL '400 days' GROUP BY available_at ORDER BY available_at DESC LIMIT 1) g ON true")
    joins.append(f"LEFT JOIN LATERAL (SELECT me,session_date,available_at FROM gu WHERE security_id=u.security_id AND available_at<{cutoff} ORDER BY session_date DESC LIMIT 1) cap ON true")
    joins.append(f"LEFT JOIN LATERAL (SELECT ret,ret_guarded,session_date FROM gp WHERE security_id=u.security_id AND CAST(session_date AS TIMESTAMPTZ)+INTERVAL '22 hours'<{cutoff} ORDER BY session_date DESC LIMIT 1) pr ON true")
    # capq references the exact selected raw cap vintage; move it after cap.
    joins.sort(key=lambda j: 0 if ") cap ON true" in j else 1)
    from . import compustat_std as STD
    quarterly={r[0] for r in (*STD.FUNDQ_ITEMS,*STD.DERIVED_Q)}
    for alias,view in (("stq","gst_q"),("sta","gst_a")):
        if "standardized_v11" in admitted and admitted["standardized_v11"]["status"]!="excluded":
            joins.append(f"LEFT JOIN LATERAL (SELECT *,count(*) OVER(PARTITION BY available_at,datadate) n_vintages FROM {view} WHERE cik=u.cik AND available_at<{cutoff} AND datadate<=u.session_date ORDER BY available_at DESC,datadate DESC LIMIT 1) {alias} ON true")
    if "ratios_v11" in admitted and admitted["ratios_v11"]["status"]!="excluded":
        joins.append(f"LEFT JOIN LATERAL (SELECT * FROM gr WHERE security_id=u.security_id AND available_at<{cutoff} ORDER BY available_at DESC,session_date DESC LIMIT 1) r ON true")
    if "ea_actuals" in admitted and admitted["ea_actuals"]["status"]!="excluded" and "ann_features.parquet" in admitted["ea_actuals"]["products"]:
        joins.append(f"LEFT JOIN LATERAL (SELECT *,count(*) OVER(PARTITION BY available_at) n_vintages FROM gea WHERE cik=u.cik AND available_at<{cutoff} ORDER BY available_at DESC LIMIT 1) ea ON true")
    for source,view,alias in (('text_features','gtxt','txt'),('classification_tnic','gtnic','tnic')):
        if source in admitted and admitted[source]['status']!='excluded':
            joins.append(f'LEFT JOIN LATERAL (SELECT *,count(*) OVER(PARTITION BY available_at) n_vintages FROM {view} WHERE cik=u.cik AND available_at<{cutoff} ORDER BY available_at DESC LIMIT 1) {alias} ON true')
    if 'fundamentals_notes' in admitted and admitted['fundamentals_notes']['status']!='excluded':
        for name,contract in registry.items():
            if contract['source']!='fundamentals_notes' or contract['kind']=='normalized_score':continue
            alias=ident('note_'+name)
            joins.append(f"LEFT JOIN LATERAL (SELECT *,count(*) OVER(PARTITION BY available_at,period) n_vintages FROM gnotes WHERE cik=u.cik AND field='{name}' AND available_at<{cutoff} AND period<=u.session_date ORDER BY available_at DESC,period DESC LIMIT 1) {alias} ON true")
    for source,view,alias,date in (('short_interest','gsi','si','settlement_date'),('short_volume_ext','gsv','sv','trade_date')):
        if source in admitted and admitted[source]['status']!='excluded':
            joins.append(f'LEFT JOIN LATERAL (SELECT *,count(*) OVER(PARTITION BY available_at,{date}) n_vintages FROM {view} WHERE security_id=u.security_id AND available_at<{cutoff} AND {date}<=u.session_date ORDER BY available_at DESC,{date} DESC LIMIT 1) {alias} ON true')
    if 'short_interest' in admitted and admitted['short_interest']['status']!='excluded' and 'char_monthly' in admitted:
        joins.append(f'LEFT JOIN LATERAL (SELECT *,count(*) OVER(PARTITION BY available_at,session_date) n_vintages FROM g_price_quality WHERE security_id=u.security_id AND session_date=si.settlement_date AND available_at<{cutoff} ORDER BY available_at DESC LIMIT 1) sden ON true')
        joins.append(f'LEFT JOIN LATERAL (SELECT * FROM g_cap_quality WHERE security_id=u.security_id AND session_date=si.settlement_date AND available_at<{cutoff} ORDER BY available_at DESC LIMIT 1) sdenq ON true')
    for name, contract in registry.items():
        if contract['kind']=='normalized_score':
            continue
        source = contract["source"]
        source_reason=S.UNSUPPORTED.get(source,"independently reviewed native source admission pending").replace("'","''")
        value, clock, reason, qualified = "NULL::DOUBLE", "NULL::TIMESTAMPTZ", "'"+source_reason+"'", "false"
        if source == "char_monthly" and "char_monthly" in admitted:
            value, clock = "m." + ident(name), f"TRY_CAST(json_extract_string(m.source_clocks,'$.{name}') AS TIMESTAMPTZ)"
            reason = f"coalesce(json_extract_string(m.quality_reasons,'$.{name}'),'monthly_not_yet_available')"
            qualified=f"m.{ident(name)} IS NOT NULL"
        elif name in ("guid_surprise", "guid_revision") and "events_guidance_v2" in admitted:
            value, clock, reason = "g." + ident(name), "g.available_at", "coalesce(g.feature_reason,'no_visible_guidance')"
            qualified="g.feature_reason='qualified'"
        elif name == "me":
            value, clock, reason = "cap.me", "cap.available_at", "'no_visible_cap'"
            if "char_monthly" in admitted:
                qualified=f"capq.certified AND capq.membership_certified AND capq.currency='USD' AND capq.source_available_at<{cutoff}"
                clock="greatest(cap.available_at,capq.available_at,capq.source_available_at)"
        elif name == "ret":
            value = "CASE WHEN NOT pr.ret_guarded THEN pr.ret END"
            clock, reason = "CAST(pr.session_date AS TIMESTAMPTZ)+INTERVAL '22 hours'", "'missing_or_guarded_historical_return'"
            if "char_monthly" in admitted:
                qualified=f"retq.return_certified AND retq.source_available_at<{cutoff} AND retq.ret=pr.ret"
                clock="greatest(CAST(pr.session_date AS TIMESTAMPTZ)+INTERVAL '22 hours',retq.available_at,retq.source_available_at)"
        elif name=="ret_excess" and "char_monthly" in admitted:
            value="CASE WHEN retq.return_certified THEN retq.ret-retq.rf END"
            qualified=f"retq.return_certified AND retq.source_available_at<{cutoff} AND retq.factor_at<{cutoff}"
            clock="greatest(retq.source_available_at,retq.available_at,retq.factor_at)"
            reason="'riskfree_or_historical_return_unqualified'"
        elif source=="standardized_v11" and "standardized_v11" in admitted and admitted["standardized_v11"]["status"]!="excluded":
            native_name=name.removeprefix("std_");alias="stq" if native_name in quarterly else "sta"
            value=f"CASE WHEN {alias}.n_vintages=1 AND {alias}.datadate>=u.session_date-INTERVAL '550 days' THEN {alias}.{ident(native_name)} END"
            clock=alias+".available_at";reason="'missing_stale_or_ambiguous_standardized_vintage'"
            qualified=f"{alias}.n_vintages=1 AND ({alias}.curcd='USD' OR ({alias}.fx_converted AND {alias}.fx_rate>0)) AND EXISTS(SELECT 1 FROM g_fund_clock fc WHERE fc.cik={alias}.cik AND fc.accession={alias}.accession AND fc.available_at<={alias}.available_at AND fc.clock_basis IS NOT NULL AND fc.clock_basis NOT LIKE '%unresolved%' AND fc.clock_basis NOT LIKE '%unknown%')"
        elif source=="ratios_v11" and "ratios_v11" in admitted and admitted["ratios_v11"]["status"]!="excluded":
            value="r."+ident(name.removeprefix("ratio_"));clock="r.available_at";reason="'native_ratio_missing_history_or_structural_null'"
            if 'char_monthly' in admitted:
                qualified=f"EXISTS(SELECT 1 FROM g_cap_quality cq WHERE cq.security_id=r.security_id AND cq.session_date=r.session_date AND cq.certified AND cq.membership_certified AND cq.currency='USD' AND cq.available_at<{cutoff} AND cq.source_available_at<{cutoff}) AND EXISTS(SELECT 1 FROM g_price_quality pq WHERE pq.security_id=r.security_id AND pq.session_date=r.session_date AND pq.return_certified AND pq.available_at<{cutoff} AND pq.source_available_at<{cutoff})"
        elif source=='fundamentals_notes' and source in admitted and admitted[source]['status']!='excluded':
            alias=ident('note_'+name)
            value=f'CASE WHEN {alias}.n_vintages=1 THEN coalesce({alias}.value,{alias}.diagnostic_value) END'
            clock=alias+'.available_at';reason=f"coalesce({alias}.reason,'missing_or_ambiguous_native_notes_vintage')"
            qualified=f'{alias}.n_vintages=1 AND {alias}.qualified AND {alias}.value IS NOT NULL AND {alias}.period>=u.session_date-INTERVAL \'550 days\''
        elif source in ('short_interest','short_volume_ext') and source in admitted and admitted[source]['status']!='excluded':
            alias='si' if source=='short_interest' else 'sv'
            clock=alias+'.available_at';reason=f"coalesce({alias}.qualification_reason,'missing_or_ambiguous_native_short_vintage')"
            qualified=f'{alias}.n_vintages=1 AND {alias}.qualified'
            if name=='short_volume_ratio':
                value='CASE WHEN sv.n_vintages=1 AND sv.total_volume>0 THEN sv.short_volume/sv.total_volume END'
                qualified+=' AND sv.total_volume>0'
            elif name=='short_interest_ratio':
                if 'char_monthly' in admitted:
                    value='CASE WHEN si.n_vintages=1 AND sden.n_vintages=1 AND sden.shares>0 THEN si.si_shares/sden.shares END'
                    clock='greatest(si.available_at,sden.available_at,sden.source_available_at,sdenq.available_at,sdenq.source_available_at)'
                    qualified+=f" AND sden.n_vintages=1 AND sden.price_certified AND sden.shares>0 AND sden.source_available_at<{cutoff} AND sdenq.certified AND sdenq.membership_certified AND sdenq.currency='USD' AND sdenq.source_available_at<{cutoff}"
                else:reason="'independently admitted dated native shares denominator required'";qualified='false'
            else:value=f'CASE WHEN {alias}.n_vintages=1 THEN {alias}.{ident(contract["source_columns"][0])} END'
        elif source=='text_features' and contract.get('native_name') and source in admitted and admitted[source]['status']!='excluded':
            native_name=contract['native_name'];value='txt.'+ident(native_name);clock='txt.available_at'
            reason="coalesce(txt.reason,'native_text_missing_or_unqualified')"
            qualified="txt.n_vintages=1 AND txt.reason IS NULL AND txt.acceptance_clock IS NOT NULL AND txt.acceptance_clock NOT LIKE '%unresolved%'"
            from . import text_features as TF
            section=next((section for section in TF.SECTIONS if native_name.startswith(section+'_')),None)
            if section:
                qualified+=' AND txt.'+ident(section+'_reason')+' IS NULL'
                if native_name in [section+'_'+metric for metric in TF.SIM_COLS]:
                    qualified+=' AND txt.'+ident(section+'_comparison_reason')+' IS NULL'
                if native_name.endswith('_minedit'):
                    qualified+=' AND txt.'+ident(section+'_edit_reason')+' IS NULL'
        elif source=='classification_tnic' and source in admitted and admitted[source]['status']!='excluded':
            value='tnic.'+ident(contract['native_name']);clock='tnic.available_at'
            reason="coalesce(tnic.reason,'native_tnic_missing_or_unqualified')"
            qualified="tnic.n_vintages=1 AND tnic.reason IS NULL AND tnic.peer_basis='reproduced_tnic3_df25_sic3'"
            if contract['native_name']=='n_fallback_peers':qualified='false'
        elif source=="ea_actuals" and "ea_actuals" in admitted and admitted["ea_actuals"]["status"]!="excluded" and "ann_features.parquet" in admitted["ea_actuals"]["products"]:
            value=f"CASE WHEN ea.n_vintages=1 THEN ea.{ident(contract['source_columns'][0])} END";clock="ea.available_at";reason="'native_earnings_history_or_ambiguity'"
            native_name=contract['source_columns'][0]
            qualified="ea.n_vintages=1 AND ea.clock_reason='qualified' AND ea.currency='USD' AND ea.period_end<=u.session_date"
            if native_name.startswith('sue_'):
                qualified+=' AND ea.'+ident(native_name+'_reason')+"='qualified'"
            elif native_name.startswith('epsfiq'):
                qualified+=' AND ea.eps_split_comparable'
        # Source NULLs do not get a fabricated clock or certification.
        fields.extend([f"{value} AS {ident(name)}", f"CASE WHEN {value} IS NOT NULL THEN {clock} END AS {ident(name+'_source_at')}",
            f"CASE WHEN {value} IS NULL THEN {reason} WHEN coalesce({qualified},false) THEN 'conditional_native_qualified' ELSE 'diagnostic_native_value' END AS {ident(name+'_quality')}",
            f"({value} IS NOT NULL AND coalesce({qualified},false)) AS {ident(name+'_qualified')}"])
    raw = (f"SELECT u.session_date,u.security_id,u.cik,u.cik AS issuer_id,p.member_equity,u.univ_all_common,u.univ_r3000p,u.exchcd_like,{cutoff} AS available_at,"
        + ",".join(fields) + " FROM gu u JOIN gp p USING(session_date,security_id) JOIN gc c USING(session_date) "
        + " ".join(joins) + " WHERE " + where)
    # Normalization is a separate layer over source-qualified raw values. Ties
    # receive their average rank; missing names never enter the reference set.
    scores=[]
    for name,contract in registry.items():
        if contract['kind']!='normalized_score':continue
        field=contract['source_columns'][0];direction=contract['bronze_to_gold']['direction']
        v=ident(field);eligible=f'(univ_all_common AND {ident(field+"_qualified")} AND {v} IS NOT NULL)'
        count=f'count(*) FILTER(WHERE {eligible}) OVER(PARTITION BY session_date)'
        rank=f'rank() OVER(PARTITION BY session_date ORDER BY CASE WHEN {eligible} THEN {v} END NULLS LAST)'
        tied=f'count(*) FILTER(WHERE {eligible}) OVER(PARTITION BY session_date,{v})'
        value=f'CASE WHEN {eligible} AND {count}>=100 THEN {direction}*(2*({rank}+({tied}-1)/2.0-1)/({count}-1)-1) END'
        clock=f'max({ident(field+"_source_at")}) FILTER(WHERE {eligible}) OVER(PARTITION BY session_date)'
        scores.extend([f'{value} AS {ident(name)}',f'CASE WHEN {value} IS NOT NULL THEN {clock} END AS {ident(name+"_source_at")}',
            f'CASE WHEN {value} IS NULL THEN \'missing_qualified_cross_section\' ELSE \'conditional_native_qualified\' END AS {ident(name+"_quality")}',
            f'({value} IS NOT NULL) AS {ident(name+"_qualified")}'])
    return 'SELECT raw.*,'+','.join(scores)+' FROM ('+raw+') raw' if scores else raw


def units(con, start, end):
    dates = [r[0] for r in con.execute("SELECT session_date FROM gc WHERE session_date BETWEEN ? AND ? ORDER BY 1", [start, end]).fetchall()]
    if not dates or dates[0] != start or dates[-1] != end:
        raise ValueError("complete native start/end session window required")
    result = []
    for year in sorted({d.year for d in dates}):
        selected = [d for d in dates if d.year == year]
        result.append((f"daily/year={year}", "daily", selected[0], selected[-1]))
    for (date,) in con.execute("SELECT DISTINCT month_end FROM gc WHERE month_end BETWEEN ? AND ? ORDER BY 1", [start, end]).fetchall():
        result.append((f"monthly/month={date:%Y-%m}", "monthly", date, date))
    return result


@contextmanager
def native_unit(con,kind,start,end,admitted):
    """Native ratios at every source close, then strict-visible source consumption.

This extends immutable monthly products through the producer's unchanged daily
reader formulas. Price-close rows are consumed only after their actual native
publication clock, like other source features; no same-close price backdating.
    """
    row=admitted.get("ratios_v11")
    if not row or row["status"]=="excluded":
        yield;return
    from . import tier1_native_readers as I
    sessions=f"SELECT session_date FROM gc WHERE session_date BETWEEN DATE '{start}'-INTERVAL '7 days' AND DATE '{end}'"
    # admit_plan performs complete native admission once for this transaction;
    # build/replay authenticate all admitted bytes before and after consumption.
    native_sql=I._admitted_ratio_sql(row,sessions,"CAST(session_date AS TIMESTAMP)+INTERVAL 22 HOUR")
    con.execute("CREATE OR REPLACE TEMP VIEW gr AS SELECT * FROM ("+native_sql+")")
    yield


def measure(con, path, expected_sql):
    con.execute(f"CREATE OR REPLACE TEMP VIEW gold_check AS SELECT * FROM {sql([path])}")
    if con.execute('DESCRIBE SELECT * FROM gold_check').fetchall()!=con.execute('DESCRIBE ('+expected_sql+')').fetchall():
        raise ValueError('golden exact typed schema replay failure')
    duplicate = con.execute("SELECT count(*) FROM (SELECT session_date,security_id FROM gold_check GROUP BY 1,2 HAVING count(*)<>1)").fetchone()[0]
    different = con.execute(f"SELECT count(*) FROM ((SELECT * FROM gold_check EXCEPT ALL ({expected_sql})) UNION ALL (({expected_sql}) EXCEPT ALL SELECT * FROM gold_check))").fetchone()[0]
    if duplicate or different:
        raise ValueError("golden exact full replay/key failure")
    coverage = []
    for name in R.registry():
        for population in POPS:
            rows = con.execute(f"SELECT year(session_date),count(*) FILTER(WHERE {population}),count({ident(name)}) FILTER(WHERE {population}) FROM gold_check GROUP BY 1 ORDER BY 1").fetchall()
            coverage.extend({"year": y, "column": name, "universe": population, "denominator": d, "nonnull": n} for y, d, n in rows)
        if con.execute(f"SELECT 1 FROM gold_check WHERE {ident(name)} IS NOT NULL AND ({ident(name+'_source_at')} IS NULL OR {ident(name+'_source_at')}>=available_at) LIMIT 1").fetchone():
            raise ValueError("source clock escaped GC1")
    alignment = con.execute("SELECT count(*) FROM ((SELECT session_date,security_id FROM gold_check WHERE univ_all_common EXCEPT ALL SELECT u.session_date,u.security_id FROM gu u WHERE u.univ_all_common AND u.session_date IN(SELECT DISTINCT session_date FROM gold_check)) UNION ALL (SELECT u.session_date,u.security_id FROM gu u WHERE u.univ_all_common AND u.session_date IN(SELECT DISTINCT session_date FROM gold_check) EXCEPT ALL SELECT session_date,security_id FROM gold_check WHERE univ_all_common))").fetchone()[0]
    if alignment:
        raise ValueError("all_common membership alignment failure")
    return {"rows": pq.ParquetFile(path).metadata.num_rows, "duplicate_keys": duplicate, "replay_differences": different,
            "all_common_differences": alignment, "coverage": coverage}


def build(plan_path, stage, generation_id, max_new_units=None):
    stage = Path(stage).resolve()
    if not re.fullmatch(r"[A-Za-z0-9_-]+", generation_id):
        raise ValueError("invalid generation")
    plan = C.read_json(Path(plan_path))
    with writer_lock(stage):
        con = connect(stage / "_scratch" / generation_id)
        consumed_source_pins=None
        try:
            admitted, exclusions = admit_plan(plan, con)
            bind_views(con, admitted)
            start, end = dt.date.fromisoformat(plan["start"]), dt.date.fromisoformat(plan["end"])
            if start != dt.date(2020, 1, 2) or end!=con.execute('SELECT max(session_date) FROM gu').fetchone()[0]:
                raise ValueError("original full daily start/latest native population window required")
            inventory = units(con, start, end)
            generation = stage / "generations" / generation_id
            generation.mkdir(parents=True, exist_ok=True)
            frozen = {"plan": plan, "plan_path": str(Path(plan_path).resolve()), "plan_pin": S.pin(plan_path), "inventory": [[n,k,str(a),str(b)] for n,k,a,b in inventory], "exclusions": exclusions,"source_pins":S.admitted_pins(admitted)}
            consumed_source_pins=frozen['source_pins']
            snapshot = generation / "snapshot.json"
            if snapshot.exists() and C.read_json(snapshot) != frozen:
                raise ValueError("stale restart snapshot")
            if not snapshot.exists():
                C.write_json_atomic(snapshot, frozen)
            built = 0
            for name, kind, a, b in inventory:
                output, receipt = generation / name / "panel.parquet", generation / name / "receipt.json"
                with native_unit(con,kind,a,b,admitted):
                    expected = query(kind, a, b, admitted)
                    if receipt.exists():
                        saved = C.read_json(receipt)
                        if saved["snapshot_sha256"] != S.digest(frozen) or saved["output"] != S.pin(output):
                            raise ValueError("stale or changed restart unit")
                        if measure(con, output, expected) != saved["measurement"]:
                            raise ValueError("restart evidence changed")
                        continue
                    if max_new_units is not None and built >= max_new_units:
                        return {"status": "continuation", "published": False, "new_units": built}
                    C.copy_to_parquet(con, expected, output, row_group_size=2048)
                    measured = measure(con, output, expected)
                    C.write_json_atomic(receipt, {"snapshot_sha256": S.digest(frozen), "output": S.pin(output), "measurement": measured})
                    built += 1
            # Public replay re-admits complete actual dependencies; no operational
            # success can mask failed economics, labels, prediction or acceptance.
            manifest = {"stage": STAGE, "schema": SCHEMA, "status": "complete", "readiness": "NOT_PRODUCTION_READY",
                "source_quality_certified": False, "snapshot": S.pin(snapshot), "registry": R.registry(),
                "files": {name + "/panel.parquet": S.pin(generation/name/"panel.parquet") for name,_,_,_ in inventory},
                "receipts": {name + "/receipt.json": S.pin(generation/name/"receipt.json") for name,_,_,_ in inventory},
                "input_manifests_sha256": {n: row["spec"]["manifest_pin"]["sha256"] for n,row in admitted.items() if "spec" in row},
                "exclusions": exclusions, "gate_status": {"unique_keys": "PASS", "all_common_alignment": "PASS", "full_replay": "PASS",
                    "source_admission": "FAIL", "required_coverage": "FAIL", "labels": "FAIL", "predictive_5": "FAIL", "predictive_21": "FAIL", "predictive_63": "FAIL", "consumer_acceptance": "FAIL"}}
            C.write_json_atomic(generation / "manifest.json", manifest)
            validate_generation(stage, generation, con)
            current = stage / "current.json"
            pointer = {"generation": generation_id, "manifest": S.pin(generation/"manifest.json")}
            if current.exists():
                C.write_json_atomic(stage/"previous.json", C.read_json(current))
            C.write_json_atomic(current, pointer)
            return {"status": "diagnostic_published", "published": True, "gate_status": manifest["gate_status"]}
        finally:
            try:
                if consumed_source_pins is not None:S.check_pins(consumed_source_pins)
            finally:
                con.close()


def validate_generation(stage, generation, con):
    generation, stage = Path(generation).resolve(), Path(stage).resolve()
    if generation.parent != stage/"generations":
        raise ValueError("foreign golden generation")
    before = S.pin(generation/"manifest.json")
    consumed = S.generation_pins(generation)
    m, frozen = C.read_json(generation/"manifest.json"), C.read_json(generation/"snapshot.json")
    if m.get("schema") != SCHEMA or m.get("stage") != STAGE or m.get("status") != "complete" or m.get("source_quality_certified") is not False:
        raise ValueError("unsupported golden manifest")
    if m["snapshot"] != S.pin(generation/"snapshot.json") or m["registry"] != R.registry():
        raise ValueError("changed registry/snapshot")
    if S.pin(frozen["plan_path"]) != frozen["plan_pin"]:
        raise ValueError("independently reviewed plan changed")
    admitted, exclusions = admit_plan(frozen["plan"], con)
    if frozen.get('source_pins')!=S.admitted_pins(admitted):
        raise ValueError('complete native consumed source inventory changed')
    bind_views(con, admitted)
    if frozen['plan']['start']!='2020-01-02' or dt.date.fromisoformat(frozen['plan']['end'])!=con.execute('SELECT max(session_date) FROM gu').fetchone()[0]:
        raise ValueError('original complete latest native output window changed')
    inventory = units(con, dt.date.fromisoformat(frozen["plan"]["start"]), dt.date.fromisoformat(frozen["plan"]["end"]))
    if frozen["inventory"] != [[n,k,str(a),str(b)] for n,k,a,b in inventory] or exclusions != m["exclusions"]:
        raise ValueError("incomplete source/unit inventory")
    if set(m["files"]) != {n+"/panel.parquet" for n,_,_,_ in inventory} or set(m["receipts"]) != {n+"/receipt.json" for n,_,_,_ in inventory}:
        raise ValueError("incomplete publication inventory")
    sampled = []
    rng = random.Random(16200)
    for name, kind, a, b in inventory:
        path = generation/name/"panel.parquet"
        receipt = generation/name/"receipt.json"
        if S.pin(path) != m["files"][name+"/panel.parquet"] or S.pin(receipt) != m["receipts"][name+"/receipt.json"]:
            raise ValueError("changed unit/receipt")
        row = C.read_json(receipt)
        with native_unit(con,kind,a,b,admitted):
            measured = measure(con, path, query(kind, a, b, admitted))
        if row != {"snapshot_sha256": S.digest(frozen), "output": S.pin(path), "measurement": measured}:
            raise ValueError("public evidence replay differs")
        # Full EXCEPT ALL above audits every cell, stronger than 200 samples.
        # Deterministic reservoir records the requested 200 cell coordinates.
        keys = con.execute("SELECT session_date,security_id FROM gold_check ORDER BY hash(session_date,security_id,16200) LIMIT 200").fetchall()
        for d, sid in keys:
            coordinate = [kind, str(d), sid, rng.choice(list(R.registry()))]
            if len(sampled) < 200:
                sampled.append(coordinate)
            else:
                j = rng.randrange(measured["rows"] + 200)
                if j < 200:
                    sampled[j] = coordinate
    again,_=admit_plan(frozen["plan"], con)
    if frozen['source_pins']!=S.admitted_pins(again):
        raise ValueError('native consumed source inventory changed after replay')
    S.check_pins(consumed)
    if S.pin(generation/"manifest.json") != before:
        raise ValueError("golden manifest changed during replay")
    return {"status": "valid_diagnostic", "pit_sample": sampled, "all_cells_replayed": True, "gate_status": m["gate_status"]}


def rollback(stage):
    stage = Path(stage)
    with writer_lock(stage):
        pointer = C.read_json(stage/"previous.json")
        generation = S.member(stage/"generations", pointer["generation"])
        if S.pin(generation/"manifest.json") != pointer["manifest"]:
            raise ValueError("stale rollback target")
        con = connect(stage/"_scratch"/"rollback")
        try:
            validate_generation(stage, generation, con)
            old = C.read_json(stage/"current.json")
            C.write_json_atomic(stage/"current.json", pointer)
            C.write_json_atomic(stage/"previous.json", old)
        finally:
            con.close()


def load_golden(start, end, columns, universe="univ_all_common", *, stage=None, kind="daily", batch_size=2048):
    """Bounded iterator; fully authenticate/replay immutable candidate before yield."""
    stage = Path(stage or C.build_root()/STAGE)
    if universe not in POPS or kind not in ("daily", "monthly") or not 1 <= batch_size <= 8192:
        raise ValueError("invalid bounded reader selection")
    if not columns or len(set(columns)) != len(columns) or any(n not in R.registry() and n not in AXES for n in columns):
        raise ValueError("unknown/duplicate/empty columns")
    start, end = dt.date.fromisoformat(str(start)), dt.date.fromisoformat(str(end))
    if start > end:
        raise ValueError("reversed scan")
    pointer = C.read_json(stage/"current.json")
    generation = S.member(stage/"generations", pointer["generation"])
    con = connect(stage/"_scratch"/"load")
    try:
        if S.pin(generation/"manifest.json") != pointer["manifest"]:
            raise ValueError("changed consumer pointer")
        validate_generation(stage, generation, con)
        consumed=S.generation_pins(generation)
        m = C.read_json(generation/"manifest.json")
        selected = [generation/n for n in m["files"] if n.startswith(kind+"/")]
        cur = con.execute(f"SELECT {','.join(ident(n) for n in columns)} FROM {sql(selected)} WHERE session_date BETWEEN ? AND ? AND {universe} ORDER BY session_date,security_id", [start, end])
        while rows := cur.fetchmany(batch_size):
            yield rows
        if S.pin(generation/"manifest.json") != pointer["manifest"]:
            raise ValueError("generation changed during bounded scan")
        S.check_pins(consumed)
    finally:
        try:
            if 'consumed' in locals():S.check_pins(consumed)
        finally:con.close()


def main(argv=None):
    p = argparse.ArgumentParser(__doc__)
    p.add_argument("plan", type=Path); p.add_argument("stage", type=Path); p.add_argument("generation")
    p.add_argument("--max-new-units", type=int)
    args = p.parse_args(argv)
    print(json.dumps(build(args.plan, args.stage, args.generation, args.max_new_units)))


if __name__ == "__main__":
    main()
