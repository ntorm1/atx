"""0331 part 1: permanent identity registries and public clocked history.

The migration installs schema only. Retained reconstruction is a separate stage;
no current identifiers are backcast and an empty history is honestly empty.
0330 is reserved for the independent vintage layer (node 2.6).
"""
from __future__ import annotations

import json

import duckdb

from ._runner import Migration
from .bodies_0327 import _replace_rows_by_swap, refresh_schema_contract_pin_by_swap


def _identity_tables_v1():
    # These literals live inside a migration-local callable so the checksum
    # covers them. Runtime builders must remain schema-compatible with 0331.
    return {
        "security_permanent_ids": ("perm_security_id BIGINT NOT NULL, security_id VARCHAR NOT NULL, first_trade_date DATE, last_trade_date DATE, created_at TIMESTAMP NOT NULL", ("perm_security_id",)),
        "company_permanent_ids": ("perm_company_id BIGINT NOT NULL, cik VARCHAR, basis VARCHAR NOT NULL, created_at TIMESTAMP NOT NULL", ("perm_company_id",)),
        "security_company_links": ("perm_security_id BIGINT NOT NULL, perm_company_id BIGINT NOT NULL, cik VARCHAR, link_start DATE NOT NULL, link_end DATE, link_basis VARCHAR NOT NULL, link_primary VARCHAR NOT NULL, tier VARCHAR, available_at TIMESTAMP NOT NULL, evidence_ids VARCHAR, created_at TIMESTAMP NOT NULL, valid_until TIMESTAMP, class_status VARCHAR NOT NULL", ("perm_security_id", "perm_company_id", "link_start", "available_at")),
        "identity_unresolved_memberships": ("membership_id VARCHAR NOT NULL, perm_security_id BIGINT, perm_company_id BIGINT NOT NULL, cik VARCHAR NOT NULL, link_start DATE NOT NULL, link_end DATE, available_at TIMESTAMP NOT NULL, valid_until TIMESTAMP, evidence_ids VARCHAR NOT NULL, reason VARCHAR NOT NULL", ("membership_id",)),
        "security_names_history": ("perm_security_id BIGINT NOT NULL, ticker VARCHAR, issuer_name VARCHAR, venue VARCHAR, valid_from DATE NOT NULL, valid_to DATE, source VARCHAR NOT NULL, available_at TIMESTAMP NOT NULL", ("perm_security_id", "valid_from", "source", "available_at", "ticker", "issuer_name")),
        "identity_link_evidence": ("evidence_id VARCHAR NOT NULL, perm_security_id BIGINT, cik VARCHAR, source_file VARCHAR NOT NULL, source_sha256 VARCHAR NOT NULL, assertion VARCHAR NOT NULL, valid_from DATE, valid_to DATE, available_at TIMESTAMP NOT NULL, observed_at TIMESTAMP, evidence_kind VARCHAR NOT NULL, superseded_id VARCHAR, availability_basis VARCHAR NOT NULL", ("evidence_id",)),
    }


def _public_history_v1(con):
    """Event-keyed history; set joins avoid correlated DuckDB binder failures."""
    con.execute("""CREATE OR REPLACE VIEW v_security_master_public AS
    WITH events AS (
        SELECT perm_security_id, CAST(first_trade_date AS TIMESTAMP) + INTERVAL 22 HOUR AS event_at FROM security_permanent_ids
        UNION SELECT perm_security_id, CAST(last_trade_date AS TIMESTAMP) + INTERVAL 1 DAY + INTERVAL 22 HOUR FROM security_permanent_ids
        UNION SELECT perm_security_id, available_at FROM security_company_links
        UNION SELECT perm_security_id, valid_until FROM security_company_links WHERE valid_until IS NOT NULL
        UNION SELECT perm_security_id, greatest(available_at,CAST(link_start AS TIMESTAMP) + INTERVAL 22 HOUR) FROM security_company_links
        UNION SELECT perm_security_id, greatest(available_at,CAST(link_end AS TIMESTAMP) + INTERVAL 1 DAY + INTERVAL 22 HOUR) FROM security_company_links WHERE link_end IS NOT NULL
        UNION SELECT perm_security_id, available_at FROM security_names_history
        UNION SELECT perm_security_id, greatest(available_at,CAST(valid_from AS TIMESTAMP) + INTERVAL 22 HOUR) FROM security_names_history
        UNION SELECT perm_security_id, greatest(available_at,CAST(valid_to AS TIMESTAMP) + INTERVAL 1 DAY + INTERVAL 22 HOUR) FROM security_names_history WHERE valid_to IS NOT NULL
    ), spine AS (
        SELECT p.*,e.event_at FROM events e JOIN security_permanent_ids p USING(perm_security_id)
        WHERE CAST(p.first_trade_date AS TIMESTAMP) + INTERVAL 22 HOUR <= e.event_at
    ), all_links AS (
        SELECT e.perm_security_id,e.event_at,l.perm_company_id,l.cik,l.link_basis,l.tier,
            (l.tier IN ('high','medium') OR l.link_basis IN ('strict_dated','current_ticker_verified')) AS eligible
        FROM spine e JOIN security_company_links l ON l.perm_security_id=e.perm_security_id
         AND l.available_at<=e.event_at AND (l.valid_until IS NULL OR e.event_at<l.valid_until)
         AND l.link_start<=CAST(e.event_at AS DATE) AND (l.link_end IS NULL OR l.link_end>=CAST(e.event_at AS DATE))
    ), owners AS (
        SELECT perm_security_id,event_at,
            CASE WHEN count(DISTINCT perm_company_id)=1 THEN max(perm_company_id) FILTER(WHERE eligible) END AS perm_company_id,
            CASE WHEN count(DISTINCT perm_company_id)=1 THEN max(cik) FILTER(WHERE eligible) END AS cik,
            CASE WHEN count(DISTINCT perm_company_id)>1 THEN 'ambiguous' ELSE max(link_basis) FILTER(WHERE eligible) END AS link_basis,
            CASE WHEN count(DISTINCT perm_company_id)=1 THEN max(tier) FILTER(WHERE eligible) END AS tier
        FROM all_links GROUP BY perm_security_id,event_at
    ), names AS (
        SELECT e.perm_security_id,e.event_at,n.ticker,n.issuer_name,n.venue,n.available_at
        FROM spine e JOIN security_names_history n ON n.perm_security_id=e.perm_security_id
         AND n.available_at<=e.event_at AND n.valid_from<=CAST(e.event_at AS DATE)
         AND (n.valid_to IS NULL OR n.valid_to>=CAST(e.event_at AS DATE))
    ), tickers AS (
        SELECT * FROM names WHERE ticker IS NOT NULL QUALIFY dense_rank() OVER
            (PARTITION BY perm_security_id,event_at ORDER BY available_at DESC)=1
    ), issuers AS (
        SELECT * FROM names WHERE issuer_name IS NOT NULL QUALIFY dense_rank() OVER
            (PARTITION BY perm_security_id,event_at ORDER BY available_at DESC)=1
    ), venues AS (
        SELECT * FROM names WHERE venue IS NOT NULL QUALIFY dense_rank() OVER
            (PARTITION BY perm_security_id,event_at ORDER BY available_at DESC)=1
    ), ticker AS (
        SELECT perm_security_id,event_at,CASE WHEN count(DISTINCT ticker)=1 THEN max(ticker) END AS primary_symbol
        FROM tickers GROUP BY perm_security_id,event_at
    ), issuer AS (
        SELECT perm_security_id,event_at,CASE WHEN count(DISTINCT issuer_name)=1 THEN max(issuer_name) END AS name
        FROM issuers GROUP BY perm_security_id,event_at
    ), venue AS (
        SELECT perm_security_id,event_at,CASE WHEN count(DISTINCT venue)=1 THEN max(venue) END AS venue
        FROM venues GROUP BY perm_security_id,event_at
    ) SELECT p.security_id,p.perm_security_id,o.perm_company_id,
        CASE WHEN o.cik IS NOT NULL THEN 'CIK-' || o.cik END AS entity_id,
        CASE WHEN o.cik IS NOT NULL THEN 'CIK-' || o.cik END AS issuer_id,
        t.primary_symbol,n.name,v.venue,CAST(NULL AS VARCHAR) AS asset_class,
        CAST(NULL AS VARCHAR) AS country,CAST(NULL AS VARCHAR) AS currency,
        p.first_trade_date<=CAST(p.event_at AS DATE) AND p.last_trade_date>=CAST(p.event_at AS DATE) AS active,
        o.cik,CAST(NULL AS VARCHAR) AS lei,CAST(NULL AS VARCHAR) AS figi,o.link_basis,o.tier,
        CAST(p.event_at AS DATE) AS as_of_date,p.event_at AS available_at,
        'identity_history_reconstructed_v1' AS source,CAST(NULL AS VARCHAR) AS run_id,p.created_at AS source_loaded_at
    FROM spine p LEFT JOIN owners o USING(perm_security_id,event_at)
    LEFT JOIN ticker t USING(perm_security_id,event_at)
    LEFT JOIN issuer n USING(perm_security_id,event_at)
    LEFT JOIN venue v USING(perm_security_id,event_at)""")


def _identity_catalog_v1(conn, descriptions, keys):
    """Insert-only replacements preserve existing catalog keys/defaults/indexes."""
    names = ",".join("'" + name + "'" for name in descriptions)
    conn.execute("CREATE TEMP TABLE identity_catalog_0331 AS SELECT * FROM table_catalog WHERE false")
    conn.executemany("""INSERT INTO identity_catalog_0331
        (table_name,layer,entity,grain,description,natural_key_json,pit_notes,created_at,updated_at)
        VALUES (?,?,'security',?,?,?,?,now(),now())""", [
        (name, "serving" if name.startswith("v_") else "silver", ",".join(keys[name]), description,
         json.dumps(keys[name]), "Reconstructed history is unverified; filter business dates and evidence clocks independently.")
        for name, description in descriptions.items()])
    table_columns = ("table_name","layer","entity","grain","description","natural_key_json","pit_notes","created_at","updated_at")
    _replace_rows_by_swap(conn, "table_catalog", table_columns,
        f"SELECT * FROM table_catalog WHERE table_name NOT IN ({names}) UNION ALL SELECT * FROM identity_catalog_0331")
    fields = ("table_name","field_name","semantic_type","description","nullable","unit","source_field","created_at","updated_at")
    _replace_rows_by_swap(conn, "field_catalog", fields, f"""
        SELECT * FROM field_catalog WHERE table_name NOT IN ({names})
        UNION ALL SELECT table_name,column_name,
            CASE WHEN data_type LIKE '%TIMESTAMP%' THEN 'timestamp'
                 WHEN data_type = 'DATE' THEN 'date'
                 WHEN column_name LIKE '%_id' OR column_name='cik' THEN 'identifier' ELSE 'text' END,
            CASE WHEN column_name='available_at' THEN 'Evidence availability clock; reconstructed clocks are modeled and labeled.'
                 WHEN column_name='valid_until' THEN 'Exclusive evidence-version end, separate from inclusive business end.'
                 ELSE replace(column_name,'_',' ') || ' on ' || table_name END,
            is_nullable,NULL,column_name,CAST(now() AS TIMESTAMP),CAST(now() AS TIMESTAMP)
        FROM duckdb_columns() WHERE table_name IN ({names})""")
    conn.execute("DROP TABLE identity_catalog_0331")


def _permanent_identity_history(conn: duckdb.DuckDBPyConnection) -> None:
    tables = _identity_tables_v1()
    for name, (columns, _) in tables.items():
        conn.execute(f"CREATE TABLE IF NOT EXISTS {name} ({columns})")
    _public_history_v1(conn)
    descriptions = {
        "security_permanent_ids": "Permanent positive vendor-line IDs, never ticker IDs and never recycled.",
        "company_permanent_ids": "Permanent SEC CIK company IDs, independent of company names.",
        "security_company_links": "Clocked company membership; inclusive business interval and exclusive evidence valid_until.",
        "identity_unresolved_memberships": "Possible company classes withheld from complete company ME until resolved.",
        "security_names_history": "Dated symbols, names and venues; current snapshots never backfilled.",
        "identity_link_evidence": "Content-pinned assertions with separate modeled availability and observation clocks.",
        "v_security_master_public": "Permanent-line event history; unavailable historical descriptors remain NULL.",
    }
    keys = {name: values[1] for name, values in tables.items()}
    keys["v_security_master_public"] = ("security_id", "available_at")
    _identity_catalog_v1(conn, descriptions, keys)
    refresh_schema_contract_pin_by_swap(conn)


MIGRATIONS = [Migration(version=331, name="permanent_identity_history", up=_permanent_identity_history)]
