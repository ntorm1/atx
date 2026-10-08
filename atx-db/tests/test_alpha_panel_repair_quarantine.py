"""Future identity repairs never feed a causal numeric feature window."""
import datetime as dt

import duckdb
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import characteristics as CH
from atx_db.alpha_panel import gold as G


def test_sparse_asof_repair_mask_is_past_only_and_uses_calendar_sessions(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setattr(CH, "REPAIR_LOOKBACK", 2)
    days = [dt.date(2020, 1, 2) + dt.timedelta(days=k) for k in range(7)]
    pq.write_table(pa.table({"session_date": days}), tmp_path / "calendar.parquet")
    folder = tmp_path / "prices" / "year=2020"
    folder.mkdir(parents=True)
    pq.write_table(pa.table({"session_date": days, "security_id": [1] * 7,
                             "sid0_repaired": [False, True, False, False, False, True, False]}), folder / "p.parquet")
    con = duckdb.connect()
    try:
        # The line skips two calendar sessions after the first repair. A row-count
        # window would incorrectly keep the fourth observation in quarantine.
        panel = pa.table({"session_date": [days[k] for k in (0, 1, 4, 5, 6)], "security_id": [1] * 5,
                          "ret": [.1] * 5, "ret_guarded": [False] * 5, "close": [10.] * 5})
        con.register("panel", panel)
        relation = CH.repair_events(con)
        output = con.execute(CH.input_sql("panel", None, panel.column_names, relation) + " ORDER BY session_date").fetch_arrow_table()
        assert output["source_repaired_now"].to_pylist() == [False, True, False, True, False]
        assert output["source_repaired_window"].to_pylist() == [False, True, False, True, True]
        assert output["close"].to_pylist() == [10., None, 10., None, 10.]
        assert output["ret"].to_pylist() == [.1, None, .1, None, .1]
        assert output["ret_guarded"].to_pylist() == [False, True, False, True, False]
        # Cutting source events before the future repair gives identical earlier inputs.
        relation = CH.repair_events(con, days[4])
        truncated = con.execute(CH.input_sql("panel", None, panel.column_names, relation)
                                + " WHERE p.session_date<=? ORDER BY p.session_date", [days[4]]).fetch_arrow_table()
        assert truncated["source_repaired_window"].to_pylist() == [False, True, False]
    finally:
        con.close()


def test_gold_does_not_zero_fill_quarantined_event_or_keep_repaired_current_row():
    rows = pa.table({"session_date": [dt.date(2020, 1, 2)] * 5, "security_id": [1, 2, 3, 4, 5],
                     "cik": [1] * 5, "grp_ff49": [1] * 5, "grp_ff12": [1] * 5,
                     "member_equity": [True] * 5, "x": [None, None, 1., 2., 3.],
                     "source_repaired_now": [True, False, False, False, False],
                     "source_repaired_window": [True, True, False, False, False]})
    selection = {"selected": [{"name": "x", "family": "event_time", "fill_zero": True, "variant": "raw"}], "controls": []}
    result = G.score_session(rows, selection)
    assert result["security_id"].to_pylist() == [2, 3, 4, 5]
    assert result["g_x"].to_pylist()[0] is None
    assert np.isfinite(result["g_x"].to_numpy()[1:]).all()
