"""Bounded P9 A0 arm differences; diagnostic counts, never release admission.

Each caller must first finish D0, authenticate the complete same-code arm
receipts and sanity checks, and freeze the exact original calendar/membership.
This kernel handles the requested consumer view and identifiers, not those
upstream obligations. It reads no prices, returns, labels or source bodies.
"""
from __future__ import annotations

from contextlib import contextmanager
import copy
from dataclasses import dataclass
import datetime as dt
import json
from pathlib import Path
import re
import uuid

from .private_identity_differential import _digest, _ident, _literal

VERSION = 'p9-a0-arm-comparison-v1'
CUTOFF = '2024-01-01'
LAST_SESSION = '2023-12-29'
PAIRS = {('B1', 'A'), ('B2', 'A'), ('B12', 'A'), ('B12', 'B1')}


@dataclass(frozen=True)
class Window:
    """Consumer-view window of one comparison; the defaults are the request's original pre-2024 window.

    first_session None starts at the bridge's first start (original); a date starts the session axis there and
    keeps rows 8/9 to rows overlapping it. Rows 8/9 split at cutoff. Every session year needs a row 7 bucket.
    observed_at (a removed source's observation clock) also splits rows 1-4 and 7 by session mark d 22:00:
    before it a difference is a look-ahead; at or after it the source was public.
    """
    first_session: dt.date | None = None
    last_session: dt.date = dt.date.fromisoformat(LAST_SESSION)
    cutoff: dt.date = dt.date.fromisoformat(CUTOFF)
    row_7_buckets: tuple[tuple[str, int, int], ...] = (
        ('2017-2019', 2017, 2019), ('2020', 2020, 2020), ('2021', 2021, 2021), ('2022', 2022, 2022),
        ('2023', 2023, 2023))
    panel_from_year: int = 2018
    observed_at: dt.datetime | None = None

    def describe(self, start):
        return {'first_session': self.first_session and self.first_session.isoformat(),
                'start_session': start.isoformat(), 'last_session': self.last_session.isoformat(),
                'cutoff': self.cutoff.isoformat(), 'row_7_buckets': [list(b) for b in self.row_7_buckets],
                'panel_from_year': self.panel_from_year,
                'observed_at': self.observed_at and self.observed_at.isoformat(sep=' ')}


def _path(value):
    path = Path(value)
    if '..' in path.parts:
        raise ValueError('Diagnostic parent traversal refused')
    path = path.absolute()
    for item in (path, *path.parents):
        if item.is_symlink() or item.is_junction():
            raise ValueError('Diagnostic reparse paths refused')
    return path.resolve(strict=False)


def _resources(con):
    settings = dict(con.execute("SELECT name,value FROM duckdb_settings() WHERE name IN "
        "('threads','TimeZone','memory_limit','temp_directory')").fetchall())
    units = {'B': 1, 'KiB': 1024, 'MiB': 1024**2, 'GiB': 1024**3, 'TiB': 1024**4}
    match = re.fullmatch(r'(\d+(?:\.\d+)?)\s+(B|KiB|MiB|GiB|TiB)', settings['memory_limit'])
    databases = [row[2] for row in con.execute('PRAGMA database_list').fetchall() if row[2]]
    if (settings['threads'] != '1' or settings['TimeZone'] != 'UTC' or not match or
            float(match[1]) * units[match[2]] > 128 * 1024**2 or
            not settings['temp_directory'] or len(databases) != 1):
        raise ValueError('One file-backed 128MiB/one-thread/UTC database with spill required')
    return _path(databases[0]).parent


@contextmanager
def _inputs(pins):
    frozen = copy.deepcopy(pins)
    def check():
        for name, expected in frozen.items():
            if not re.fullmatch('[0-9a-f]{64}', expected) or _digest(_path(name)) != expected:
                raise ValueError('Diagnostic original input pin changed')
    check()
    try:
        yield
    finally:
        check()


def _schema(con, relation):
    return [(row[0], row[1]) for row in con.execute(f'DESCRIBE SELECT * FROM {relation}').fetchall()]


def _require(con, relation, columns):
    actual = dict(_schema(con, relation))
    for name, kinds in columns.items():
        if name not in actual or actual[name] not in kinds:
            raise ValueError('Native diagnostic schema differs: ' + name)


def _count(con, query):
    return con.execute('SELECT count(*) FROM (' + query + ') counted').fetchone()[0]


def _parquet(files):
    return 'read_parquet([' + ','.join(_literal(_path(p).as_posix()) for p in files) + '], hive_partitioning=false)'


def _state_sql(pit, calendar, year, last=LAST_SESSION, first=None):
    # Sparse states are sufficient: missing keys have zero qualifying CIKs.
    # No full security x session cross product or whole-panel Python object.
    lower = f" AND c.session_date>=DATE '{first}'" if first else ''
    return f'''SELECT p.sr_id,c.session_date, count(DISTINCT p.cik)::BIGINT n_ciks,
        list_sort(list(DISTINCT p.cik)) ciks,
        CASE WHEN count(DISTINCT p.cik)=1 THEN min(p.cik) END cik,
        bool_or(p."primary"='P') has_p,
        list_sort(list(DISTINCT coalesce(p.class_status,''))) classes,
        count(DISTINCT p.cik)=1 AND bool_or(p."primary"='P') AND
          bool_and(coalesce(p.class_status='common' OR
            (regexp_full_match(p.class_status,'[A-Z]') AND p.class_status NOT IN ('U','W','R')),false)) eligible
      FROM {pit} p JOIN {calendar} c ON c.session_date BETWEEN p.start AND coalesce(p.end_incl,DATE '9999-12-31')
      WHERE year(c.session_date)={year} AND c.session_date<=DATE '{last}'{lower}
        AND p.available_at<=CAST(c.session_date AS TIMESTAMP)+INTERVAL 22 HOUR
        AND p."primary" IN ('P','J') AND coalesce(p.basis,'')<>'current_ticker_verified'
      GROUP BY p.sr_id,c.session_date'''


def _pair_sql(a, b):
    return f'''SELECT coalesce(a.sr_id,b.sr_id) sr_id,coalesce(a.session_date,b.session_date) session_date,
       coalesce(a.n_ciks,0) a_n,coalesce(b.n_ciks,0) b_n,a.cik a_cik,b.cik b_cik,
       a.ciks a_ciks,b.ciks b_ciks,coalesce(a.has_p,false) a_p,coalesce(b.has_p,false) b_p,
       a.classes a_classes,b.classes b_classes,
       coalesce(a.eligible,false) a_eligible,coalesce(b.eligible,false) b_eligible
       FROM {a} a FULL OUTER JOIN {b} b USING(sr_id,session_date)'''


def _differences_sql(paired):
    # Linked means a uniquely resolved CIK, matching consumer -1/-2 handling.
    # Ambiguities are separately retained; no arbitrary CIK or silent removal.
    return f'''SELECT *, CASE
       WHEN a_n=1 AND b_n<>1 THEN '1_a_only'
       WHEN b_n=1 AND a_n<>1 THEN '1_b_only'
       WHEN a_n=1 AND b_n=1 AND a_cik<>b_cik THEN '2_cik'
       WHEN a_n=1 AND b_n=1 AND a_cik=b_cik AND a_p AND NOT b_p THEN '3_p_to_j'
       WHEN a_n=1 AND b_n=1 AND a_cik=b_cik AND NOT a_p AND b_p THEN '3_j_to_p'
       WHEN a_n=1 AND b_n=1 AND a_cik=b_cik AND a_p=b_p AND a_classes IS DISTINCT FROM b_classes THEN '4_class'
       END category FROM {paired}'''


def _panel_sql(panel, members, year, last=LAST_SESSION, first=None):
    lower = f" AND m.session_date>=DATE '{first}'" if first else ''
    return f'''SELECT m.security_id sr_id,m.session_date,
         list_sort(list(DISTINCT p.cik) FILTER(WHERE p.cik IS NOT NULL)) ciks,
         count(DISTINCT p.cik) n_ciks
       FROM {members} m LEFT JOIN {panel} p ON p.security_id=m.security_id
         AND m.session_date BETWEEN p.valid_from AND p.valid_to
         AND CASE WHEN p.link_tier='backfill' THEN p.evidence_at ELSE p.available_at END
             <=CAST(m.session_date AS TIMESTAMP)+INTERVAL 22 HOUR
       WHERE m.member AND year(m.session_date)={year}
         AND m.session_date<=DATE '{last}'{lower}
       GROUP BY m.security_id,m.session_date'''


def compare_arms(con, *, baseline, variant, calendar, members, pins, output, window=None):
    """Produce rows1-10 and identifier-only parts for one of four fixed pairs.

    ``window`` (:class:`Window`) bounds the consumer view; the default is the request's original window.

    baseline/variant are {arm, pit, link_table}; pins maps every exact consumed
    absolute path to its independently frozen SHA256. Membership has its entire
    native file inventory, not a caller-selected sample. Full arm/source/code
    admission is the caller's separate responsibility. No resume or overwrite is
    implicit: failed output remains evidence and a retry uses a fresh directory.
    """
    baseline, variant = copy.deepcopy(baseline), copy.deepcopy(variant)
    members, pins = tuple(members), copy.deepcopy(pins)
    window = window or Window()
    last, cutoff = window.last_session.isoformat(), window.cutoff.isoformat()
    if window.first_session is not None and window.first_session > window.last_session:
        raise ValueError('Comparison window starts after it ends')
    if (variant['arm'], baseline['arm']) not in PAIRS or not members:
        raise ValueError('A fixed P9 comparison and complete membership inventory required')
    directory = _path(output)
    database_root = _resources(con)
    if database_root not in directory.parents or directory == database_root:
        raise ValueError('Comparison outputs must be below the isolated database directory')
    paths = [baseline['pit'], variant['pit'], baseline['link_table'], variant['link_table'], calendar, *members]
    physical = {str(_path(path)) for path in paths}
    if physical != set(pins) or any(directory == _path(path) or directory in _path(path).parents for path in physical):
        raise ValueError('Exact complete distinct input pins and disjoint output required')
    prefix = 'a0_pair_' + uuid.uuid4().hex
    made = []
    def view(suffix, query):
        name = _ident(prefix + suffix)
        con.execute(f'CREATE TEMP VIEW {name} AS {query}')
        made.append(('VIEW', name))
        return name
    def table(suffix, query):
        name = _ident(prefix + suffix)
        con.execute(f'CREATE TEMP TABLE {name} AS {query}')
        made.append(('TABLE', name))
        return name
    def cleanup(mark=0):
        for kind, name in reversed(made[mark:]):
            con.execute(f'DROP {kind} IF EXISTS {name}')
        del made[mark:]
    outputs = {}
    def emit(name, query):
        target = directory / name
        con.execute(f'COPY (SELECT * FROM ({query}) emitted ORDER BY ALL) '
                    f'TO {_literal(target.as_posix())} (FORMAT PARQUET, COMPRESSION ZSTD)')
        outputs[name] = {'sha256': _digest(target), 'bytes': target.stat().st_size}
        return target
    with _inputs(pins):
        directory.mkdir(parents=False, exist_ok=False)
        try:
            a = view('_a_pit', 'SELECT * FROM ' + _parquet([baseline['pit']]))
            b = view('_b_pit', 'SELECT * FROM ' + _parquet([variant['pit']]))
            la = view('_a_panel', 'SELECT * FROM ' + _parquet([baseline['link_table']]))
            lb = view('_b_panel', 'SELECT * FROM ' + _parquet([variant['link_table']]))
            cal = view('_cal', 'SELECT session_date FROM ' + _parquet([calendar]))
            # Explicit projection prevents reading membership's aggregate returns.
            member = view('_member', 'SELECT security_id,session_date,member FROM ' + _parquet(members))
            integer, clock = {'BIGINT'}, {'TIMESTAMP', 'TIMESTAMP WITH TIME ZONE'}
            for relation in (a, b):
                _require(con, relation, {'sr_id': integer, 'cik': integer, 'start': {'DATE'},
                    'end_incl': {'DATE'}, 'available_at': clock, 'primary': {'VARCHAR'},
                    'basis': {'VARCHAR'}, 'class_status': {'VARCHAR'}})
                if _count(con, f'SELECT 1 FROM {relation} WHERE sr_id IS NULL OR sr_id<=0 OR '
                        'cik IS NULL OR cik<=0 OR start IS NULL OR available_at IS NULL OR start>end_incl'):
                    raise ValueError('Invalid native bridge identifier/date/clock')
            if _schema(con, a) != _schema(con, b):
                raise ValueError('Full PIT schemas differ; row equality is undefined')
            for relation in (la, lb):
                _require(con, relation, {'security_id': integer, 'cik': integer,
                    'valid_from': {'DATE'}, 'valid_to': {'DATE'}, 'available_at': clock,
                    'evidence_at': clock, 'link_tier': {'VARCHAR'}})
                if _count(con, f'SELECT 1 FROM {relation} WHERE security_id IS NULL OR security_id<=0 OR '
                    "cik IS NULL OR cik<=0 OR valid_from IS NULL OR valid_to IS NULL OR valid_from>valid_to OR "
                    "CASE WHEN link_tier='backfill' THEN evidence_at ELSE available_at END IS NULL"):
                    raise ValueError('Invalid native panel identifier/date/clock')
            _require(con, cal, {'session_date': {'DATE'}})
            _require(con, member, {'security_id': integer, 'session_date': {'DATE'}, 'member': {'BOOLEAN'}})
            if _count(con, f'SELECT session_date FROM {cal} GROUP BY 1 HAVING count(*)<>1 OR session_date IS NULL'):
                raise ValueError('Calendar has duplicate or null sessions')
            if _count(con, f'SELECT security_id,session_date FROM {member} GROUP BY 1,2 HAVING '
                    'count(*)<>1 OR security_id IS NULL OR security_id<=0 OR session_date IS NULL'):
                raise ValueError('Membership has duplicate or invalid cells')
            if _count(con, f'SELECT 1 FROM {member} m ANTI JOIN {cal} c USING(session_date)'):
                raise ValueError('Membership session absent from original calendar')
            if _count(con, f'SELECT 1 FROM {member} WHERE member IS NULL'):
                raise ValueError('Unknown membership flag cannot be treated as false')
            first = con.execute(f'SELECT min(start) FROM (SELECT start FROM {a} UNION ALL SELECT start FROM {b})').fetchone()[0]
            cal_lo, cal_hi = con.execute(f'SELECT min(session_date),max(session_date) FROM {cal}').fetchone()
            original = 'Calendar must cover the full original 2017-2023 bridge range'
            if first is None:
                raise ValueError(original)
            if window.first_session is None:
                start, lower = first, None
                if first.year < 2017:
                    raise ValueError(original)
            else:
                # the axis holds calendar sessions only: start at the first one on or after the window start
                opened = con.execute(f"SELECT min(session_date) FROM {cal} WHERE session_date>="
                                     f"DATE '{window.first_session.isoformat()}'").fetchone()[0]
                start, lower = max(first, opened or window.first_session), window.first_session.isoformat()
            if start.isoformat() > last or cal_lo is None or cal_lo > start or cal_hi.isoformat() < last:
                raise ValueError(original if window == Window() else
                                 f'Calendar must cover the full comparison window {start}..{last}')
            years = range(start.year, window.last_session.year + 1)
            bucket_of = {}
            for year in years:
                labels = [label for label, lo, hi in window.row_7_buckets if lo <= year <= hi]
                if len(labels) != 1:
                    raise ValueError(f'Session year {year} needs exactly one row 7 bucket')
                bucket_of[str(year)] = labels[0]
            axis_lines = _count(con, f'SELECT sr_id FROM {a} UNION SELECT sr_id FROM {b}')
            axis_dates = _count(con, f'SELECT session_date FROM {cal} WHERE session_date>={_literal(start.isoformat())} '
                f"AND session_date<=DATE '{last}'")
            row_differences = {}
            overlap = f" AND coalesce(end_incl,DATE '9999-12-31')>=DATE '{lower}'" if lower else ''
            for row, condition in (('8', f"start<DATE '{cutoff}' AND available_at<TIMESTAMP '{cutoff}'{overlap}"),
                                   ('9', f"start<DATE '{cutoff}' AND available_at>=TIMESTAMP '{cutoff}'{overlap}")):
                selects = [f'SELECT * FROM {relation} WHERE {condition}' for relation in (a, b)]
                row_differences[row] = {
                    'a_only': _count(con, selects[0] + ' EXCEPT ALL ' + selects[1]),
                    'b_only': _count(con, selects[1] + ' EXCEPT ALL ' + selects[0])}
            yearly, change_parts = {}, []
            mark = len(made)
            categories = ('1_a_only','1_b_only','2_cik','3_p_to_j','3_j_to_p','4_class')
            sides = ('before', 'at_or_after')
            observed = (f"TIMESTAMP '{window.observed_at.isoformat(sep=' ')}'"
                        if window.observed_at is not None else None)
            for year in years:
                sa = table('_a_state', _state_sql(a, cal, year, last, lower))
                sb = table('_b_state', _state_sql(b, cal, year, last, lower))
                paired = table('_paired', _pair_sql(sa, sb))
                diff = view('_diff', _differences_sql(paired))
                counts = dict(con.execute(f'SELECT category,count(*) FROM {diff} WHERE category IS NOT NULL GROUP BY 1').fetchall())
                row7_sql = f'''SELECT count(*) FILTER(WHERE d.a_eligible AND NOT d.b_eligible),
                    count(*) FILTER(WHERE d.b_eligible AND NOT d.a_eligible),count(*) FILTER(WHERE d.a_eligible)
                    FROM {paired} d JOIN {member} m ON m.security_id=d.sr_id AND m.session_date=d.session_date
                    WHERE m.member'''
                row7 = con.execute(row7_sql).fetchone()
                ambiguous = con.execute(f'''SELECT count(*) FILTER(WHERE a_n>1),count(*) FILTER(WHERE b_n>1),
                    count(*) FILTER(WHERE (a_n>1 OR b_n>1) AND a_ciks IS DISTINCT FROM b_ciks) FROM {paired}''').fetchone()
                active = _count(con, f'SELECT 1 FROM {paired}')
                dates = _count(con, f"SELECT 1 FROM {cal} WHERE year(session_date)={year} AND session_date>="
                    f"DATE '{start.isoformat()}' AND session_date<=DATE '{last}'")
                entry = {'rows_1_4': {key: counts.get(key, 0) for key in categories},
                    'row_7': dict(zip(('a_only','b_only','a_total'), row7, strict=True)),
                    'ambiguous_cells_a': ambiguous[0], 'ambiguous_cells_b': ambiguous[1],
                    'ambiguous_cik_set_changed': ambiguous[2], 'both_no_qualifying_cik': axis_lines*dates-active,
                    'row_10': None, 'panel_ambiguous_a': None, 'panel_ambiguous_b': None}
                if observed:
                    # the session mark d 22:00 against the removed source's observation clock
                    side = (f"CASE WHEN CAST({{d}}session_date AS TIMESTAMP)+INTERVAL 22 HOUR>={observed} "
                            "THEN 'at_or_after' ELSE 'before' END")
                    split = {(s, c): n for s, c, n in con.execute(f"SELECT {side.format(d='')},category,count(*) "
                             f'FROM {diff} WHERE category IS NOT NULL GROUP BY 1,2').fetchall()}
                    entry['rows_1_4_by_observation'] = {s: {c: split.get((s, c), 0) for c in categories} for s in sides}
                    entry['row_7_by_observation'] = {s: dict(zip(('a_only','b_only','a_total'), con.execute(
                        f"{row7_sql} AND {side.format(d='d.')}='{s}'").fetchone(), strict=True)) for s in sides}
                change_parts.append(emit(f'{year}-rows-1-4-identifiers.parquet',
                    f'SELECT sr_id,session_date,category,a_cik,b_cik,a_ciks,b_ciks FROM {diff} WHERE category IS NOT NULL'))
                emit(f'{year}-row-7-identifiers.parquet', f'''SELECT d.sr_id,d.session_date,
                    CASE WHEN d.a_eligible THEN 'a_only' ELSE 'b_only' END side
                    FROM {paired} d JOIN {member} m ON m.security_id=d.sr_id AND m.session_date=d.session_date
                    WHERE m.member AND d.a_eligible<>d.b_eligible''')
                emit(f'{year}-ambiguity-identifiers.parquet', f'''SELECT sr_id,session_date,a_ciks,b_ciks
                    FROM {paired} WHERE a_n>1 OR b_n>1''')
                if year >= window.panel_from_year:
                    pa = table('_pa', _panel_sql(la, member, year, last, lower))
                    pb = table('_pb', _panel_sql(lb, member, year, last, lower))
                    entry['row_10'] = _count(con, f'SELECT 1 FROM {pa} a JOIN {pb} b USING(sr_id,session_date) '
                        'WHERE a.ciks IS DISTINCT FROM b.ciks')
                    entry['panel_ambiguous_a'] = _count(con, f'SELECT 1 FROM {pa} WHERE n_ciks>1')
                    entry['panel_ambiguous_b'] = _count(con, f'SELECT 1 FROM {pb} WHERE n_ciks>1')
                yearly[str(year)] = entry
                cleanup(mark)
            changes = _parquet(change_parts)
            n_ids = _count(con, f'SELECT DISTINCT sr_id FROM {changes}')
            cik_query = f'SELECT unnest(a_ciks) cik FROM {changes} UNION SELECT unnest(b_ciks) cik FROM {changes}'
            n_ciks = _count(con, cik_query)
            emit('rows-1-4-sr-id-list.parquet', f'SELECT DISTINCT sr_id FROM {changes}')
            emit('rows-1-4-cik-list.parquet', cik_query)
            buckets = {label: {'a_only': 0, 'b_only': 0, 'a_total': 0} for label, _, _ in window.row_7_buckets}
            totals = {key: 0 for key in categories}
            by_side = {s: {key: 0 for key in categories} for s in sides}
            row7_side = {s: {'a_only': 0, 'b_only': 0, 'a_total': 0} for s in sides}
            for year, entry in yearly.items():
                for key, value in entry['row_7'].items():
                    buckets[bucket_of[year]][key] += value
                for key, value in entry['rows_1_4'].items():
                    totals[key] += value
                for s in sides if observed else ():
                    for key, value in entry['rows_1_4_by_observation'][s].items():
                        by_side[s][key] += value
                    for key, value in entry['row_7_by_observation'][s].items():
                        row7_side[s][key] += value
            result = {'version': VERSION, 'status': 'CONDITIONAL_DIAGNOSTIC_COUNTS',
                'comparison': variant['arm'] + ' vs ' + baseline['arm'], 'baseline': baseline, 'variant': variant,
                'input_sha256': pins, 'mark_utc': '22:00:00', 'first_bridge_start': first.isoformat(),
                'last_session': last, 'window': window.describe(start), 'complete_axis_cells': axis_lines*axis_dates,
                'rows_1_4': totals, 'row_5_distinct_sr_id': n_ids, 'row_6_distinct_cik': n_ciks,
                'row_7': buckets, 'row_8': row_differences['8'], 'row_9': row_differences['9'],
                'row_10': {year: yearly.get(year,{}).get('row_10',0)
                           for year in map(str,range(window.panel_from_year, window.last_session.year + 1))},
                'yearly': yearly, 'files': outputs, 'D0_verified_here': False, 'sanity_verified_here': False,
                'source_quality_certified': False, 'gate_promotion': False,
                'qualification': 'Complete physical rows are compared, preserving multiplicities. '
                    'Consumer linked means exactly one qualifying CIK; ambiguous states are separately counted and retained. '
                    'Panel CIK uses backfill evidence_at and all other available_at. Missing/ambiguous panel states compare '
                    'as distinct CIK sets without arbitrary selection. This kernel does not authenticate original source '
                    'coverage, same-code arms, D0, non-vacuity, row11 or producing-commit provenance. Those remain mandatory '
                    'before interpreting any zero. No later-evidence residual or held-fixed input is cleared.'}
            if observed:
                result['rows_1_4_by_observation'] = by_side
                result['row_7_by_observation'] = row7_side
        finally:
            cleanup()
    # Completion only after original byte checks and SQL cleanup, including exceptions.
    with (directory / 'counts.json').open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write('\n')
    return result
