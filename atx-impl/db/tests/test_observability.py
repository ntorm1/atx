from __future__ import annotations

import datetime as dt
import uuid


def _upsert_watermark(store, dataset_id: str, value: str) -> None:
    store.con.execute(
        """
        INSERT OR REPLACE INTO dataset_watermarks (
            dataset_id, watermark_name, watermark_value, updated_at
        )
        VALUES (?, 'max_available_at', ?, ?)
        """,
        [dataset_id, value, dt.datetime(2026, 7, 4, 12, 0, 0)],
    )


def _insert_sla(store, dataset_id: str, *, max_lag_days: int, severity: str) -> None:
    store.con.execute(
        """
        INSERT OR REPLACE INTO dataset_freshness_sla (
            dataset_id, max_lag_days, severity, enabled, updated_at
        )
        VALUES (?, ?, ?, true, ?)
        """,
        [dataset_id, max_lag_days, severity, dt.datetime(2026, 7, 4, 12, 0, 0)],
    )


def _insert_quality_history(store, dataset_id: str, check_name: str, values: list[float]) -> None:
    base = dt.datetime(2026, 7, 1, 9, 30, 0)
    for idx, value in enumerate(values):
        store.con.execute(
            """
            INSERT INTO data_quality_checks (
                check_id, dataset_id, table_name, check_name, status, severity,
                observed_value, threshold_value, details_json, checked_at
            )
            VALUES (?, ?, 'probe_table', ?, 'passed', 'warning', ?, 0, '{}', ?)
            """,
            [
                str(uuid.uuid4()),
                dataset_id,
                check_name,
                float(value),
                base + dt.timedelta(minutes=idx),
            ],
        )


def test_freshness_sla_emits_stale_row_and_routes_through_gate(tmp_store):
    from db.observability import evaluate_freshness_slas
    from db.quality import evaluate_quality_gate

    _insert_sla(tmp_store, "freshness_probe_stale", max_lag_days=10, severity="critical")
    _insert_sla(tmp_store, "freshness_probe_fresh", max_lag_days=10, severity="critical")
    _upsert_watermark(tmp_store, "freshness_probe_stale", "2026-06-01T00:00:00")
    _upsert_watermark(tmp_store, "freshness_probe_fresh", "2026-07-03T00:00:00")

    results = evaluate_freshness_slas(
        tmp_store,
        as_of=dt.datetime(2026, 7, 4, 12, 0, 0),
    )

    assert [result.dataset_id for result in results] == ["freshness_probe_stale"]
    assert results[0].status == "failed"
    assert results[0].severity == "critical"
    assert results[0].observed_value > 10

    recorded = tmp_store.con.execute(
        """
        SELECT status, severity, observed_value, threshold_value
        FROM data_quality_checks
        WHERE check_name = 'freshness_sla_freshness_probe_stale'
        """
    ).fetchone()
    assert recorded[0:2] == ("failed", "critical")
    assert recorded[2] > recorded[3]

    gate = evaluate_quality_gate(
        tmp_store,
        "freshness_probe_stale",
        record=False,
        additional_results=results,
    )
    assert gate.decision == "halt"
    assert gate.worst_severity == "critical"


def test_rowcount_anomaly_records_outlier_and_quality_event(tmp_store):
    from db.observability import detect_rowcount_anomalies

    _insert_quality_history(
        tmp_store,
        "rowcount_probe",
        "row_count_probe",
        [100, 101, 99, 100, 140],
    )

    anomalies = detect_rowcount_anomalies(
        tmp_store,
        window=4,
        z_threshold=3.5,
        severity="critical",
        dataset_ids=["rowcount_probe"],
    )

    assert len(anomalies) == 1
    assert anomalies[0].dataset_id == "rowcount_probe"
    assert anomalies[0].observed_value == 140
    assert anomalies[0].z_score > 3.5

    anomaly_row = tmp_store.con.execute(
        """
        SELECT dataset_id, is_anomaly, severity
        FROM data_quality_anomaly
        WHERE anomaly_id = ?
        """,
        [anomalies[0].anomaly_id],
    ).fetchone()
    assert anomaly_row == ("rowcount_probe", True, "critical")

    quality_row = tmp_store.con.execute(
        """
        SELECT status, severity
        FROM data_quality_checks
        WHERE check_name = ?
        ORDER BY checked_at DESC
        LIMIT 1
        """,
        [anomalies[0].check_name],
    ).fetchone()
    assert quality_row == ("failed", "critical")


def test_rowcount_anomaly_ignores_normal_series(tmp_store):
    from db.observability import detect_rowcount_anomalies

    _insert_quality_history(
        tmp_store,
        "rowcount_clean",
        "row_count_clean",
        [100, 101, 99, 100, 101],
    )

    anomalies = detect_rowcount_anomalies(
        tmp_store,
        window=4,
        z_threshold=3.5,
        severity="warning",
        dataset_ids=["rowcount_clean"],
    )

    assert anomalies == []
    assert tmp_store.con.execute(
        "SELECT count(*) FROM data_quality_anomaly WHERE dataset_id = 'rowcount_clean'"
    ).fetchone()[0] == 0
