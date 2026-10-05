"""Physical P9 diagnostics checked against a small independent consumer oracle."""
import collections
import datetime as dt
import hashlib
import json

import duckdb
import pytest

from atx_db.alpha_panel import private_identity_arm_comparison as D


@pytest.fixture
def connection(tmp_path):
    con = duckdb.connect(str(tmp_path / 'comparison.duckdb'), config={
        'memory_limit': '128MB', 'threads': 1, 'temp_directory': str(tmp_path / 'spill'),
        'preserve_insertion_order': False})
    con.execute("SET TimeZone='UTC'")
    yield con
    con.close()


def setup(con, root):
    dates = [dt.date(2017,10,2),dt.date(2018,1,2),dt.date(2019,1,2),
             dt.date(2020,1,2),dt.date(2021,1,4),dt.date(2021,1,5),
             dt.date(2022,1,3),dt.date(2023,12,29),dt.date(2024,1,2)]
    lo, hi = dt.date(2021,1,4), dt.date(2021,1,5)
    known = dt.datetime(2020,1,1)
    def row(sid,cik=None,p='P',cls='common',**extra):
        return {'sr_id':sid,'cik':cik or sid,'start':extra.get('start',lo),
                'end_incl':extra.get('end',hi),'available_at':extra.get('clock',known),
                'primary':p,'basis':extra.get('basis','strict_dated'),'class_status':cls,
                'extra_native_column':extra.get('extra','kept')}
    left = [row(1),row(3),row(4),row(5,p='J'),row(6),row(7),row(8,p='J'),row(8),
            row(9,clock=dt.datetime(2021,1,4,22,0,1)),row(10,basis='current_ticker_verified'),
            row(11),row(11,111),row(12),row(13,cls=None),row(14,end=None),
            row(15,start=dates[0]),row(16,clock=dt.datetime(2024,1,1)),row(17,extra='left')]
    right = [row(2),row(3,103),row(4,p='J'),row(5),row(6,cls='A'),row(7,cls='U'),row(8),
             row(9,clock=dt.datetime(2021,1,4,22)),row(11),row(12),row(12,112),
             row(13,cls=''),row(14,end=None),row(15,start=dates[0]),
             row(16,clock=dt.datetime(2024,1,2)),row(17,extra='right')]
    # Duplicate full rows change EXCEPT ALL counts, not the consumer state.
    left.append(dict(left[0]))
    schema = ('sr_id BIGINT,cik BIGINT,start DATE,end_incl DATE,available_at TIMESTAMP,'
              '"primary" VARCHAR,basis VARCHAR,class_status VARCHAR,extra_native_column VARCHAR')
    pins, files = {}, {}
    def write(name, table):
        path=root/(name+'.parquet')
        con.execute(f"COPY {table} TO '{path.as_posix()}' (FORMAT PARQUET)")
        pins[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        files[name]=path
    for name, rows in (('a',left),('b',right)):
        con.execute(f'CREATE TABLE {name}({schema})')
        con.executemany(f'INSERT INTO {name} VALUES (?,?,?,?,?,?,?,?,?)',[list(row.values()) for row in rows])
        write(name,name)
    con.execute('CREATE TABLE cal(session_date DATE)')
    con.executemany('INSERT INTO cal VALUES (?)',[(d,) for d in dates])
    write('calendar','cal')
    con.execute('CREATE TABLE membership(security_id BIGINT,session_date DATE,member BOOLEAN,mkt_ret DOUBLE)')
    membership={(sid,d) for sid in range(1,18) for d in dates if sid!=6}
    con.executemany('INSERT INTO membership VALUES (?,?,?,?)',[(sid,d,True,999.0) for sid,d in membership])
    write('members','membership')
    pschema=('security_id BIGINT,cik BIGINT,valid_from DATE,valid_to DATE,'
             'available_at TIMESTAMP,evidence_at TIMESTAMP,link_tier VARCHAR')
    con.execute('CREATE TABLE pa('+pschema+')')
    con.execute('CREATE TABLE pb('+pschema+')')
    # Backfill is admitted by old evidence_at despite its 2026 available_at.
    con.execute("INSERT INTO pa VALUES (1,1,DATE '2021-01-04',DATE '2021-01-05',TIMESTAMP '2026-01-01',TIMESTAMP '2021-01-04 22:00:00','backfill')")
    con.execute("INSERT INTO pb VALUES (1,2,DATE '2021-01-04',DATE '2021-01-05',TIMESTAMP '2021-01-05 22:00:01',TIMESTAMP '2020-01-01','strict')")
    write('la','pa'); write('lb','pb')
    arguments={'baseline':{'arm':'A','pit':str(files['a']),'link_table':str(files['la'])},
               'variant':{'arm':'B12','pit':str(files['b']),'link_table':str(files['lb'])},
               'calendar':str(files['calendar']),'members':[str(files['members'])],
               'pins':pins,'output':root/'result'}
    return arguments,left,right,dates,membership


def oracle(rows,sid,date):
    mark=dt.datetime.combine(date,dt.time(22))
    visible=[r for r in rows if r['sr_id']==sid and r['start']<=date and
             (r['end_incl'] is None or date<=r['end_incl']) and r['available_at']<=mark and
             r['primary'] in ('P','J') and r['basis']!='current_ticker_verified']
    ciks={r['cik'] for r in visible}
    classes={r['class_status'] or '' for r in visible}
    primary=any(r['primary']=='P' for r in visible)
    allowed=lambda value:value=='common' or (len(value)==1 and 'A'<=value<='Z' and value not in 'UWR')
    eligible=len(ciks)==1 and primary and all(map(allowed,classes))
    return ciks,primary,classes,eligible


def test_full_yearly_counts_identifiers_clocks_classes_and_full_rows(connection,tmp_path):
    args,a,b,dates,membership=setup(connection,tmp_path)
    result=D.compare_arms(connection,**args)
    counts=collections.Counter(); changed_ids=set(); changed_ciks=set()
    eligible=collections.defaultdict(lambda:collections.Counter())
    for sid in range(1,18):
        for date in dates[:-1]:
            ac,ap,acl,ae=oracle(a,sid,date); bc,bp,bcl,be=oracle(b,sid,date)
            category=None
            if len(ac)==1 and len(bc)!=1: category='1_a_only'
            elif len(bc)==1 and len(ac)!=1: category='1_b_only'
            elif len(ac)==len(bc)==1:
                if ac!=bc: category='2_cik'
                elif ap!=bp: category='3_p_to_j' if ap else '3_j_to_p'
                elif acl!=bcl: category='4_class'
            if category:
                counts[category]+=1; changed_ids.add(sid); changed_ciks|=ac|bc
            if (sid,date) in membership:
                bucket=str(date.year) if date.year>=2020 else '2017-2019'
                eligible[bucket]['a_only']+=int(ae and not be)
                eligible[bucket]['b_only']+=int(be and not ae)
                eligible[bucket]['a_total']+=int(ae)
    assert result['rows_1_4']==counts
    assert result['row_5_distinct_sr_id']==len(changed_ids)
    assert result['row_6_distinct_cik']==len(changed_ciks)
    for year,values in result['row_7'].items():
        assert values=={k:eligible[year][k] for k in values}
    cutoff=dt.datetime(2024,1,1)
    for number,late in (('8',False),('9',True)):
        ar=collections.Counter(tuple(r.values()) for r in a if r['start']<cutoff.date() and (r['available_at']>=cutoff)==late)
        br=collections.Counter(tuple(r.values()) for r in b if r['start']<cutoff.date() and (r['available_at']>=cutoff)==late)
        assert result['row_'+number]=={'a_only':sum((ar-br).values()),'b_only':sum((br-ar).values())}
    assert result['row_10']['2021']==2
    assert result['complete_axis_cells']==17*8
    assert result['yearly']['2021']['ambiguous_cells_a']==2
    assert result['yearly']['2021']['ambiguous_cells_b']==2
    names={row[0] for row in connection.execute(f"SELECT cik FROM read_parquet('{(tmp_path/'result/rows-1-4-cik-list.parquet').as_posix()}')").fetchall()}
    assert names==changed_ciks and {111,112}<=names
    assert not result['D0_verified_here'] and not result['gate_promotion']
    assert json.loads((tmp_path/'result/counts.json').read_text())['rows_1_4']==counts
    assert not connection.execute("SELECT table_name FROM duckdb_tables() WHERE table_name LIKE 'a0_pair_%'").fetchall()


def test_input_mutation_finally_refuses_completion_and_cleans_sql(connection,tmp_path):
    args,*_=setup(connection,tmp_path)
    source=tmp_path/'a.parquet'
    class Mutation:
        done=False
        def execute(self,sql):
            result=connection.execute(sql)
            if 'EXCEPT ALL' in sql and not self.done:
                self.done=True
                with source.open('ab') as stream: stream.write(b'changed')
            return result
    with pytest.raises((ValueError,duckdb.InvalidInputException)):
        D.compare_arms(Mutation(),**args)
    assert not (tmp_path/'result/counts.json').exists()
    assert not connection.execute("SELECT table_name FROM duckdb_tables() WHERE table_name LIKE 'a0_pair_%'").fetchall()


def test_duplicate_membership_and_schema_changes_fail_closed(connection,tmp_path):
    args,*_=setup(connection,tmp_path)
    connection.execute('INSERT INTO membership SELECT * FROM membership LIMIT 1')
    source=tmp_path/'members.parquet'
    source.unlink()
    connection.execute(f"COPY membership TO '{source.as_posix()}' (FORMAT PARQUET)")
    args['pins'][str(source)]=hashlib.sha256(source.read_bytes()).hexdigest()
    with pytest.raises(ValueError,match='duplicate or invalid'):
        D.compare_arms(connection,**args)
    assert not (tmp_path/'result/counts.json').exists()
    args['output']=tmp_path/'schema-result'
    source=tmp_path/'b.parquet'
    source.unlink()
    connection.execute(f"COPY (SELECT *,TRUE extra_column FROM b) TO '{source.as_posix()}' (FORMAT PARQUET)")
    args['pins'][str(source)]=hashlib.sha256(source.read_bytes()).hexdigest()
    with pytest.raises(ValueError,match='Full PIT schemas'):
        D.compare_arms(connection,**args)
    assert not (tmp_path/'schema-result/counts.json').exists()


def test_null_native_panel_end_refuses_permanent_coverage(connection,tmp_path):
    args,*_=setup(connection,tmp_path)
    connection.execute('UPDATE pa SET valid_to=NULL')
    source=tmp_path/'la.parquet'
    source.unlink()
    connection.execute(f"COPY pa TO '{source.as_posix()}' (FORMAT PARQUET)")
    args['pins'][str(source)]=hashlib.sha256(source.read_bytes()).hexdigest()
    with pytest.raises(ValueError,match='Invalid native panel'):
        D.compare_arms(connection,**args)
    assert not (tmp_path/'result/counts.json').exists()


def _window_oracle(a, b, dates, membership, window, buckets):
    counts = collections.Counter(); observed = collections.defaultdict(collections.Counter)
    eligible = collections.defaultdict(collections.Counter)
    for sid in range(1, 18):
        for date in dates:
            ac, ap, acl, ae = oracle(a, sid, date); bc, bp, bcl, be = oracle(b, sid, date)
            category = None
            if len(ac) == 1 and len(bc) != 1: category = '1_a_only'
            elif len(bc) == 1 and len(ac) != 1: category = '1_b_only'
            elif len(ac) == len(bc) == 1:
                if ac != bc: category = '2_cik'
                elif ap != bp: category = '3_p_to_j' if ap else '3_j_to_p'
                elif acl != bcl: category = '4_class'
            side = None
            if window.observed_at is not None:
                side = 'at_or_after' if dt.datetime.combine(date, dt.time(22)) >= window.observed_at else 'before'
            if category:
                counts[category] += 1
                if side: observed[side][category] += 1
            if (sid, date) in membership:
                bucket = next(label for label, lo, hi in buckets if lo <= date.year <= hi)
                eligible[bucket]['a_only'] += int(ae and not be)
                eligible[bucket]['b_only'] += int(be and not ae)
                eligible[bucket]['a_total'] += int(ae)
    return counts, observed, eligible


def test_explicit_original_window_is_the_default(connection, tmp_path):
    args, *_ = setup(connection, tmp_path)
    default = D.compare_arms(connection, **args)
    args['output'] = tmp_path / 'explicit'
    explicit = D.compare_arms(connection, **args, window=D.Window())
    for key in ('rows_1_4', 'row_5_distinct_sr_id', 'row_6_distinct_cik', 'row_7', 'row_8', 'row_9', 'row_10',
                'complete_axis_cells', 'first_bridge_start', 'last_session'):
        assert explicit[key] == default[key]
    assert default['window']['last_session'] == '2023-12-29' and default['window']['cutoff'] == '2024-01-01'
    assert default['window']['first_session'] is None and 'rows_1_4_by_observation' not in default
    assert list(default['row_7']) == ['2017-2019', '2020', '2021', '2022', '2023']
    assert list(default['row_10']) == [str(y) for y in range(2018, 2024)]


def test_extended_window_bounds_sessions_rows_and_observation_clock(connection, tmp_path):
    args, a, b, dates, membership = setup(connection, tmp_path)
    buckets = (('2021', 2021, 2021), ('2022', 2022, 2022), ('2023', 2023, 2023), ('2024', 2024, 2024))
    window = D.Window(first_session=dt.date(2021, 1, 5), last_session=dt.date(2024, 1, 2), cutoff=dt.date(2024, 1, 2),
                      row_7_buckets=buckets, panel_from_year=2021, observed_at=dt.datetime(2021, 1, 5, 22))
    result = D.compare_arms(connection, **args, window=window)
    inside = [d for d in dates if window.first_session <= d <= window.last_session]
    assert inside[0] == dt.date(2021, 1, 5) and inside[-1] == dt.date(2024, 1, 2)
    counts, observed, eligible = _window_oracle(a, b, inside, membership, window, buckets)
    assert result['rows_1_4'] == {k: counts.get(k, 0) for k in result['rows_1_4']}
    assert sum(counts.values()) > 0 and set(observed) == {'at_or_after'}
    assert result['rows_1_4_by_observation']['before'] == {k: 0 for k in result['rows_1_4']}
    assert result['rows_1_4_by_observation']['at_or_after'] == {k: observed['at_or_after'].get(k, 0)
                                                                 for k in result['rows_1_4']}
    for label, values in result['row_7'].items():
        assert values == {k: eligible[label][k] for k in values}
    assert result['complete_axis_cells'] == 17 * len(inside)
    assert list(result['row_10']) == ['2021', '2022', '2023', '2024'] and result['row_10']['2021'] == 1
    assert result['window']['first_session'] == '2021-01-05' and result['last_session'] == '2024-01-02'
    cutoff = dt.datetime(2024, 1, 2)
    for number, late in (('8', False), ('9', True)):
        keep = lambda r: (r['start'] < cutoff.date() and (r['available_at'] >= cutoff) == late and
                          (r['end_incl'] is None or r['end_incl'] >= window.first_session))
        ar = collections.Counter(tuple(r.values()) for r in a if keep(r))
        br = collections.Counter(tuple(r.values()) for r in b if keep(r))
        assert result['row_' + number] == {'a_only': sum((ar - br).values()), 'b_only': sum((br - ar).values())}
    # available_at exactly at the cutoff is never readable inside the window: row 9, not row 8
    assert result['row_9'] == {'a_only': 0, 'b_only': 1}
    # a row ending the day before the window opens is outside rows 8/9 (only the open-ended row 14 remains)
    args['output'] = tmp_path / 'later'
    later = D.compare_arms(connection, **args, window=D.Window(
        first_session=dt.date(2021, 1, 6), last_session=dt.date(2024, 1, 2), cutoff=dt.date(2024, 1, 2),
        row_7_buckets=(('2022-2024', 2022, 2024),), panel_from_year=2022))
    assert later['window']['start_session'] == '2022-01-03'
    assert later['row_8'] == {'a_only': 0, 'b_only': 0} and later['row_9'] == {'a_only': 0, 'b_only': 0}
    assert later['row_10'] == {'2022': 0, '2023': 0, '2024': 0}


def test_observation_clock_splits_differences_at_the_session_mark(connection, tmp_path):
    args, a, b, dates, membership = setup(connection, tmp_path)
    window = D.Window(observed_at=dt.datetime(2021, 1, 4, 22))
    result = D.compare_arms(connection, **args, window=window)
    counts, observed, _ = _window_oracle(a, b, dates[:-1], membership, window, D.Window().row_7_buckets)
    assert result['rows_1_4'] == counts
    for side in ('before', 'at_or_after'):
        assert result['rows_1_4_by_observation'][side] == {k: observed[side].get(k, 0) for k in result['rows_1_4']}
    assert sum(observed['at_or_after'].values()) > 0
    totals = collections.Counter()
    for side in ('before', 'at_or_after'):
        totals.update(result['row_7_by_observation'][side])
    for key in ('a_only', 'b_only', 'a_total'):
        assert totals[key] == sum(v[key] for v in result['row_7'].values())


def test_window_refuses_uncovered_sessions_years_and_inverted_bounds(connection, tmp_path):
    args, *_ = setup(connection, tmp_path)
    for n, (window, message) in enumerate((
            (D.Window(last_session=dt.date(2024, 1, 3), cutoff=dt.date(2024, 1, 4)), 'Calendar must cover'),
            (D.Window(first_session=dt.date(2022, 1, 1), last_session=dt.date(2021, 12, 31)), 'window'),
            (D.Window(first_session=dt.date(2020, 1, 1), last_session=dt.date(2024, 1, 2), cutoff=dt.date(2024, 1, 3),
                      row_7_buckets=(('2020-2023', 2020, 2023),)), 'bucket'))):
        args['output'] = tmp_path / f'refused-{n}'
        with pytest.raises(ValueError, match=message):
            D.compare_arms(connection, **args, window=window)
        assert not (tmp_path / f'refused-{n}' / 'counts.json').exists()
