"""Forward-return labels for the gold alpha panels (measurement only; never a feature input).

Convention (Global Constraint 3, identical endpoints to the consumer's ``close[d+1+h]/close[d+1]-1``): decide after
the close of session ``d``, trade ``d+1``, earn from the close of ``d+1``::

    fwd_h(d) = sum_{k=2..h+1} ln(1 + r[d+k])       h in {1, 5, 21, 63}

over *calendar* sessions (``calendar.parquet``), ``r`` = panel ``ret`` with ``ret_guarded`` rows (and ``ret <= -1``)
treated as missing. ``n_h`` counts the non-missing returns in the window. A label is NULL when fewer than
``ceil(0.9 h)`` returns exist, except for a line that delists inside the window: its last vendor session ``L``
satisfies ``d+1 <= L <= d+h``; the window then stops at ``L``, adds ``ln(1 + dlret)`` for the next slot when the
``delisting/`` stage carries an imputed delisting return for the line (``dlret`` not NULL; NULL for an unknown
cause: nothing is added, CRSP cash convention, return 0 afterwards) and is complete whatever its length
(NULL only when it holds no return at all). Lines ``continued`` under another security id are not terminal and
follow the ordinary rule. ``n_h`` includes the delisting return when added.

**Holdout (D6 as amended by R1):** a label is written only when session ``d+1+h <= LABEL_CUTOFF`` (2023-12-29, the
last 2023 session); the panel is never scanned past the cutoff (:func:`scan_panel` refuses) and the build asserts
the largest label-end session before publishing each year.

Output ``labels/year=YYYY/labels.parquet``: ``session_date, security_id, fwd_1, fwd_5, fwd_21, fwd_63, n_5, n_21,
n_63`` for decision sessions ``LABEL_START`` (2019-01-02) onward, one row per panel line-session with at least one
label. Resumable per year (an existing year file is kept unless ``--force``).
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
import time
from typing import Any

import duckdb

from . import common as C

LABEL_CUTOFF = dt.date(2023, 12, 29)
LABEL_START = dt.date(2019, 1, 2)
HORIZONS = (1, 5, 21, 63)
SCHEMA = "atx.alpha-panel.labels/v1"
STAGE = "labels"


class HoldoutViolation(RuntimeError):
    """A read or a label that would touch a return realized after ``LABEL_CUTOFF``."""


def min_count(h: int) -> int:
    """``ceil(0.9 h)`` in integers (float 0.9*10 rounds above 9)."""
    return -(-9 * h // 10)


def scan_panel(con: duckdb.DuckDBPyConnection, files: list[str], lo: dt.date, hi: dt.date, table: str = "w") -> int:
    """Load per-line log returns ``(session_date, idx, security_id, lr, lidx, dlret)`` for ``lo..hi`` into ``table``.

    Needs temp tables ``cal(session_date, idx)`` and ``ev(security_id, lidx, dlret)``. Refuses (before reading) any
    ``hi`` after ``LABEL_CUTOFF`` and asserts the loaded maximum afterwards."""
    if hi > LABEL_CUTOFF:
        raise HoldoutViolation(f"panel scan to {hi} is past the label cutoff {LABEL_CUTOFF}")
    if not files:
        raise FileNotFoundError("no panel files to scan")
    flist = "[" + ", ".join(f"'{f}'" for f in files) + "]"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {table} AS
        SELECT p.session_date, c.idx, p.security_id,
               CASE WHEN coalesce(p.ret_guarded, false) OR p.ret IS NULL OR NOT isfinite(p.ret) OR p.ret <= -1 THEN NULL
                    WHEN e.lidx IS NOT NULL AND c.idx > e.lidx THEN NULL
                    ELSE ln(1 + p.ret) END AS lr,
               e.lidx, e.dlret
        FROM read_parquet({flist}, hive_partitioning = false) p
        JOIN cal c USING (session_date)
        LEFT JOIN ev e USING (security_id)
        WHERE p.session_date BETWEEN DATE '{lo}' AND DATE '{hi}'
    """)
    top = con.execute(f"SELECT max(session_date) FROM {table}").fetchone()[0]
    if top is not None and top > LABEL_CUTOFF:
        raise HoldoutViolation(f"panel rows dated {top} were read (cutoff {LABEL_CUTOFF})")
    return int(con.execute(f"SELECT count(*) FROM {table}").fetchone()[0])


def _label_sql(cut_idx: int, lo_idx: int, hi_idx: int) -> str:
    wins = []
    for h in HORIZONS:
        fr = f"OVER (PARTITION BY security_id ORDER BY idx RANGE BETWEEN 2 FOLLOWING AND {h + 1} FOLLOWING)"
        wins.append(f"sum(lr) {fr} AS s{h}, count(lr) {fr} AS c{h}")
    outer = []
    for h in HORIZONS:
        trunc = f"(lidx IS NOT NULL AND lidx >= idx + 1 AND lidx <= idx + {h})"
        dl = f"({trunc} AND dlret IS NOT NULL)"
        n = f"(coalesce(c{h}, 0) + CASE WHEN {dl} THEN 1 ELSE 0 END)"
        tot = f"(coalesce(s{h}, 0) + CASE WHEN {dl} THEN ln(1 + dlret) ELSE 0 END)"
        outer.append(f"""CASE WHEN idx + 1 + {h} > {cut_idx} THEN NULL
                             WHEN {trunc} THEN CASE WHEN {n} > 0 THEN {tot} END
                             WHEN coalesce(c{h}, 0) >= {min_count(h)} THEN s{h} END AS fwd_{h}""")
        if h != 1:
            outer.append(f"CASE WHEN idx + 1 + {h} > {cut_idx} THEN NULL ELSE {n} END AS n_{h}")
    return f"""
        WITH x AS (SELECT session_date, idx, security_id, lidx, dlret, {', '.join(wins)} FROM w)
        SELECT session_date, security_id, {', '.join(outer)} FROM x
        WHERE idx BETWEEN {lo_idx} AND {hi_idx}
    """


def build(memory: str = "300MB", threads: int = 2, force: bool = False, years: list[int] | None = None) -> dict[str, Any]:
    """Build the labels stage (all decision years, or ``years``) and publish the stage manifest."""
    root = C.build_root()
    con = C.connect(memory=memory, threads=threads, db_file="labels.duckdb")
    cal = C.load_calendar(con)
    if LABEL_CUTOFF not in cal:
        raise HoldoutViolation(f"label cutoff {LABEL_CUTOFF} is not a calendar session")
    idx_of = {d: i for i, d in enumerate(cal)}
    cut_idx = idx_of[LABEL_CUTOFF]
    last_dec = cut_idx - 2  # fwd_1 ends at d+2
    con.execute("CREATE TEMP TABLE cal (session_date DATE, idx BIGINT)")
    con.executemany("INSERT INTO cal VALUES (?, ?)", [(d, i) for i, d in enumerate(cal)])
    ev_path = root / "delisting" / "events.parquet"
    if ev_path.exists():
        con.execute(f"""CREATE TEMP TABLE ev AS
            SELECT e.security_id, c.idx AS lidx, CAST(e.dlret AS DOUBLE) AS dlret
            FROM read_parquet('{ev_path.as_posix()}') e
            JOIN cal c ON c.session_date = e.last_session WHERE NOT coalesce(e.continued, false)""")
    else:
        con.execute("CREATE TEMP TABLE ev (security_id BIGINT, lidx BIGINT, dlret DOUBLE)")
    dec = [(i, d) for i, d in enumerate(cal) if d >= LABEL_START and i <= last_dec]
    out = C.stage_dir(STAGE)
    receipt: dict[str, Any] = {"rule": "forward-log-return-d+2..d+1+h", "cutoff": str(LABEL_CUTOFF),
                               "start": str(LABEL_START), "rows_per_year": {}, "timings_s": {},
                               "delisting_events_used": ev_path.exists()}
    for year in sorted({d.year for _i, d in dec}):
        if years and year not in years:
            continue
        dest = out / f"year={year}" / "labels.parquet"
        if dest.exists() and not force:
            receipt["rows_per_year"][str(year)] = con.execute(
                f"SELECT count(*) FROM read_parquet('{dest.as_posix()}')").fetchone()[0]
            continue
        yi = [i for i, d in dec if d.year == year]
        lo_i, hi_i = yi[0], yi[-1]
        scan_hi = min(hi_i + 1 + max(HORIZONS), cut_idx)
        t0 = time.perf_counter()
        files = [(root / "panel" / f"year={y}" / "*.parquet").as_posix()
                 for y in range(cal[lo_i].year, cal[scan_hi].year + 1) if (root / "panel" / f"year={y}").is_dir()]
        scan_panel(con, files, cal[lo_i], cal[scan_hi])
        con.execute(f"CREATE OR REPLACE TEMP TABLE y AS {_label_sql(cut_idx, lo_i, hi_i)}")
        con.execute("DROP TABLE w")
        for h in HORIZONS:
            d_max = con.execute(f"SELECT max(session_date) FROM y WHERE fwd_{h} IS NOT NULL").fetchone()[0]
            if d_max is not None and cal[idx_of[d_max] + 1 + h] > LABEL_CUTOFF:
                raise HoldoutViolation(f"year {year}: a fwd_{h} label ends {cal[idx_of[d_max] + 1 + h]} > {LABEL_CUTOFF}")
        n = C.copy_to_parquet(con, """SELECT CAST(session_date AS DATE) AS session_date, CAST(security_id AS BIGINT) AS security_id,
                fwd_1, fwd_5, fwd_21, fwd_63, CAST(n_5 AS SMALLINT) AS n_5, CAST(n_21 AS SMALLINT) AS n_21,
                CAST(n_63 AS SMALLINT) AS n_63 FROM y
            WHERE fwd_1 IS NOT NULL OR fwd_5 IS NOT NULL OR fwd_21 IS NOT NULL OR fwd_63 IS NOT NULL
            ORDER BY session_date, security_id""", dest, row_group_size=65536)
        con.execute("DROP TABLE y")
        receipt["rows_per_year"][str(year)] = n
        receipt["timings_s"][str(year)] = round(time.perf_counter() - t0, 1)
        print(f"labels {year}: {n} rows in {receipt['timings_s'][str(year)]} s", flush=True)
    # re-derive the end-session bound over everything on disk (covers resumed years)
    files_all = (out / "year=*" / "labels.parquet").as_posix()
    disk_end = 0
    for h in HORIZONS:
        m = con.execute(f"""SELECT max(c.idx) FROM read_parquet('{files_all}') l JOIN cal c USING (session_date)
                            WHERE l.fwd_{h} IS NOT NULL""").fetchone()[0]
        if m is not None:
            disk_end = max(disk_end, m + 1 + h)
    con.close()
    (root / "_tmp" / "labels.duckdb").unlink(missing_ok=True)
    if disk_end > cut_idx:
        raise HoldoutViolation(f"labels on disk end at {cal[disk_end]} > {LABEL_CUTOFF}")
    receipt["max_label_end"] = str(cal[disk_end]) if disk_end else None
    receipt["rows_total"] = sum(receipt["rows_per_year"].values())
    inputs = {rel: C.sha256_file(root / rel) for rel in ("delisting/manifest.json", "panel/manifest.json")
              if (root / rel).exists()}
    C.write_stage_manifest(STAGE, SCHEMA, ("labels", "common"), {
        "rule_text": __doc__, "input_manifests_sha256": inputs, "max_label_end": receipt["max_label_end"],
        "receipt": receipt})
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--years", type=int, nargs="*")
    ap.add_argument("--memory", default="300MB")
    a = ap.parse_args(argv)
    r = build(memory=a.memory, force=a.force, years=a.years)
    print({k: v for k, v in r.items() if k != "timings_s"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
