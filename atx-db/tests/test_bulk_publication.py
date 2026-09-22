from __future__ import annotations

import pytest

from atx_db._bulk_publication import publish_validated_shadow


def test_publish_validated_shadow_swaps_all_rows_and_related_writes(tmp_store):
    tmp_store.con.execute("CREATE TABLE publication_live (value VARCHAR NOT NULL)")
    tmp_store.con.execute("CREATE TABLE publication_shadow (value VARCHAR NOT NULL)")
    tmp_store.con.execute("CREATE TABLE publication_ledger (value VARCHAR NOT NULL)")
    tmp_store.con.execute("INSERT INTO publication_live VALUES ('old')")
    tmp_store.con.execute("INSERT INTO publication_shadow VALUES ('new')")

    publish_validated_shadow(
        tmp_store,
        live_table="publication_live",
        shadow_table="publication_shadow",
        before_swap=lambda: tmp_store.con.execute("INSERT INTO publication_ledger VALUES ('new')"),
    )

    assert tmp_store.con.execute("SELECT * FROM publication_live").fetchall() == [("new",)]
    assert tmp_store.con.execute("SELECT * FROM publication_ledger").fetchall() == [("new",)]
    assert tmp_store.con.execute(
        "SELECT count(*) FROM information_schema.tables WHERE table_name IN "
        "('publication_shadow', 'publication_live_bulk_previous')"
    ).fetchone() == (0,)


def test_publish_validated_shadow_rolls_back_related_writes_and_keeps_shadow(tmp_store):
    tmp_store.con.execute("CREATE TABLE failed_live (value VARCHAR NOT NULL)")
    tmp_store.con.execute("CREATE TABLE failed_shadow (value VARCHAR NOT NULL)")
    tmp_store.con.execute("CREATE TABLE failed_ledger (value VARCHAR NOT NULL)")
    tmp_store.con.execute("INSERT INTO failed_live VALUES ('old')")
    tmp_store.con.execute("INSERT INTO failed_shadow VALUES ('new')")

    def fail_after_related_write() -> None:
        tmp_store.con.execute("INSERT INTO failed_ledger VALUES ('new')")
        raise RuntimeError("injected publication failure")

    with pytest.raises(RuntimeError, match="injected publication failure"):
        publish_validated_shadow(
            tmp_store,
            live_table="failed_live",
            shadow_table="failed_shadow",
            before_swap=fail_after_related_write,
        )

    assert tmp_store.con.execute("SELECT * FROM failed_live").fetchall() == [("old",)]
    assert tmp_store.con.execute("SELECT * FROM failed_shadow").fetchall() == [("new",)]
    assert tmp_store.con.execute("SELECT * FROM failed_ledger").fetchall() == []


def test_publish_validated_shadow_rejects_weakened_contract(tmp_store):
    tmp_store.con.execute("CREATE TABLE contract_live (id VARCHAR PRIMARY KEY, value VARCHAR NOT NULL DEFAULT 'old')")
    tmp_store.con.execute("CREATE TABLE contract_shadow (id VARCHAR, value VARCHAR)")

    with pytest.raises(RuntimeError, match="shadow contract differs"):
        publish_validated_shadow(
            tmp_store,
            live_table="contract_live",
            shadow_table="contract_shadow",
        )


def test_publish_validated_shadow_updates_existing_view_reader(tmp_store):
    tmp_store.con.execute("CREATE TABLE reader_live (value VARCHAR NOT NULL)")
    tmp_store.con.execute("CREATE TABLE reader_shadow (value VARCHAR NOT NULL)")
    tmp_store.con.execute("INSERT INTO reader_live VALUES ('old')")
    tmp_store.con.execute("INSERT INTO reader_shadow VALUES ('new')")
    tmp_store.con.execute("CREATE VIEW reader_view AS SELECT value FROM reader_live")

    publish_validated_shadow(
        tmp_store,
        live_table="reader_live",
        shadow_table="reader_shadow",
    )

    assert tmp_store.con.execute("SELECT * FROM reader_view").fetchall() == [("new",)]
