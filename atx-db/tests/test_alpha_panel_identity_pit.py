"""Synthetic regressions for bridge tier isolation and knowledge clocks."""
import datetime as dt

import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import identity_backfill as BF, identity_names as N, identity_table as T


@pytest.fixture
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT', str(tmp_path))
    for attr, directory in [('FINRA_SI_DIR', 'finra'), ('FSDS_DIR', 'fsds'),
                            ('COMPANYFACTS_DIR', 'facts'), ('IDENTITY_R4_DIR', 'r4')]:
        monkeypatch.setattr(C, attr, tmp_path / directory)
    con = C.connect(memory='128MB', threads=1)

    def put(path, sql):
        C.copy_to_parquet(con, sql, tmp_path / path)

    put('calendar/calendar.parquet', "SELECT CAST(d AS DATE) session_date FROM generate_series(DATE '2021-01-04', DATE '2021-01-07', INTERVAL 1 DAY) t(d)")
    monkeypatch.setattr(C, 'calendar_path', lambda: tmp_path / 'calendar/calendar.parquet')
    put('_tmp/panel_core/part.parquet', """SELECT session_date, sid::BIGINT security_id, 'ACME' ticker,
        CASE WHEN sid=2 AND session_date BETWEEN DATE '2021-01-05' AND DATE '2021-01-06' THEN 200 ELSE 100-sid END::DOUBLE adv63,
        true is_operating, false is_index FROM read_parquet('""" + (tmp_path / 'calendar/calendar.parquet').as_posix() + "') CROSS JOIN (VALUES(1),(2),(3)) s(sid)")
    put('_tmp/panel_member/part.parquet', "SELECT session_date,security_id,true member FROM read_parquet('" + (tmp_path / '_tmp/panel_core/part.parquet').as_posix() + "')")
    put('short_interest/si.parquet', "SELECT 3::BIGINT security_id,'ACME' symbol,DATE '2020-12-30' settlement_date,DATE '2021-01-03' dissemination_date")
    raw = tmp_path / 'finra/raw/si_fixture.csv'
    raw.parent.mkdir(parents=True)
    raw.write_text('symbolCode|settlementDate|issueName\nACME|2020-12-30|Acme Common Stock\n')
    put('fsds/sub/part.parquet', "SELECT 123::BIGINT cik,'Acme Inc' AS name,NULL::VARCHAR former,NULL::DATE changed,DATE '2020-12-01' filed,'10-K' form")
    put('facts/batch-0.parquet', "SELECT 123::BIGINT cik,DATE '2020-12-01' filed_date,'10-K' form,'us-gaap' taxonomy")
    yield tmp_path, con, put
    con.close()


def bridge(root, con, variant):
    # The fallback makes RED exercise current behavior before v4 exists.
    path = root / f'export/identity-bridge-v4-{variant}/links.parquet'
    if not path.exists():
        path = root / f'export/identity-bridge-v2-{variant}/links.parquet'
    return con.execute(f"SELECT sr_id,start,end_incl,\"primary\",link_tier FROM read_parquet('{path.as_posix()}') ORDER BY ALL").fetchall()


def t1_inputs(put):
    sql = """SELECT sid::BIGINT security_id,123::BIGINT cik,DATE '2021-01-04' AS start,
        DATE '2021-01-07' end_incl,'P' AS "primary",tier,'fixture' basis,
        CASE WHEN tier='backfill' THEN TIMESTAMP '2026-09-20' ELSE TIMESTAMP '2020-01-01' END available_at
        FROM (VALUES(1,'strict'),(2,'backfill')) t(sid,tier)"""
    put('identity/links_combined.parquet', sql)
    put('identity/links.parquet', sql + " WHERE tier='strict'")
    put('identity/links_pit.parquet', sql + " WHERE tier='strict'")
    put('identity/links_name_days.parquet', "SELECT 3::BIGINT security_id,DATE '2021-01-04' session_date,123::BIGINT cik,TIMESTAMP '2020-01-01' available_at WHERE false")


def test_t1_backfill_cannot_change_pit_primary_or_split_island(lake):
    root, con, put = lake
    t1_inputs(put)
    T.build()
    expected = [(1, dt.date(2021, 1, 4), dt.date(2021, 1, 7), 'P', 'strict')]
    assert bridge(root, con, 'pit') == expected
    assert bridge(root, con, 'strict') == expected


@pytest.mark.parametrize('s2', ['0', '1'])
def test_t2_name_survives_backfill_overlap(lake, monkeypatch, s2):
    root, con, put = lake
    monkeypatch.setenv('ATX_A0_S2', s2)
    put('identity/links.parquet', """SELECT 1::BIGINT security_id,123::BIGINT cik,DATE '2021-01-04' AS start,
        DATE '2021-01-07' end_incl,'P' AS "primary",'strict' tier,'fixture' basis,TIMESTAMP '2020-01-01' available_at WHERE false""")
    put('r4/phases/export-001/security_company_links.part-0000.parquet', "SELECT 3::BIGINT perm_security_id,'123' cik,'P' link_primary,'current_ticker_verified' link_basis,NULL::DATE valid_until,TIMESTAMP '2026-09-20' available_at")
    put('prices/2021/part.parquet', "SELECT 3::BIGINT security_id,session_date FROM read_parquet('" + C.calendar_path().as_posix() + "')")
    monkeypatch.setattr(BF, 'ALIVE_FROM', '2021-01-01')
    BF.build()
    N.build()
    receipt = BF.build()
    T.build()
    assert (receipt['backfill_rows'] == 0) == (s2 == '1')
    assert [(r[0], r[1], r[2], r[4]) for r in bridge(root, con, 'pit')] == [
        (3, dt.date(2021, 1, 4), dt.date(2021, 1, 7), 'name')]
    assert [r[4] for r in bridge(root, con, 'all')] == (['backfill'] if s2 == '0' else ['name'])


@pytest.mark.parametrize('clock, first_j', [('2021-01-05 22:00:00', 5),
                                         ('2021-01-05 22:00:00.000001', 6)])
def test_own_tiers_and_mark_boundary(lake, clock, first_j):
    root, con, put = lake
    sql = f"""SELECT sid::BIGINT security_id,123::BIGINT cik,DATE '2021-01-04' AS start,
        DATE '2021-01-07' end_incl,'P' AS "primary",tier,'fixture' basis,available_at
        FROM (VALUES (1,'strict',TIMESTAMP '2020-01-01'),
                     (2,'strict',TIMESTAMP '{clock}'),
                     (3,'name',TIMESTAMP '2020-01-01')) t(sid,tier,available_at)"""
    put('identity/links_combined.parquet', sql)
    put('identity/links_pit.parquet', sql)
    put('identity/links_name_days.parquet', "SELECT 3::BIGINT security_id,session_date,123::BIGINT cik,TIMESTAMP '2020-01-01' available_at FROM read_parquet('" + C.calendar_path().as_posix() + "')")
    # Name line has the largest volume, but cannot affect strict. Line 2 is eligible exactly at the mark.
    put('_tmp/panel_core/part.parquet', "SELECT session_date,sid::BIGINT security_id,'ACME' ticker,(sid*100)::DOUBLE adv63,true is_operating,false is_index FROM read_parquet('" + C.calendar_path().as_posix() + "') CROSS JOIN (VALUES(1),(2),(3)) t(sid)")
    T.build()
    strict = bridge(root, con, 'strict')
    assert (1, dt.date(2021,1,4), dt.date(2021,1,first_j-1), 'P', 'strict') in strict
    assert (1, dt.date(2021,1,first_j), dt.date(2021,1,7), 'J', 'strict') in strict
    assert [r[3] for r in bridge(root, con, 'pit') if r[0] == 3] == ['P']
    assert not (root / 'export/identity-bridge-v2-pit').exists()


def test_existing_export_refused_before_stage_mutation(lake):
    root, con, put = lake
    t1_inputs(put)
    target = root / 'export/identity-bridge-v4-pit'
    target.mkdir(parents=True)
    marker = target / 'manifest.json'
    marker.write_bytes(b'preserve existing export')
    with pytest.raises(FileExistsError, match='refusing to replace'):
        T.build()
    assert marker.read_bytes() == b'preserve existing export'
    assert not (root / 'identity/link_table.parquet').exists()


def test_all_tier_primary_is_preserved(lake):
    root, con, put = lake
    t1_inputs(put)
    T.build()
    rows = bridge(root, con, 'all')
    assert (1, dt.date(2021,1,5), dt.date(2021,1,6), 'J', 'strict') in rows
    assert (2, dt.date(2021,1,5), dt.date(2021,1,6), 'P', 'backfill') in rows
    assert con.execute(f"SELECT is_issuer_primary FROM read_parquet('{(root / 'identity/link_table.parquet').as_posix()}') WHERE security_id=1 AND valid_from=DATE '2021-01-05'").fetchone() == (False,)


@pytest.mark.parametrize('variant', ['strict', 'pit', 'all'])
def test_registry_discovers_current_v4_bridge(variant):
    from atx_db.stagelake import registry

    stage_name = f'export_identity_bridge_{variant}'
    stage = next(s for s in registry.load(include_discovered=False) if s.name == stage_name)
    assert stage.built_by == 'identity_table'
    assert stage.inputs == ('identity_table',)
    assert stage.manifest == f'export/identity-bridge-v4-{variant}/manifest.json'
    assert [(output.glob, output.view) for output in stage.outputs] == [
        (f'export/identity-bridge-v4-{variant}/links.parquet', stage_name)]

