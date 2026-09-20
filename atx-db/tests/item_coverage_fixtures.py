"""Compact synthetic aggregate evidence for coverage gate consumer tests."""

from __future__ import annotations

import datetime as dt

from atx_db.item_coverage import DEFAULT_SOURCE, DEFAULT_UNIVERSE_ID


def seed_gate_evidence(store, *, as_of=dt.date(2024, 6, 28), items=(1101,)):
    store.con.execute(
        """INSERT OR REPLACE INTO item_coverage_cohort_years
        SELECT ?,y::INTEGER,make_date(y,12,31),?,'test','test','{}',3000,3000,3000,
               0,0,0,true,'complete',make_date(y,12,31)+INTERVAL 22 HOUR,'test',TIMESTAMP '2024-01-01'
        FROM range(2015,?) years(y)""",
        [DEFAULT_UNIVERSE_ID, as_of, as_of.year],
    )
    for item in items:
        store.con.execute(
            """INSERT OR REPLACE INTO fundamental_item_coverage
            (coverage_id,source,universe_id,item_id,canonical_code,basis,fiscal_year,
             n_securities,n_with_value,coverage_pct,as_of_date,cohort_status,ranking_date)
            SELECT 'gate-'||?||'-'||y,?,?,?,'item-'||?,'annual',y,3000,2700,90.0,?,'complete',
                   make_date(y,12,31) FROM range(2015,?) years(y)""",
            [item, DEFAULT_SOURCE, DEFAULT_UNIVERSE_ID, item, item, as_of, as_of.year],
        )
