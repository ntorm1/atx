from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


# ---------------------------------------------------------------------------
# Seed helpers
# ---------------------------------------------------------------------------

def _insert_manager(
    tmp_store,
    *,
    manager_id: str,
    cik: str,
    manager_name: str,
    first_filing_date: dt.date = dt.date(2019, 2, 14),
    last_filing_date: dt.date = dt.date(2024, 2, 14),
    first_report_period: dt.date = dt.date(2018, 12, 31),
    last_report_period: dt.date = dt.date(2023, 12, 31),
) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO thirteenf_managers (
            manager_id, cik, manager_name, city, state_or_country,
            crd_number, sec_file_number, first_report_period, last_report_period,
            first_filing_date, last_filing_date, filing_count, amendment_count,
            source_period_count, source
        )
        VALUES (?, ?, ?, 'NEW YORK', 'NY', NULL, NULL, ?, ?, ?, ?, 4, 0, 4, 'test')
        """,
        [
            manager_id, cik, manager_name, first_report_period, last_report_period,
            first_filing_date, last_filing_date,
        ],
    )


def _insert_report(
    tmp_store,
    *,
    manager_report_id: str,
    manager_id: str,
    cik: str,
    filing_manager_name: str,
    report_period: dt.date,
    filing_date: dt.date,
) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO thirteenf_manager_reports (
            manager_report_id, manager_id, accession_number, cik, report_period,
            filing_date, source_period, submission_type, filing_manager_name, source
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, '13F-HR', ?, 'test')
        """,
        [
            manager_report_id, manager_id, f"acc-{manager_report_id}", cik,
            report_period, filing_date, "01jan-31mar", filing_manager_name,
        ],
    )


# ---------------------------------------------------------------------------
# normalize_filer_name
# ---------------------------------------------------------------------------

def test_normalize_strips_legal_form_suffix():
    from atx_db.filer_alias import normalize_filer_name

    assert normalize_filer_name("Renaissance Technologies LLC") == "RENAISSANCE TECHNOLOGIES"
    assert normalize_filer_name("BERKSHIRE HATHAWAY INC") == "BERKSHIRE HATHAWAY"
    assert normalize_filer_name("Citadel Advisors L.L.C.") == "CITADEL ADVISORS"


def test_normalize_keeps_business_words_conservative():
    from atx_db.filer_alias import normalize_filer_name

    # "CAPITAL"/"MANAGEMENT" are business words, not legal-form suffixes: keep them
    # so unrelated "Acme Capital" and "Acme Partners" never collapse together.
    assert normalize_filer_name("ACME CAPITAL MANAGEMENT LP") == "ACME CAPITAL MANAGEMENT"


def test_normalize_collapses_punctuation_and_whitespace():
    from atx_db.filer_alias import normalize_filer_name

    assert normalize_filer_name("  Smith  &   Co.  ") == "SMITH"
    assert normalize_filer_name("") == ""
    assert normalize_filer_name(None) == ""


# ---------------------------------------------------------------------------
# SELF rows
# ---------------------------------------------------------------------------

def test_self_alias_row_per_cik(tmp_store):
    from atx_db.filer_alias import refresh_filer_aliases, FilerAliasOptions

    _insert_manager(tmp_store, manager_id="m1", cik="0000001", manager_name="Renaissance Technologies LLC")
    _insert_manager(tmp_store, manager_id="m2", cik="0000002", manager_name="Bridgewater Associates LP")

    refresh_filer_aliases(tmp_store, FilerAliasOptions())

    rows = tmp_store.con.execute(
        """
        SELECT alias_cik, primary_cik, normalized_name, confidence, is_current, valid_to
        FROM filer_13f_cik_alias
        WHERE alias_type = 'SELF'
        ORDER BY alias_cik
        """
    ).fetchall()
    assert rows == [
        ("0000001", "0000001", "RENAISSANCE TECHNOLOGIES", 1.0, True, None),
        ("0000002", "0000002", "BRIDGEWATER ASSOCIATES", 1.0, True, None),
    ]


# ---------------------------------------------------------------------------
# NAME_HISTORY rows (intra-CIK rename)
# ---------------------------------------------------------------------------

def test_name_history_emitted_on_intra_cik_rename(tmp_store):
    from atx_db.filer_alias import refresh_filer_aliases, FilerAliasOptions

    _insert_manager(
        tmp_store, manager_id="m1", cik="0000010", manager_name="NewCo Management LLC",
        first_filing_date=dt.date(2019, 2, 14),
    )
    # Two distinct reported names over time for the same CIK.
    _insert_report(
        tmp_store, manager_report_id="r1", manager_id="m1", cik="0000010",
        filing_manager_name="OldCo Management LLC",
        report_period=dt.date(2018, 12, 31), filing_date=dt.date(2019, 2, 14),
    )
    _insert_report(
        tmp_store, manager_report_id="r2", manager_id="m1", cik="0000010",
        filing_manager_name="NewCo Management LLC",
        report_period=dt.date(2021, 12, 31), filing_date=dt.date(2022, 2, 14),
    )

    refresh_filer_aliases(tmp_store, FilerAliasOptions())

    hist = tmp_store.con.execute(
        """
        SELECT normalized_name, is_current, valid_from, valid_to
        FROM filer_13f_cik_alias
        WHERE alias_cik = '0000010' AND alias_type = 'NAME_HISTORY'
        ORDER BY valid_from
        """
    ).fetchall()
    # The superseded prior name is a closed, non-current history row.
    assert len(hist) == 1
    assert hist[0][0] == "OLDCO MANAGEMENT"
    assert hist[0][1] is False
    assert hist[0][3] is not None  # valid_to closed


# ---------------------------------------------------------------------------
# NAME_MATCH_CANDIDATE rows (cross-CIK, low confidence, no auto-merge)
# ---------------------------------------------------------------------------

def test_name_match_candidate_links_duplicate_names_low_confidence(tmp_store):
    from atx_db.filer_alias import refresh_filer_aliases, FilerAliasOptions

    _insert_manager(tmp_store, manager_id="m1", cik="0000100", manager_name="Acme Capital LLC")
    _insert_manager(tmp_store, manager_id="m2", cik="0000200", manager_name="ACME CAPITAL L.L.C.")

    refresh_filer_aliases(tmp_store, FilerAliasOptions())

    cand = tmp_store.con.execute(
        """
        SELECT alias_cik, primary_cik, confidence, cluster_key
        FROM filer_13f_cik_alias
        WHERE alias_type = 'NAME_MATCH_CANDIDATE'
        ORDER BY alias_cik
        """
    ).fetchall()
    # One candidate row links the non-representative CIK to the representative
    # (lexicographically smallest CIK in the cluster). 0.5 confidence = not authoritative.
    assert cand == [("0000200", "0000100", 0.5, "ACME CAPITAL")]


def test_resolve_primary_cik_default_ignores_candidates(tmp_store):
    from atx_db.filer_alias import refresh_filer_aliases, FilerAliasOptions, resolve_primary_cik

    _insert_manager(tmp_store, manager_id="m1", cik="0000100", manager_name="Acme Capital LLC")
    _insert_manager(tmp_store, manager_id="m2", cik="0000200", manager_name="ACME CAPITAL L.L.C.")
    refresh_filer_aliases(tmp_store, FilerAliasOptions())

    # Default min_confidence=1.0 -> the 0.5 candidate is ignored, no false merge.
    assert resolve_primary_cik(tmp_store, "0000200", dt.date(2024, 6, 1)) == "0000200"
    # Opting into candidates rolls up to the representative.
    assert resolve_primary_cik(tmp_store, "0000200", dt.date(2024, 6, 1), min_confidence=0.5) == "0000100"


# ---------------------------------------------------------------------------
# Injectable seed (SUBADVISOR / MA_CONTINUITY)
# ---------------------------------------------------------------------------

def test_seed_file_injects_ma_continuity_rollup(tmp_store, tmp_path):
    from atx_db.filer_alias import refresh_filer_aliases, FilerAliasOptions, resolve_primary_cik

    _insert_manager(tmp_store, manager_id="m1", cik="0000300", manager_name="Parent Asset Mgmt LLC")
    _insert_manager(tmp_store, manager_id="m2", cik="0000400", manager_name="Acquired Advisors LP")

    seed = tmp_path / "seed.csv"
    seed.write_text(
        "parent_cik,child_cik,alias_type,valid_from,confidence,evidence\n"
        "0000300,0000400,MA_CONTINUITY,2022-01-01,1.0,2022 acquisition 8-K\n",
        encoding="utf-8",
    )

    refresh_filer_aliases(tmp_store, FilerAliasOptions(seed_file=seed))

    # Authoritative rollup at default confidence.
    assert resolve_primary_cik(tmp_store, "0000400", dt.date(2024, 6, 1)) == "0000300"
    # Before the continuity effective date, the child resolves to itself.
    assert resolve_primary_cik(tmp_store, "0000400", dt.date(2021, 6, 1)) == "0000400"


# ---------------------------------------------------------------------------
# Dataset.run + quality + bitemporal invariants
# ---------------------------------------------------------------------------

def test_dataset_run_records_quality_check(tmp_store):
    from atx_db.filer_alias import FilerAliasDataset, FilerAliasOptions

    _insert_manager(tmp_store, manager_id="m1", cik="0000001", manager_name="Renaissance Technologies LLC")

    result = FilerAliasDataset().run(tmp_store, FilerAliasOptions())
    assert result.rows_loaded >= 1

    checks = tmp_store.con.execute(
        "SELECT count(*) FROM data_quality_checks WHERE dataset_id = 'filer_13f_cik_alias'"
    ).fetchone()[0]
    assert checks >= 1


def test_no_overlapping_authoritative_windows_per_alias_cik(tmp_store):
    from atx_db.filer_alias import refresh_filer_aliases, FilerAliasOptions

    _insert_manager(tmp_store, manager_id="m1", cik="0000010", manager_name="NewCo Management LLC")
    _insert_report(
        tmp_store, manager_report_id="r1", manager_id="m1", cik="0000010",
        filing_manager_name="OldCo Management LLC",
        report_period=dt.date(2018, 12, 31), filing_date=dt.date(2019, 2, 14),
    )
    _insert_report(
        tmp_store, manager_report_id="r2", manager_id="m1", cik="0000010",
        filing_manager_name="NewCo Management LLC",
        report_period=dt.date(2021, 12, 31), filing_date=dt.date(2022, 2, 14),
    )
    refresh_filer_aliases(tmp_store, FilerAliasOptions())

    # Authoritative spine (SELF + seed rollups) must not have overlapping validity
    # windows for the same alias_cik. NAME_HISTORY/NAME_MATCH_CANDIDATE are excluded
    # from this invariant by design.
    overlaps = tmp_store.con.execute(
        """
        WITH auth AS (
            SELECT alias_id, alias_cik, valid_from,
                   coalesce(valid_to, DATE '9999-12-31') AS valid_to
            FROM filer_13f_cik_alias
            WHERE alias_type IN ('SELF', 'SUBADVISOR', 'MA_CONTINUITY', 'MANUAL')
        )
        SELECT count(*)
        FROM auth a JOIN auth b
          ON a.alias_cik = b.alias_cik
         AND a.valid_from < b.valid_to
         AND b.valid_from < a.valid_to
         AND a.alias_id <> b.alias_id
        """
    ).fetchone()[0]
    assert overlaps == 0


def test_refresh_is_idempotent(tmp_store):
    from atx_db.filer_alias import refresh_filer_aliases, FilerAliasOptions

    _insert_manager(tmp_store, manager_id="m1", cik="0000001", manager_name="Renaissance Technologies LLC")

    n1 = refresh_filer_aliases(tmp_store, FilerAliasOptions())
    n2 = refresh_filer_aliases(tmp_store, FilerAliasOptions())
    assert n1 == n2
    total = tmp_store.con.execute("SELECT count(*) FROM filer_13f_cik_alias").fetchone()[0]
    assert total == n2
