"""Separate 5/21/63 labels; computational kernels do not admit native sources.

Native return/proceeds admission is a prerequisite, never supplied by renaming
a self-asserted Parquet file. Every full window remains strictly pre2023.
"""
from __future__ import annotations
import datetime as dt
import math
from pathlib import Path
from . import common as C
from .golden_sources import digest, pin

SEAL = dt.date(2023, 1, 1)
HORIZONS = (5, 21, 63)
SCHEMA = "atx.golden-labels/v1"


def guard(feature, entry, exit, observations=()):
    if not (feature < entry <= exit < SEAL) or feature < dt.date(2020, 1, 1):
        raise ValueError("complete forward window violates pre2023 seal")
    if any(d >= SEAL or d < entry or d > exit for d in observations):
        raise ValueError("contributing return observation violates full holding window")


def label_window(calendar, feature_index, horizon, observations):
    """Next-close entry, h subsequent close-to-close total return observations.

observations indexed by session contain ret, source_qualified, delisted,
dlret, dlret_qualified and residual_cash_qualified. A delisting needs actual
proceeds; absent proceeds never become zero. After a qualified cash conversion
the cash return itself must still be independently sourced for each session.
    """
    if horizon not in HORIZONS or feature_index < 0:
        raise ValueError("unsupported label horizon/index")
    end_index = feature_index + horizon + 1
    if end_index >= len(calendar):
        return {"value": None, "reason": "incomplete_calendar_window", "horizon": horizon}
    feature, entry, exit = calendar[feature_index], calendar[feature_index+1], calendar[end_index]
    dates = calendar[feature_index+2:end_index+1]
    guard(feature, entry, exit, dates)
    wealth = 1.0
    cash = False
    for date in dates:
        row = observations.get(date)
        if not row or row.get("source_qualified") is not True:
            return {"value": None, "reason": "missing_or_unqualified_total_return", "horizon": horizon,
                    "feature_date": feature, "entry_date": entry, "exit_date": exit}
        ret = row.get("cash_return") if cash else row.get("ret")
        if ret is None or not math.isfinite(ret) or ret < -1:
            return {"value": None, "reason": "missing_cash_return" if cash else "missing_total_return", "horizon": horizon,
                    "feature_date": feature, "entry_date": entry, "exit_date": exit}
        if cash and row.get("cash_return_qualified") is not True:
            return {"value": None, "reason": "residual_cash_basis_unqualified", "horizon": horizon,
                    "feature_date": feature, "entry_date": entry, "exit_date": exit}
        wealth *= 1 + ret
        if row.get('cash_conversion'):
            if cash:
                raise ValueError('duplicate native cash conversion')
            # Native proceeds already replaced the prior raw price in ret.
            # This flag changes state only; it never adds a second DLRET.
            if date != exit and row.get('residual_cash_qualified') is not True:
                return {'value': None, 'reason': 'residual_cash_convention_pending', 'horizon': horizon,
                        'feature_date': feature, 'entry_date': entry, 'exit_date': exit}
            cash = True
        if row.get("delisted"):
            dlret = row.get("dlret")
            if cash or row.get("dlret_qualified") is not True or dlret is None or not math.isfinite(dlret) or dlret < -1:
                return {"value": None, "reason": "missing_or_unqualified_delisting_proceeds", "horizon": horizon,
                        "feature_date": feature, "entry_date": entry, "exit_date": exit}
            wealth *= 1 + dlret
            if date != exit and row.get("residual_cash_qualified") is not True:
                return {"value": None, "reason": "residual_cash_convention_pending", "horizon": horizon,
                        "feature_date": feature, "entry_date": entry, "exit_date": exit}
            cash = True
    return {"value": wealth-1, "reason": "qualified_complete_window", "horizon": horizon,
            "feature_date": feature, "entry_date": entry, "exit_date": exit,
            "observations": horizon, "delisting_adjusted": cash}


def iter_labels(calendar, securities, read_observations):
    """Bound one security/year slice; callback is admitted native reader, not IO discovery."""
    if calendar != sorted(set(calendar)):
        raise ValueError("nonunique/nonincreasing session calendar")
    for sid in securities:
        for year in (2020, 2021, 2022):
            indices = [i for i, d in enumerate(calendar) if d.year == year]
            if not indices:
                continue
            selected_end = min(len(calendar)-1, indices[-1]+64)
            observations = read_observations(sid, calendar[indices[0]], calendar[selected_end])
            for i in indices:
                for h in HORIZONS:
                    end = i+h+1
                    if end < len(calendar) and calendar[end] >= SEAL:
                        yield {"security_id": sid, "feature_date": calendar[i], "horizon": h, "value": None,
                               "reason": "window_crosses_pre2023_seal"}
                    else:
                        yield {"security_id": sid, **label_window(calendar, i, h, observations)}


def require_native_returns(root,admission,con):
    from .golden_returns import NativeWindows
    return NativeWindows(root,admission,con)


def publish_exclusions(output, source_dependencies):
    """A label manifest reports unavailable windows without manufacturing labels."""
    output = Path(output)
    m = {"schema": SCHEMA, "status": "excluded", "physically_separate_from_features": True,
         "horizons": list(HORIZONS), "end_exclusive": str(SEAL), "files": {},
         "source_dependencies": source_dependencies, "gate_status": "FAIL",
         "reason": "native complete holding-window source/proceeds/cash admission pending"}
    if output.exists() and C.read_json(output) != m:
        raise ValueError("immutable label exclusion changed")
    if not output.exists():
        C.write_json_atomic(output, m)
    return m


def diagnostic_query(year):
    """Complete native denominators, physical NULL labels and keyed exclusions.

No source-only individual DLRET or legacy panel ret is promoted to a qualified
forward label. Window metadata is absent for prohibited complete windows.
    """
    return f"""WITH sessions AS (SELECT session_date,row_number() OVER(ORDER BY session_date) i FROM gc),
    horizons AS (SELECT unnest([5,21,63])::INTEGER horizon)
    SELECT u.session_date feature_date,u.security_id,h.horizon,
      CASE WHEN x.session_date<DATE '2023-01-01' THEN e.session_date END entry_date,
      CASE WHEN x.session_date<DATE '2023-01-01' THEN x.session_date END exit_date,
      NULL::DOUBLE AS value,false label_qualified,
      CASE WHEN x.session_date IS NULL THEN 'incomplete_calendar_window'
           WHEN x.session_date>=DATE '2023-01-01' THEN 'window_crosses_pre2023_seal'
           ELSE 'native_complete_holding_window_source_admission_pending' END reason,
      u.univ_all_common,u.univ_r3000p,NULL::TIMESTAMPTZ AS available_at
    FROM gu u JOIN sessions f USING(session_date) CROSS JOIN horizons h
    LEFT JOIN sessions e ON e.i=f.i+1 LEFT JOIN sessions x ON x.i=f.i+h.horizon+1
    WHERE year(u.session_date)={int(year)} AND u.session_date>=DATE '2020-01-01' AND u.session_date<DATE '2023-01-01'"""


def build_labels(golden_stage,golden_generation,output_stage,generation_id,*,returns_binding=None):
    """Conditional full native label publication; missing original proofs stay NULL."""
    from . import golden_panel as P, golden_sources as S
    golden_stage,output_stage=Path(golden_stage).resolve(),Path(output_stage).resolve()
    golden=S.member(golden_stage/"generations",golden_generation)
    if not generation_id or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in generation_id):
        raise ValueError("invalid label generation")
    with P.writer_lock(output_stage):
        con=P.connect(output_stage/"_scratch"/generation_id)
        try:
            P.validate_generation(golden_stage,golden,con)
            frozen={"golden_stage":str(golden_stage),"golden_generation":golden_generation,"golden_manifest":pin(golden/"manifest.json"),
                    "code":S.code_pins(),"horizons":list(HORIZONS),"returns_binding":returns_binding}
            windows=require_native_returns(returns_binding["lake_root"],returns_binding,con) if returns_binding else None
            generation=output_stage/"generations"/generation_id
            generation.mkdir(parents=True,exist_ok=True)
            if (generation/"snapshot.json").exists() and C.read_json(generation/"snapshot.json")!=frozen:
                raise ValueError("stale label restart")
            C.write_json_atomic(generation/"snapshot.json",frozen)
            files,coverage={},{}
            for year in (2020,2021,2022):
                path=generation/f"year={year}/labels.parquet"
                query=diagnostic_query(year)
                replay=output_stage/f"_scratch/{generation_id}/year={year}/labels.parquet" if windows else None
                if windows:
                    write_native_year(con,query,replay,windows)
                    query=f"SELECT * FROM {P.sql([replay])}"
                if path.exists():
                    difference=con.execute(f"SELECT count(*) FROM ((SELECT * FROM {P.sql([path])} EXCEPT ALL ({query})) UNION ALL (({query}) EXCEPT ALL SELECT * FROM {P.sql([path])}))").fetchone()[0]
                    if difference:raise ValueError("changed label restart rows")
                else:
                    C.copy_to_parquet(con,query,path,row_group_size=2048)
                files[path.relative_to(generation).as_posix()]=pin(path)
                for population in ("univ_all_common","univ_r3000p"):
                    rows=con.execute(f"SELECT horizon,reason,count(*),count(value) FROM {P.sql([path])} WHERE {population} GROUP BY 1,2 ORDER BY 1,2").fetchall()
                    coverage[f"{year}:{population}"]=[{"horizon":h,"reason":r,"denominator":n,"qualified_labels":q} for h,r,n,q in rows]
            m={"schema":SCHEMA,"stage":"golden_labels","status":"complete_diagnostic","physically_separate_from_features":True,
               "snapshot":pin(generation/"snapshot.json"),"files":files,"coverage":coverage,"gate_status":"FAIL",
               "source_quality_certified":False,"end_exclusive":str(SEAL),"native_conditional_windows":bool(windows)}
            C.write_json_atomic(generation/"manifest.json",m)
            validate_generation(output_stage,generation,con)
            C.write_json_atomic(output_stage/"current.json",{"generation":generation_id,"manifest":pin(generation/"manifest.json")})
            return m
        finally:con.close()


def validate_generation(stage,generation,con):
    from . import golden_panel as P, golden_sources as S
    generation,stage=Path(generation).resolve(),Path(stage).resolve()
    if generation.parent!=stage/"generations":raise ValueError("foreign label generation")
    before=pin(generation/"manifest.json");m=C.read_json(generation/"manifest.json")
    frozen=C.read_json(generation/"snapshot.json")
    if m.get("schema")!=SCHEMA or m.get("status")!="complete_diagnostic" or m.get("gate_status")!="FAIL" or m.get("physically_separate_from_features") is not True:
        raise ValueError("unsupported label certification")
    if m["snapshot"]!=pin(generation/"snapshot.json") or frozen["code"]!=S.code_pins():raise ValueError("stale label source/code snapshot")
    consumed=S.generation_pins(generation)
    golden=S.member(Path(frozen["golden_stage"])/"generations",frozen["golden_generation"])
    if frozen["golden_manifest"]!=pin(golden/"manifest.json"):raise ValueError("changed golden label dependency")
    P.validate_generation(Path(frozen["golden_stage"]),golden,con)
    windows=require_native_returns(frozen["returns_binding"]["lake_root"],frozen["returns_binding"],con) if frozen.get("returns_binding") else None
    if set(m["files"])!={f"year={y}/labels.parquet" for y in (2020,2021,2022)}:raise ValueError("incomplete label inventory")
    for year in (2020,2021,2022):
        path=generation/f"year={year}/labels.parquet"
        if pin(path)!=m["files"][path.relative_to(generation).as_posix()]:raise ValueError("changed label file")
        query=diagnostic_query(year)
        if windows:
            # Private source reconstruction: comparison includes every label,
            # NULL, reason and full universe denominator, not only passing rows.
            replay=stage/"_scratch"/"public-replay"/f"year={year}/labels.parquet"
            write_native_year(con,query,replay,windows)
            query=f"SELECT * FROM {P.sql([replay])}"
        difference=con.execute(f"SELECT count(*) FROM ((SELECT * FROM {P.sql([path])} EXCEPT ALL ({query})) UNION ALL (({query}) EXCEPT ALL SELECT * FROM {P.sql([path])}))").fetchone()[0]
        if difference:raise ValueError("complete native label denominator replay differs")
        for population in ('univ_all_common','univ_r3000p'):
            rows=con.execute(f'SELECT horizon,reason,count(*),count(value) FROM {P.sql([path])} WHERE {population} GROUP BY 1,2 ORDER BY 1,2').fetchall()
            expected=[{'horizon':h,'reason':r,'denominator':n,'qualified_labels':q} for h,r,n,q in rows]
            if m['coverage'].get(f'{year}:{population}')!=expected:raise ValueError('label coverage evidence differs from full population')
    if pin(generation/"manifest.json")!=before:raise ValueError("label changed during validation")
    if windows:windows.recheck()
    S.check_pins(consumed)
    return m


def write_native_year(con,axes_query,path,windows):
    """Stream bounded axes batches; preserve every unsuccessful candidate/window."""
    import os
    import pyarrow as pa,pyarrow.parquet as pq
    calendar=[r[0] for r in con.execute("SELECT session_date FROM gc ORDER BY 1").fetchall()]
    indices={d:i for i,d in enumerate(calendar)}
    cursor=con.cursor()
    try:
        # A persistent scratch relation is visible to the read cursor while
        # native source queries run on the parent connection. DuckDB TEMP views
        # are connection-local and cannot safely back a separate cursor.
        con.execute("CREATE OR REPLACE TABLE golden_label_work_axes AS "+axes_query)
        cursor.execute("SELECT * FROM golden_label_work_axes ORDER BY feature_date,security_id,horizon")
        names=[r[0] for r in cursor.description]
        schema=pa.schema([("feature_date",pa.date32()),("security_id",pa.int64()),("horizon",pa.int32()),
            ("entry_date",pa.date32()),("exit_date",pa.date32()),("value",pa.float64()),("label_qualified",pa.bool_()),
            ("reason",pa.string()),("univ_all_common",pa.bool_()),("univ_r3000p",pa.bool_()),
            ("available_at",pa.timestamp('us',tz='UTC')),
            ("proof_sha256",pa.string()),("source_contract_sha256",pa.string())])
        path=Path(path);path.parent.mkdir(parents=True,exist_ok=True);temporary=path.with_suffix(".partial")
        with pq.ParquetWriter(temporary,schema,compression="zstd") as writer:
            while batch:=cursor.fetchmany(32):
                rows=[]
                for raw in batch:
                    row=dict(zip(names,raw))
                    result=windows.window(calendar,indices[row["feature_date"]],row["security_id"],row["horizon"])
                    row.update(value=result.get("value"),reason=result["reason"],label_qualified=result.get("value") is not None,
                               available_at=result.get('available_at'),
                               proof_sha256=result.get("proof_sha256"),source_contract_sha256=result.get("source_contract_sha256"))
                    rows.append(row)
                writer.write_table(pa.Table.from_pylist(rows,schema=schema))
        windows.recheck();os.replace(temporary,path)
    finally:
        try:
            # Authenticate the whole bounded consumption, including failure or
            # early termination; source-sized hashing is never per window.
            windows.recheck()
        finally:
            cursor.close()
