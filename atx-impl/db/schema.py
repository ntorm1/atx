from __future__ import annotations

import json

from .connection import DuckDBStore
from .fundamental_statements import seed_fundamental_statement_map
from .parity import seed_provider_parity_matrix


def ensure_quant_schema(store: DuckDBStore) -> None:
    """Create the quant warehouse tables shared by public-data datasets."""

    con = store.con
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version VARCHAR PRIMARY KEY,
            description VARCHAR NOT NULL,
            checksum VARCHAR,
            applied_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS source_systems (
            source_system_id VARCHAR PRIMARY KEY,
            name VARCHAR NOT NULL,
            base_url VARCHAR,
            license_note VARCHAR,
            cadence VARCHAR,
            requires_key BOOLEAN NOT NULL DEFAULT false,
            metadata_json VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS dataset_catalog (
            dataset_id VARCHAR PRIMARY KEY,
            source_system_id VARCHAR,
            name VARCHAR NOT NULL,
            description VARCHAR,
            grain VARCHAR,
            primary_table VARCHAR,
            pit_column VARCHAR,
            available_at_column VARCHAR,
            owner VARCHAR,
            metadata_json VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS table_catalog (
            table_name VARCHAR PRIMARY KEY,
            layer VARCHAR NOT NULL,
            entity VARCHAR,
            grain VARCHAR,
            description VARCHAR,
            natural_key_json VARCHAR,
            pit_notes VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS field_catalog (
            table_name VARCHAR NOT NULL,
            field_name VARCHAR NOT NULL,
            semantic_type VARCHAR,
            description VARCHAR,
            nullable BOOLEAN,
            unit VARCHAR,
            source_field VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (table_name, field_name)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS provider_parity_matrix (
            provider VARCHAR NOT NULL,
            provider_domain VARCHAR NOT NULL,
            warehouse_domain VARCHAR NOT NULL,
            reference_tables_json VARCHAR NOT NULL,
            institutional_grain VARCHAR NOT NULL,
            institutional_keys_json VARCHAR NOT NULL,
            pit_fields_json VARCHAR NOT NULL,
            factors_or_fields_json VARCHAR NOT NULL,
            open_substitute VARCHAR NOT NULL,
            warehouse_tables_json VARCHAR NOT NULL,
            parity_status VARCHAR NOT NULL,
            limitations VARCHAR,
            next_gap VARCHAR,
            source_urls_json VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (provider, provider_domain)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS raw_source_files (
            source_id VARCHAR PRIMARY KEY,
            dataset_id VARCHAR NOT NULL,
            source_url VARCHAR NOT NULL,
            cache_path VARCHAR,
            sha256 VARCHAR,
            byte_count BIGINT,
            fetched_at TIMESTAMP NOT NULL DEFAULT now(),
            status VARCHAR NOT NULL,
            metadata_json VARCHAR
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS nasdaq_symbol_directory (
            directory VARCHAR NOT NULL,
            symbol VARCHAR NOT NULL,
            security_name VARCHAR,
            market_category VARCHAR,
            exchange VARCHAR,
            cqs_symbol VARCHAR,
            etf BOOLEAN,
            test_issue BOOLEAN,
            financial_status VARCHAR,
            round_lot_size INTEGER,
            next_shares BOOLEAN,
            nasdaq_symbol VARCHAR,
            as_of_date DATE NOT NULL,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS nasdaq_listing_events (
            event_id VARCHAR PRIMARY KEY,
            symbol VARCHAR NOT NULL,
            security_id VARCHAR,
            company_name VARCHAR,
            nasdaq_action VARCHAR,
            bx_action VARCHAR,
            psx_action VARCHAR,
            effective_date DATE,
            primary_listing_market VARCHAR,
            as_of_date DATE NOT NULL,
            source_file_created_at TIMESTAMP,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS sec_submissions (
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            filing_date DATE,
            report_date DATE,
            acceptance_datetime TIMESTAMP,
            form VARCHAR,
            primary_document VARCHAR,
            primary_doc_description VARCHAR,
            file_number VARCHAR,
            film_number VARCHAR,
            items VARCHAR,
            size BIGINT,
            is_xbrl BOOLEAN,
            is_inline_xbrl BOOLEAN,
            act VARCHAR,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS macro_series (
            source VARCHAR NOT NULL,
            series_id VARCHAR NOT NULL,
            title VARCHAR,
            frequency VARCHAR,
            units VARCHAR,
            seasonal_adjustment VARCHAR,
            notes VARCHAR,
            source_url VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS data_quality_checks (
            check_id VARCHAR PRIMARY KEY,
            dataset_id VARCHAR NOT NULL,
            table_name VARCHAR NOT NULL,
            check_name VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            observed_value DOUBLE,
            threshold_value DOUBLE,
            details_json VARCHAR,
            checked_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS lake_export_runs (
            export_run_id VARCHAR PRIMARY KEY,
            lake_root VARCHAR NOT NULL,
            object_count BIGINT NOT NULL,
            total_rows BIGINT,
            total_byte_count BIGINT,
            started_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP,
            status VARCHAR NOT NULL,
            error_message VARCHAR,
            params_json VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS lake_export_files (
            export_run_id VARCHAR NOT NULL,
            object_name VARCHAR NOT NULL,
            output_path VARCHAR NOT NULL,
            manifest_path VARCHAR NOT NULL,
            rows BIGINT NOT NULL,
            byte_count BIGINT NOT NULL,
            sha256 VARCHAR NOT NULL,
            schema_sha256 VARCHAR NOT NULL,
            format VARCHAR NOT NULL,
            compression VARCHAR,
            exported_at TIMESTAMP NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS finra_short_interest_backfill_manifests (
            manifest_id VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            start_date DATE NOT NULL,
            end_date DATE NOT NULL,
            date_order VARCHAR NOT NULL,
            limit_dates INTEGER NOT NULL,
            skip_existing_min_symbols INTEGER,
            force BOOLEAN NOT NULL,
            candidate_count BIGINT NOT NULL,
            selected_date_count BIGINT NOT NULL,
            loaded_date_count BIGINT NOT NULL,
            source_row_count BIGINT NOT NULL,
            feature_row_count BIGINT,
            selected_dates_json VARCHAR NOT NULL,
            candidates_json VARCHAR NOT NULL,
            load_results_json VARCHAR NOT NULL,
            feature_result_json VARCHAR,
            watermarks_json VARCHAR,
            params_json VARCHAR,
            source VARCHAR NOT NULL,
            started_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS securities (
            security_id VARCHAR PRIMARY KEY,
            entity_id VARCHAR,
            issuer_id VARCHAR,
            primary_symbol VARCHAR,
            name VARCHAR,
            asset_class VARCHAR NOT NULL DEFAULT 'EQUITY',
            country VARCHAR NOT NULL DEFAULT 'US',
            currency VARCHAR NOT NULL DEFAULT 'USD',
            active BOOLEAN NOT NULL DEFAULT true,
            first_seen_date DATE,
            last_seen_date DATE,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
            CREATE TABLE IF NOT EXISTS security_identifier_history (
                security_id VARCHAR NOT NULL,
                id_type VARCHAR NOT NULL,
                id_value VARCHAR NOT NULL,
                internal_cusip VARCHAR,
                valid_from DATE NOT NULL,
                valid_to DATE,
                as_of_date DATE NOT NULL,
                available_at TIMESTAMP,
                source VARCHAR NOT NULL,
                run_id VARCHAR,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
            """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS exchange_listings (
            security_id VARCHAR NOT NULL,
            ticker VARCHAR NOT NULL,
            exchange_code VARCHAR,
            mic VARCHAR,
            currency VARCHAR NOT NULL DEFAULT 'USD',
                valid_from DATE NOT NULL,
                valid_to DATE,
                as_of_date DATE NOT NULL,
                available_at TIMESTAMP,
                source VARCHAR NOT NULL,
                run_id VARCHAR,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
            """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS listing_status_intervals (
            listing_status_id VARCHAR PRIMARY KEY,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            listing_venue_code VARCHAR,
            listing_venue_name VARCHAR,
            listing_exchange_code VARCHAR,
            status VARCHAR NOT NULL,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            last_evidence_as_of_date DATE,
            last_evidence_at TIMESTAMP,
            source VARCHAR NOT NULL,
            evidence_source VARCHAR NOT NULL,
            evidence_source_table VARCHAR NOT NULL,
            source_event_id VARCHAR,
            source_snapshot_directory VARCHAR,
            source_url VARCHAR,
            method VARCHAR NOT NULL,
            details_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS delist_code_dim (
            delist_code VARCHAR PRIMARY KEY,
            code_system VARCHAR NOT NULL,
            vendor_code VARCHAR,
            crsp_dlstcd INTEGER,
            crsp_dlstcd_family VARCHAR,
            reason_category VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            terminal_trading_status VARCHAR,
            imputation_allowed BOOLEAN NOT NULL DEFAULT false,
            default_imputed_return DOUBLE,
            imputation_policy VARCHAR NOT NULL DEFAULT 'none',
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS delisting_events (
            delisting_event_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            listing_status_source VARCHAR NOT NULL,
            source_listing_status_id VARCHAR NOT NULL,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            listing_venue_code VARCHAR,
            listing_venue_name VARCHAR,
            listing_exchange_code VARCHAR,
            delist_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            delist_code VARCHAR NOT NULL,
            delist_reason VARCHAR NOT NULL,
            delisting_return DOUBLE,
            delisting_return_type VARCHAR NOT NULL,
            is_return_imputed BOOLEAN NOT NULL DEFAULT false,
            return_policy VARCHAR NOT NULL,
            return_confidence VARCHAR NOT NULL,
            return_observation_id VARCHAR,
            return_observation_source VARCHAR,
            return_observation_provider VARCHAR,
            evidence_source VARCHAR NOT NULL,
            evidence_source_table VARCHAR NOT NULL,
            source_event_id VARCHAR,
            source_url VARCHAR,
            method VARCHAR NOT NULL,
            evidence_confidence VARCHAR NOT NULL,
            inferred_from_absence BOOLEAN NOT NULL DEFAULT false,
            details_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS delisting_return_observations (
            delisting_return_observation_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            provider VARCHAR NOT NULL,
            source_file VARCHAR,
            source_file_sha256 VARCHAR,
            security_id VARCHAR,
            symbol VARCHAR,
            vendor_security_id VARCHAR,
            vendor_security_id_type VARCHAR,
            delist_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            delist_code VARCHAR,
            vendor_delist_code VARCHAR,
            crsp_dlstcd INTEGER,
            delist_amount DOUBLE,
            delist_price DOUBLE,
            delisting_return DOUBLE NOT NULL,
            delisting_return_ex_div DOUBLE,
            delist_pay_date DATE,
            next_pricing_date DATE,
            successor_security_id VARCHAR,
            successor_vendor_security_id VARCHAR,
            return_basis VARCHAR NOT NULL,
            currency VARCHAR,
            raw_payload_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS sec_company_tickers (
            cik VARCHAR NOT NULL,
            ticker VARCHAR NOT NULL,
            title VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS identifier_resolution_candidates (
            candidate_id VARCHAR PRIMARY KEY,
            source_dataset_id VARCHAR NOT NULL,
            source_table VARCHAR NOT NULL,
            source_period VARCHAR,
            source_key_type VARCHAR NOT NULL,
            source_key_value VARCHAR NOT NULL,
            source_security_id VARCHAR,
            source_name VARCHAR,
            source_normalized_name VARCHAR,
            target_security_id VARCHAR NOT NULL,
            target_id_type VARCHAR,
            target_id_value VARCHAR,
            target_name VARCHAR,
            target_normalized_name VARCHAR,
            match_method VARCHAR NOT NULL,
            confidence DOUBLE NOT NULL,
            candidate_status VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            details_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS identifier_resolution_decisions (
            decision_id VARCHAR PRIMARY KEY,
            candidate_id VARCHAR NOT NULL,
            source_dataset_id VARCHAR NOT NULL,
            source_table VARCHAR NOT NULL,
            source_period VARCHAR,
            source_key_type VARCHAR NOT NULL,
            source_key_value VARCHAR NOT NULL,
            source_security_id VARCHAR,
            target_security_id VARCHAR NOT NULL,
            target_id_type VARCHAR,
            target_id_value VARCHAR,
            match_method VARCHAR,
            confidence DOUBLE NOT NULL,
            candidate_status VARCHAR NOT NULL,
            decision_status VARCHAR NOT NULL,
            decision_method VARCHAR NOT NULL,
            decided_by VARCHAR NOT NULL,
            decided_at TIMESTAMP NOT NULL,
            effective_from DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            notes_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS equity_daily_bars (
            source VARCHAR NOT NULL,
                security_id VARCHAR NOT NULL,
                vendor_security_id VARCHAR,
                symbol VARCHAR NOT NULL,
                trade_date DATE NOT NULL,
                open DOUBLE,
            high DOUBLE,
            low DOUBLE,
            close DOUBLE,
            adjusted_close DOUBLE,
            volume BIGINT,
            vwap DOUBLE,
                dividend_amount DOUBLE,
                split_factor DOUBLE,
                is_adjusted BOOLEAN NOT NULL DEFAULT false,
                available_at TIMESTAMP,
                run_id VARCHAR,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
            """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS corporate_actions (
                source VARCHAR NOT NULL,
                security_id VARCHAR NOT NULL,
                symbol VARCHAR,
            action_type VARCHAR NOT NULL,
            ex_date DATE NOT NULL,
            declaration_date DATE,
            record_date DATE,
            payable_date DATE,
            cash_amount DOUBLE,
            split_from DOUBLE,
                split_to DOUBLE,
                adjustment_factor DOUBLE,
                details_json VARCHAR,
                available_at TIMESTAMP,
                run_id VARCHAR,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS corp_action_type_dim (
            type_code INTEGER PRIMARY KEY,
            event_type VARCHAR NOT NULL,
            category VARCHAR NOT NULL,
            sub_category VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            crsp_distcd INTEGER,
            dtcc_caev VARCHAR,
            bloomberg_type VARCHAR,
            factset_type VARCHAR,
            affects_price BOOLEAN NOT NULL,
            affects_shares BOOLEAN NOT NULL,
            taxable BOOLEAN,
            mandatory BOOLEAN NOT NULL DEFAULT true,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS adjustment_factor_history (
            adjustment_factor_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            source_action_source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            ex_date DATE NOT NULL,
            event_type VARCHAR NOT NULL,
            type_code INTEGER NOT NULL,
            event_ref_id VARCHAR NOT NULL,
            factor_price DOUBLE NOT NULL,
            factor_shares DOUBLE NOT NULL,
            factor_volume DOUBLE NOT NULL,
            ratio_numerator DOUBLE,
            ratio_denominator DOUBLE,
            cash_div_amount DOUBLE,
            cash_div_currency VARCHAR,
            cumulative_price_factor DOUBLE,
            cumulative_share_factor DOUBLE,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            classification_reason VARCHAR
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS daily_adjustment_factors (
            daily_adjustment_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            bar_source VARCHAR NOT NULL,
            factor_source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            trade_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            split_price_factor DOUBLE NOT NULL,
            split_share_factor DOUBLE NOT NULL,
            dividend_total_return_factor DOUBLE NOT NULL,
            total_return_price_factor DOUBLE NOT NULL,
            raw_close DOUBLE NOT NULL,
            split_adjusted_close DOUBLE NOT NULL,
            total_return_adjusted_close DOUBLE NOT NULL,
            raw_volume BIGINT,
            split_adjusted_volume DOUBLE,
            visible_event_count INTEGER NOT NULL,
            split_event_count INTEGER NOT NULL,
            cash_div_event_count INTEGER NOT NULL,
            last_factor_ex_date DATE,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS shares_outstanding_history (
            share_history_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            share_count_type VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            effective_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            form VARCHAR,
            accession_number VARCHAR NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            share_count DOUBLE NOT NULL,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS sec_company_facts (
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            label VARCHAR,
            description VARCHAR,
            unit VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE,
            filed_date DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            form VARCHAR,
            accession_number VARCHAR,
                frame VARCHAR,
                value DOUBLE,
                available_at TIMESTAMP,
                run_id VARCHAR,
                source_url VARCHAR NOT NULL,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_concept_catalog (
            source VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            label VARCHAR,
            description VARCHAR,
            statement_category VARCHAR,
            units_json VARCHAR NOT NULL,
            forms_json VARCHAR NOT NULL,
            fiscal_periods_json VARCHAR NOT NULL,
            first_period_end DATE,
            last_period_end DATE,
            first_filed_date DATE,
            last_filed_date DATE,
            first_available_at TIMESTAMP,
            last_available_at TIMESTAMP,
            fact_count BIGINT NOT NULL,
            security_count BIGINT NOT NULL,
            accession_count BIGINT NOT NULL,
            latest_source_loaded_at TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (source, taxonomy, concept)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_taxonomy_packages (
            taxonomy_package_id VARCHAR PRIMARY KEY,
            taxonomy VARCHAR NOT NULL,
            release_year INTEGER NOT NULL,
            source_url VARCHAR NOT NULL,
            package_sha256 VARCHAR NOT NULL,
            byte_count BIGINT NOT NULL,
            file_count BIGINT NOT NULL,
            linkbase_file_count BIGINT NOT NULL,
            relationship_count BIGINT NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_taxonomy_roles (
            role_id VARCHAR PRIMARY KEY,
            taxonomy_package_id VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            release_year INTEGER NOT NULL,
            role_uri VARCHAR,
            role_name VARCHAR,
            role_href VARCHAR,
            linkbase_type VARCHAR NOT NULL,
            source_file VARCHAR NOT NULL,
            relationship_count BIGINT NOT NULL,
            source_url VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_taxonomy_relationships (
            relationship_id VARCHAR PRIMARY KEY,
            taxonomy_package_id VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            release_year INTEGER NOT NULL,
            linkbase_type VARCHAR NOT NULL,
            source_file VARCHAR NOT NULL,
            role_uri VARCHAR,
            role_name VARCHAR,
            role_href VARCHAR,
            arcrole VARCHAR,
            from_label VARCHAR,
            to_label VARCHAR,
            parent_href VARCHAR,
            parent_taxonomy VARCHAR,
            parent_concept VARCHAR NOT NULL,
            parent_concept_kind VARCHAR,
            child_href VARCHAR,
            child_taxonomy VARCHAR,
            child_concept VARCHAR NOT NULL,
            child_concept_kind VARCHAR,
            order_value DOUBLE,
            weight DOUBLE,
            priority INTEGER,
            preferred_label VARCHAR,
            use VARCHAR,
            closed BOOLEAN,
            context_element VARCHAR,
            usable BOOLEAN,
            target_role VARCHAR,
            touches_observed_concept BOOLEAN NOT NULL DEFAULT false,
            source_url VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_dimension_edges (
            dimension_edge_id VARCHAR PRIMARY KEY,
            relationship_id VARCHAR NOT NULL,
            taxonomy_package_id VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            release_year INTEGER NOT NULL,
            role_uri VARCHAR,
            role_name VARCHAR,
            source_file VARCHAR NOT NULL,
            relationship_kind VARCHAR NOT NULL,
            arcrole VARCHAR,
            source_taxonomy VARCHAR,
            source_concept VARCHAR NOT NULL,
            source_concept_kind VARCHAR,
            target_taxonomy VARCHAR,
            target_concept VARCHAR NOT NULL,
            target_concept_kind VARCHAR,
            order_value DOUBLE,
            context_element VARCHAR,
            closed BOOLEAN,
            usable BOOLEAN,
            target_role VARCHAR,
            touches_observed_concept BOOLEAN NOT NULL DEFAULT false,
            source_url VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_fact_frames (
            fact_frame_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            frame VARCHAR NOT NULL,
            frame_year INTEGER,
            frame_quarter INTEGER,
            frame_period VARCHAR NOT NULL,
            is_instant BOOLEAN,
            fact_count BIGINT NOT NULL,
            security_count BIGINT NOT NULL,
            accession_count BIGINT NOT NULL,
            first_period_start DATE,
            last_period_end DATE,
            first_filed_date DATE,
            last_filed_date DATE,
            first_available_at TIMESTAMP,
            last_available_at TIMESTAMP,
            latest_source_loaded_at TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_filing_contexts (
            filing_context_id VARCHAR PRIMARY KEY,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            form VARCHAR,
            filing_date DATE,
            report_date DATE,
            acceptance_datetime TIMESTAMP,
            primary_document VARCHAR NOT NULL,
            context_id VARCHAR NOT NULL,
            entity_identifier_scheme VARCHAR,
            entity_identifier VARCHAR,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE,
            instant_date DATE,
            has_segment BOOLEAN NOT NULL,
            has_scenario BOOLEAN NOT NULL,
            explicit_member_count BIGINT NOT NULL,
            typed_member_count BIGINT NOT NULL,
            dimension_count BIGINT NOT NULL,
            context_hash VARCHAR NOT NULL,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_filing_dimensions (
            filing_dimension_id VARCHAR PRIMARY KEY,
            filing_context_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            form VARCHAR,
            filing_date DATE,
            acceptance_datetime TIMESTAMP,
            primary_document VARCHAR NOT NULL,
            context_id VARCHAR NOT NULL,
            context_element VARCHAR NOT NULL,
            member_kind VARCHAR NOT NULL,
            dimension_qname VARCHAR,
            dimension_taxonomy VARCHAR,
            dimension_concept VARCHAR,
            member_qname VARCHAR,
            member_taxonomy VARCHAR,
            member_concept VARCHAR,
            typed_member_value VARCHAR,
            member_text VARCHAR,
            ordinal INTEGER NOT NULL,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_filing_facts (
            filing_fact_id VARCHAR PRIMARY KEY,
            filing_context_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            form VARCHAR,
            filing_date DATE,
            acceptance_datetime TIMESTAMP,
            primary_document VARCHAR NOT NULL,
            fact_ordinal INTEGER NOT NULL,
            fact_kind VARCHAR NOT NULL,
            ix_id VARCHAR,
            qname VARCHAR NOT NULL,
            taxonomy VARCHAR,
            concept VARCHAR NOT NULL,
            context_ref VARCHAR NOT NULL,
            unit_ref VARCHAR,
            unit_measures_json VARCHAR NOT NULL,
            unit_numerator_measures_json VARCHAR NOT NULL,
            unit_denominator_measures_json VARCHAR NOT NULL,
            decimals VARCHAR,
            precision VARCHAR,
            scale INTEGER,
            sign VARCHAR,
            format VARCHAR,
            continued_at VARCHAR,
            is_hidden BOOLEAN NOT NULL,
            raw_value VARCHAR,
            normalized_value VARCHAR,
            numeric_value DOUBLE,
            is_numeric BOOLEAN NOT NULL,
            source_line INTEGER,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_validation_results (
            validation_id VARCHAR PRIMARY KEY,
            validation_run_id VARCHAR NOT NULL,
            rule_family VARCHAR NOT NULL,
            rule_code VARCHAR NOT NULL,
            severity VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            form VARCHAR,
            filing_date DATE,
            acceptance_datetime TIMESTAMP,
            primary_document VARCHAR NOT NULL,
            role_uri VARCHAR,
            parent_taxonomy VARCHAR,
            parent_concept VARCHAR NOT NULL,
            context_ref VARCHAR NOT NULL,
            unit_ref VARCHAR,
            parent_fact_id VARCHAR,
            parent_value DOUBLE,
            child_weighted_sum DOUBLE,
            absolute_difference DOUBLE,
            tolerance DOUBLE NOT NULL,
            child_count INTEGER NOT NULL,
            child_facts_json VARCHAR NOT NULL,
            message VARCHAR,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_fact_revisions (
            fact_revision_id VARCHAR NOT NULL,
            revision_group_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            accession_number VARCHAR NOT NULL,
            filed_date DATE NOT NULL,
            available_at TIMESTAMP,
            form VARCHAR,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            frame VARCHAR,
            value DOUBLE,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            is_value_changed BOOLEAN NOT NULL,
            previous_accession_number VARCHAR,
            previous_filed_date DATE,
            previous_available_at TIMESTAMP,
            previous_value DOUBLE,
            value_delta DOUBLE,
            value_delta_percent DOUBLE,
            first_filed_date DATE,
            latest_filed_date DATE,
            first_available_at TIMESTAMP,
            latest_available_at TIMESTAMP,
            run_id VARCHAR,
            source_url VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (fact_revision_id)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_statement_map (
            source VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            statement_type VARCHAR NOT NULL,
            statement_section VARCHAR NOT NULL,
            canonical_metric VARCHAR NOT NULL,
            canonical_label VARCHAR NOT NULL,
            period_type VARCHAR NOT NULL,
            normal_balance VARCHAR NOT NULL,
            unit_type VARCHAR NOT NULL,
            value_multiplier DOUBLE NOT NULL DEFAULT 1.0,
            concept_priority INTEGER NOT NULL DEFAULT 100,
            is_core_metric BOOLEAN NOT NULL DEFAULT true,
            is_active BOOLEAN NOT NULL DEFAULT true,
            notes VARCHAR,
            item_id INTEGER,
            industry_template VARCHAR DEFAULT 'ALL',
            is_derived BOOLEAN DEFAULT FALSE,
            derivation_expr VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (source, taxonomy, concept, industry_template)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_statement_points (
            statement_point_id VARCHAR NOT NULL,
            fact_revision_id VARCHAR NOT NULL,
            revision_group_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            statement_type VARCHAR NOT NULL,
            statement_section VARCHAR NOT NULL,
            canonical_metric VARCHAR NOT NULL,
            canonical_label VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            unit_type VARCHAR NOT NULL,
            period_type VARCHAR NOT NULL,
            normal_balance VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            form VARCHAR,
            accession_number VARCHAR NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            is_value_changed BOOLEAN NOT NULL,
            raw_value DOUBLE,
            value DOUBLE,
            previous_raw_value DOUBLE,
            previous_value DOUBLE,
            value_delta DOUBLE,
            value_delta_percent DOUBLE,
            run_id VARCHAR,
            source_url VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (statement_point_id)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_ttm_points (
            ttm_point_id VARCHAR NOT NULL,
            ttm_revision_group_id VARCHAR NOT NULL,
            anchor_statement_point_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            statement_type VARCHAR NOT NULL,
            statement_section VARCHAR NOT NULL,
            canonical_metric VARCHAR NOT NULL,
            canonical_label VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            unit_type VARCHAR NOT NULL,
            ttm_start_date DATE NOT NULL,
            ttm_end_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            form VARCHAR,
            accession_number VARCHAR NOT NULL,
            quarter_count INTEGER NOT NULL,
            coverage_days INTEGER NOT NULL,
            min_input_available_at TIMESTAMP,
            max_input_available_at TIMESTAMP,
            input_statement_point_ids_json VARCHAR NOT NULL,
            input_accessions_json VARCHAR NOT NULL,
            input_period_ends_json VARCHAR NOT NULL,
            ttm_value DOUBLE,
            previous_ttm_value DOUBLE,
            ttm_value_delta DOUBLE,
            ttm_value_delta_percent DOUBLE,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            is_value_changed BOOLEAN NOT NULL,
            calculation_method VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (ttm_point_id)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_periods (
            fundamental_period_id VARCHAR NOT NULL,
            period_group_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            datadate DATE,
            period_days INTEGER,
            normalized_period_type VARCHAR NOT NULL,
            calendar_year INTEGER,
            calendar_quarter INTEGER,
            calendar_period VARCHAR,
            rdq DATE,
            pdate DATE,
            fdate DATE,
            ldate DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            form VARCHAR,
            accession_number VARCHAR NOT NULL,
            reported_fiscal_years_json VARCHAR NOT NULL,
            reported_fiscal_periods_json VARCHAR NOT NULL,
            statement_types_json VARCHAR NOT NULL,
            canonical_metrics_json VARCHAR NOT NULL,
            input_statement_point_ids_json VARCHAR NOT NULL,
            statement_point_count INTEGER NOT NULL,
            canonical_metric_count INTEGER NOT NULL,
            concept_count INTEGER NOT NULL,
            value_changed_statement_count INTEGER NOT NULL,
            has_balance_sheet BOOLEAN NOT NULL,
            has_income_statement BOOLEAN NOT NULL,
            has_cash_flow BOOLEAN NOT NULL,
            has_per_share BOOLEAN NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            first_available_at TIMESTAMP,
            latest_available_at TIMESTAMP,
            source_loaded_at TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (fundamental_period_id)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_points (
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            metric VARCHAR NOT NULL,
            taxonomy VARCHAR,
            unit VARCHAR,
            period_start DATE,
            period_end DATE,
            as_of_date DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            form VARCHAR,
                accession_number VARCHAR,
                value DOUBLE,
                available_at TIMESTAMP,
                run_id VARCHAR,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS macro_observations (
            source VARCHAR NOT NULL,
                series_id VARCHAR NOT NULL,
                observation_date DATE NOT NULL,
                as_of_date DATE NOT NULL,
                available_at TIMESTAMP,
                value DOUBLE,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS trading_calendar (
            calendar_id VARCHAR NOT NULL,
            trade_date DATE NOT NULL,
            is_open BOOLEAN NOT NULL,
            open_time VARCHAR,
            close_time VARCHAR,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS universes (
            universe_id VARCHAR PRIMARY KEY,
            name VARCHAR NOT NULL,
            description VARCHAR,
            rules_json VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS universe_memberships (
            universe_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            effective_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
                weight DOUBLE,
                reason VARCHAR,
                available_at TIMESTAMP,
                source VARCHAR NOT NULL,
                run_id VARCHAR,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
            )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS feature_definitions (
            feature_set VARCHAR NOT NULL,
            feature_name VARCHAR NOT NULL,
            description VARCHAR,
            expression_sql VARCHAR,
            input_tables_json VARCHAR,
            lookback_days INTEGER,
            is_point_in_time_safe BOOLEAN NOT NULL DEFAULT true,
            available_at_policy VARCHAR,
            owner VARCHAR,
            source VARCHAR NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS feature_values (
            feature_set VARCHAR NOT NULL,
            feature_name VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            as_of_date DATE NOT NULL,
                value DOUBLE,
                input_hash VARCHAR,
                available_at TIMESTAMP,
                source VARCHAR NOT NULL,
                run_id VARCHAR,
                computed_at TIMESTAMP NOT NULL DEFAULT now()
            )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS feature_build_manifests (
            manifest_id VARCHAR PRIMARY KEY,
            feature_set VARCHAR NOT NULL,
            run_id VARCHAR,
            symbols_json VARCHAR NOT NULL,
            feature_names_json VARCHAR NOT NULL,
            input_tables_json VARCHAR NOT NULL,
            input_min_as_of_date DATE,
            input_max_as_of_date DATE,
            input_row_count BIGINT NOT NULL,
            output_min_as_of_date DATE,
            output_max_as_of_date DATE,
            output_row_count BIGINT NOT NULL,
            feature_count BIGINT NOT NULL,
            min_available_at TIMESTAMP,
            max_available_at TIMESTAMP,
            params_json VARCHAR,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS feature_set_catalog (
            feature_set VARCHAR PRIMARY KEY,
            version_label VARCHAR,
            feature_family VARCHAR,
            description VARCHAR,
            feature_count BIGINT NOT NULL,
            dependency_count BIGINT NOT NULL,
            source_table_count BIGINT NOT NULL,
            derived_feature_dependency_count BIGINT NOT NULL,
            max_lookback_days INTEGER,
            input_tables_json VARCHAR NOT NULL,
            feature_names_json VARCHAR NOT NULL,
            point_in_time_safe BOOLEAN NOT NULL,
            owner VARCHAR,
            source VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS feature_dependency_edges (
            dependency_id VARCHAR PRIMARY KEY,
            feature_set VARCHAR NOT NULL,
            feature_name VARCHAR NOT NULL,
            dependency_type VARCHAR NOT NULL,
            dependency_name VARCHAR NOT NULL,
            dependency_feature_set VARCHAR,
            dependency_feature_name VARCHAR,
            dependency_depth INTEGER NOT NULL,
            expression_sql VARCHAR,
            lookback_days INTEGER,
            is_direct BOOLEAN NOT NULL DEFAULT true,
            source VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_managers (
            manager_id VARCHAR PRIMARY KEY,
            cik VARCHAR NOT NULL,
            manager_name VARCHAR,
            city VARCHAR,
            state_or_country VARCHAR,
            crd_number VARCHAR,
            sec_file_number VARCHAR,
            first_report_period DATE,
            last_report_period DATE,
            first_filing_date DATE,
            last_filing_date DATE,
            filing_count BIGINT NOT NULL,
            amendment_count BIGINT NOT NULL,
            source_period_count BIGINT NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_manager_reports (
            manager_report_id VARCHAR PRIMARY KEY,
            manager_id VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            report_period DATE NOT NULL,
            filing_date DATE NOT NULL,
            source_period VARCHAR NOT NULL,
            submission_type VARCHAR,
            report_calendar_or_quarter DATE,
            is_amendment BOOLEAN NOT NULL DEFAULT false,
            amendment_no VARCHAR,
            amendment_type VARCHAR,
            filing_manager_name VARCHAR,
            filing_manager_city VARCHAR,
            filing_manager_state_or_country VARCHAR,
            report_type VARCHAR,
            form_13f_file_number VARCHAR,
            crd_number VARCHAR,
            sec_file_number VARCHAR,
            other_included_managers_count BIGINT,
            table_entry_total BIGINT,
            table_value_total DOUBLE,
            is_confidential_omitted BOOLEAN NOT NULL DEFAULT false,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_security_positions (
            position_id VARCHAR PRIMARY KEY,
            manager_report_id VARCHAR NOT NULL,
            manager_id VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            infotable_sk BIGINT,
            security_id VARCHAR,
            symbol VARCHAR,
            cusip VARCHAR NOT NULL,
            figi VARCHAR,
            name_of_issuer VARCHAR,
            title_of_class VARCHAR,
            report_period DATE NOT NULL,
            filing_date DATE NOT NULL,
            source_period VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            value_usd DOUBLE,
            share_quantity DOUBLE,
            share_quantity_type VARCHAR,
            put_call VARCHAR,
            investment_discretion VARCHAR,
            other_manager VARCHAR,
            voting_auth_sole DOUBLE,
            voting_auth_shared DOUBLE,
            voting_auth_none DOUBLE,
            voting_auth_total DOUBLE,
            portfolio_value_usd DOUBLE,
            portfolio_weight DOUBLE,
            is_common_share BOOLEAN NOT NULL DEFAULT false,
            is_option BOOLEAN NOT NULL DEFAULT false,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_security_ownership (
            ownership_id VARCHAR PRIMARY KEY,
            security_id VARCHAR,
            symbol VARCHAR,
            cusip VARCHAR NOT NULL,
            report_period DATE NOT NULL,
            source_period VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            holding_row_count BIGINT NOT NULL,
            filing_count BIGINT NOT NULL,
            holder_count BIGINT NOT NULL,
            common_holder_count BIGINT NOT NULL,
            common_value_usd DOUBLE,
            common_share_quantity DOUBLE,
            call_share_quantity DOUBLE,
            put_share_quantity DOUBLE,
            manager_portfolio_value_usd DOUBLE,
            avg_portfolio_weight DOUBLE,
            max_portfolio_weight DOUBLE,
            top_manager_id VARCHAR,
            top_manager_name VARCHAR,
            prior_report_period DATE,
            prior_common_value_usd DOUBLE,
            prior_common_share_quantity DOUBLE,
            prior_holder_count BIGINT,
            common_value_usd_qoq_change DOUBLE,
            common_share_quantity_qoq_change DOUBLE,
            holder_count_qoq_change BIGINT,
            common_value_usd_qoq_pct_change DOUBLE,
            common_share_quantity_qoq_pct_change DOUBLE,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS alpha_expression_catalog (
            alpha_id VARCHAR PRIMARY KEY,
            alpha_name VARCHAR NOT NULL,
            description VARCHAR,
            expression_sql VARCHAR NOT NULL,
            feature_set VARCHAR NOT NULL,
            input_features_json VARCHAR NOT NULL,
            universe_id VARCHAR,
            rebalance_frequency VARCHAR NOT NULL,
            horizon_days INTEGER NOT NULL,
            direction INTEGER NOT NULL,
            neutralization VARCHAR,
            rank_method VARCHAR,
            weighting_method VARCHAR,
            is_point_in_time_safe BOOLEAN NOT NULL DEFAULT true,
            available_at_policy VARCHAR,
            params_json VARCHAR,
            owner VARCHAR,
            source VARCHAR NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS alpha_signal_values (
            alpha_signal_id VARCHAR PRIMARY KEY,
            alpha_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            as_of_date DATE NOT NULL,
            signal_value DOUBLE NOT NULL,
            rank_value DOUBLE,
            weight DOUBLE,
            cross_section_count BIGINT NOT NULL,
            available_at TIMESTAMP,
            input_hash VARCHAR,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            computed_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS alpha_backtest_manifests (
            backtest_id VARCHAR PRIMARY KEY,
            alpha_id VARCHAR NOT NULL,
            feature_set VARCHAR NOT NULL,
            universe_id VARCHAR,
            start_date DATE,
            end_date DATE,
            horizon_days INTEGER NOT NULL,
            rebalance_frequency VARCHAR NOT NULL,
            signal_count BIGINT NOT NULL,
            security_count BIGINT NOT NULL,
            evaluation_days BIGINT NOT NULL,
            evaluated_signal_count BIGINT NOT NULL,
            average_cross_section DOUBLE,
            mean_daily_long_short_return DOUBLE,
            volatility_daily_long_short_return DOUBLE,
            mean_rank_ic DOUBLE,
            hit_rate DOUBLE,
            cumulative_long_short_return DOUBLE,
            min_available_at TIMESTAMP,
            max_available_at TIMESTAMP,
            params_json VARCHAR,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS etl_job_definitions (
            job_name VARCHAR PRIMARY KEY,
            dataset_id VARCHAR NOT NULL,
            params_json VARCHAR NOT NULL,
            enabled BOOLEAN NOT NULL DEFAULT true,
            schedule VARCHAR,
            max_retries INTEGER NOT NULL DEFAULT 0,
            retry_delay_seconds DOUBLE NOT NULL DEFAULT 0,
            dependencies_json VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS etl_job_runs (
            job_run_id VARCHAR PRIMARY KEY,
            job_name VARCHAR NOT NULL,
            dataset_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            started_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP,
            dataset_run_id VARCHAR,
            rows_loaded BIGINT,
            attempt_count INTEGER NOT NULL DEFAULT 0,
            max_retries INTEGER NOT NULL DEFAULT 0,
            retry_delay_seconds DOUBLE NOT NULL DEFAULT 0,
            params_json VARCHAR,
            error_message VARCHAR
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS etl_job_events (
            job_run_id VARCHAR NOT NULL,
            event_time TIMESTAMP NOT NULL DEFAULT now(),
            level VARCHAR NOT NULL,
            message VARCHAR NOT NULL,
            details_json VARCHAR
        )
        """
    )

    # S1: Reference classification tables (taxonomy, taxonomy_node,
    # entity_classification, taxonomy_mapping) are created exclusively by
    # migration 0003 (reference_classifications) in db/migrations.py.
    # Do NOT add CREATE TABLE statements here — the initialize() short-circuit
    # (_schema_is_current) means ensure_quant_schema is skipped on an already-
    # bootstrapped warehouse, so any table added here without a migration would
    # be silently missed. New tables must always go through a migration bump.

    _ensure_schema_evolution(store)
    _ensure_indexes_and_views(store)
    _seed_catalog(store)


def _ensure_schema_evolution(store: DuckDBStore) -> None:
    # The ALTER TABLE evolution statements have been moved into the versioned migration
    # framework (db/migrations.py, migration 0002 schema_evolution_alters).
    # apply_pending_migrations() is called from DuckDBStore.initialize() after
    # ensure_quant_schema() so the alters will still run on every fresh database.
    # This function is kept as a no-op stub so that call sites continue to compile.
    pass

def _ensure_indexes_and_views(store: DuckDBStore) -> None:
    con = store.con
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_security_identifier_history_lookup ON security_identifier_history(id_type, id_value, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_security_identifier_history_security ON security_identifier_history(security_id, id_type, valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_exchange_listings_ticker ON exchange_listings(ticker, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_identifier_resolution_candidates_key ON identifier_resolution_candidates(source_dataset_id, source_key_type, source_key_value)",
        "CREATE INDEX IF NOT EXISTS idx_identifier_resolution_candidates_target ON identifier_resolution_candidates(target_security_id)",
        "CREATE INDEX IF NOT EXISTS idx_identifier_resolution_decisions_candidate ON identifier_resolution_decisions(candidate_id, decision_method)",
        "CREATE INDEX IF NOT EXISTS idx_identifier_resolution_decisions_key ON identifier_resolution_decisions(source_dataset_id, source_key_type, source_key_value)",
        "CREATE INDEX IF NOT EXISTS idx_identifier_resolution_decisions_target ON identifier_resolution_decisions(target_security_id)",
        "CREATE INDEX IF NOT EXISTS idx_equity_daily_bars_security_date ON equity_daily_bars(security_id, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_equity_daily_bars_symbol_date ON equity_daily_bars(symbol, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_corp_action_type_dim_event ON corp_action_type_dim(event_type, type_code)",
        "CREATE INDEX IF NOT EXISTS idx_adjustment_factor_history_security ON adjustment_factor_history(security_id, ex_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_adjustment_factor_history_event ON adjustment_factor_history(event_type, type_code, ex_date)",
        "CREATE INDEX IF NOT EXISTS idx_daily_adjustment_factors_security ON daily_adjustment_factors(security_id, trade_date, as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_daily_adjustment_factors_symbol ON daily_adjustment_factors(symbol, trade_date, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_shares_outstanding_history_security ON shares_outstanding_history(security_id, share_count_type, effective_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_shares_outstanding_history_latest ON shares_outstanding_history(is_latest_revision, security_id, share_count_type)",
        "CREATE INDEX IF NOT EXISTS idx_sec_company_facts_security_asof ON sec_company_facts(security_id, filed_date)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_concept_catalog_lookup ON xbrl_concept_catalog(taxonomy, concept)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_concept_catalog_category ON xbrl_concept_catalog(statement_category, taxonomy)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_taxonomy_relationships_parent ON xbrl_taxonomy_relationships(parent_taxonomy, parent_concept, linkbase_type)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_taxonomy_relationships_child ON xbrl_taxonomy_relationships(child_taxonomy, child_concept, linkbase_type)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_taxonomy_relationships_role ON xbrl_taxonomy_relationships(taxonomy_package_id, linkbase_type, role_uri)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_dimension_edges_source ON xbrl_dimension_edges(source_taxonomy, source_concept, relationship_kind)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_dimension_edges_target ON xbrl_dimension_edges(target_taxonomy, target_concept, relationship_kind)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_fact_frames_lookup ON xbrl_fact_frames(taxonomy, concept, frame)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_filing_contexts_filing ON xbrl_filing_contexts(security_id, accession_number, primary_document)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_filing_contexts_period ON xbrl_filing_contexts(security_id, period_type, period_end, instant_date)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_filing_dimensions_context ON xbrl_filing_dimensions(filing_context_id)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_filing_dimensions_dimension ON xbrl_filing_dimensions(dimension_taxonomy, dimension_concept, member_taxonomy, member_concept)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_filing_facts_context ON xbrl_filing_facts(filing_context_id)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_filing_facts_concept ON xbrl_filing_facts(taxonomy, concept, context_ref)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_filing_facts_filing ON xbrl_filing_facts(security_id, accession_number, primary_document)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_validation_results_filing ON xbrl_validation_results(security_id, accession_number, primary_document)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_validation_results_status ON xbrl_validation_results(rule_family, status, severity)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_fact_revisions_group ON fundamental_fact_revisions(revision_group_id, revision_sequence)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_fact_revisions_security ON fundamental_fact_revisions(security_id, taxonomy, concept, period_end, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_fact_revisions_latest ON fundamental_fact_revisions(is_latest_revision, taxonomy, concept)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_statement_map_lookup ON fundamental_statement_map(taxonomy, concept, is_active)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_statement_points_metric_asof ON fundamental_statement_points(canonical_metric, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_statement_points_security ON fundamental_statement_points(security_id, statement_type, period_end, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_statement_points_revision ON fundamental_statement_points(revision_group_id, revision_sequence)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ttm_points_metric_asof ON fundamental_ttm_points(canonical_metric, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ttm_points_security ON fundamental_ttm_points(security_id, statement_type, ttm_end_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ttm_points_revision ON fundamental_ttm_points(ttm_revision_group_id, revision_sequence)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_periods_security ON fundamental_periods(security_id, period_end, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_periods_group ON fundamental_periods(period_group_id, revision_sequence)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_periods_type ON fundamental_periods(normalized_period_type, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_points_metric_asof ON fundamental_points(metric, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_feature_definitions_lookup ON feature_definitions(feature_set, feature_name)",
        "CREATE INDEX IF NOT EXISTS idx_feature_values_lookup ON feature_values(feature_set, feature_name, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_feature_build_manifests_run ON feature_build_manifests(feature_set, run_id)",
        "CREATE INDEX IF NOT EXISTS idx_feature_dependency_edges_feature ON feature_dependency_edges(feature_set, feature_name)",
        "CREATE INDEX IF NOT EXISTS idx_feature_dependency_edges_dependency ON feature_dependency_edges(dependency_type, dependency_name)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_managers_cik ON thirteenf_managers(cik)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_manager_reports_manager_period ON thirteenf_manager_reports(manager_id, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_manager_reports_accession ON thirteenf_manager_reports(accession_number, source_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_security_positions_security_period ON thirteenf_security_positions(security_id, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_security_positions_cusip_period ON thirteenf_security_positions(cusip, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_security_ownership_security_period ON thirteenf_security_ownership(security_id, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_security_ownership_cusip_period ON thirteenf_security_ownership(cusip, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_security_ownership_asof ON thirteenf_security_ownership(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_alpha_expression_catalog_feature_set ON alpha_expression_catalog(feature_set, universe_id)",
        "CREATE INDEX IF NOT EXISTS idx_alpha_signal_values_lookup ON alpha_signal_values(alpha_id, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_alpha_signal_values_security ON alpha_signal_values(security_id, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_alpha_backtest_manifests_alpha ON alpha_backtest_manifests(alpha_id, feature_set, universe_id)",
        # S1 reference-classification indexes are created by migration 0003
        # (db/migrations.py _reference_classifications) — do NOT add them here.
        "CREATE INDEX IF NOT EXISTS idx_listing_status_intervals_symbol ON listing_status_intervals(symbol, listing_venue_code, valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_listing_status_intervals_security ON listing_status_intervals(security_id, valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_listing_status_intervals_asof ON listing_status_intervals(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_events_security ON delisting_events(security_id, delist_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_events_symbol ON delisting_events(symbol, delist_date, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_events_asof ON delisting_events(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_events_code ON delisting_events(delist_code, delist_date)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_return_observations_security ON delisting_return_observations(security_id, delist_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_return_observations_symbol ON delisting_return_observations(symbol, delist_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_return_observations_vendor ON delisting_return_observations(provider, vendor_security_id_type, vendor_security_id, delist_date)",
        "CREATE INDEX IF NOT EXISTS idx_lake_export_files_run ON lake_export_files(export_run_id, object_name)",
        "CREATE INDEX IF NOT EXISTS idx_lake_export_files_object ON lake_export_files(object_name, exported_at)",
        "CREATE INDEX IF NOT EXISTS idx_sec_submissions_security_date ON sec_submissions(security_id, filing_date)",
        "CREATE INDEX IF NOT EXISTS idx_sec_submissions_accession ON sec_submissions(accession_number)",
        "CREATE INDEX IF NOT EXISTS idx_nasdaq_symbol_directory_symbol ON nasdaq_symbol_directory(symbol, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_nasdaq_listing_events_symbol_date ON nasdaq_listing_events(symbol, effective_date)",
        "CREATE INDEX IF NOT EXISTS idx_nasdaq_listing_events_security_date ON nasdaq_listing_events(security_id, effective_date)",
        "CREATE INDEX IF NOT EXISTS idx_macro_observations_lookup ON macro_observations(series_id, observation_date, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_provider_parity_matrix_domain ON provider_parity_matrix(warehouse_domain, parity_status)",
    ):
        con.execute(statement)

    con.execute(
        """
        CREATE OR REPLACE VIEW v_security_master_current AS
        SELECT
            s.security_id,
            s.entity_id,
            s.issuer_id,
            s.primary_symbol,
            s.name,
            s.asset_class,
            s.country,
            s.currency,
            s.active,
            any_value(cik.id_value) AS cik,
            any_value(cusip.id_value) AS cusip
        FROM securities s
        LEFT JOIN security_identifier_history cik
          ON cik.security_id = s.security_id
         AND cik.id_type = 'CIK'
         AND cik.valid_to IS NULL
        LEFT JOIN security_identifier_history cusip
          ON cusip.security_id = s.security_id
         AND cusip.id_type = 'CUSIP'
         AND cusip.valid_to IS NULL
        GROUP BY
            s.security_id,
            s.entity_id,
            s.issuer_id,
            s.primary_symbol,
            s.name,
            s.asset_class,
            s.country,
            s.currency,
            s.active
        """
    )
    con.execute(
        """
        CREATE OR REPLACE VIEW v_equity_daily_returns AS
        SELECT
            source,
            security_id,
            symbol,
            trade_date,
            close,
            close / lag(close) OVER (
                PARTITION BY source, security_id
                ORDER BY trade_date
            ) - 1.0 AS simple_return,
            ln(close / lag(close) OVER (
                PARTITION BY source, security_id
                ORDER BY trade_date
            )) AS log_return,
            volume,
            source_loaded_at
        FROM equity_daily_bars
        WHERE close IS NOT NULL AND close > 0
        """
    )
    con.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_points_latest AS
        SELECT *
        FROM (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY security_id, metric, period_end, unit
                    ORDER BY as_of_date DESC, source_loaded_at DESC
                ) AS recency_rank
            FROM fundamental_points
        )
        WHERE recency_rank = 1
        """
    )
    con.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_statement_latest AS
        SELECT *
        FROM (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY security_id, canonical_metric, period_end, unit
                    ORDER BY as_of_date DESC, available_at DESC NULLS LAST, source_loaded_at DESC
                ) AS recency_rank
            FROM fundamental_statement_points
        )
        WHERE recency_rank = 1
        """
    )
    con.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_ttm_latest AS
        SELECT *
        FROM (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY security_id, canonical_metric, ttm_end_date, unit
                    ORDER BY as_of_date DESC, available_at DESC NULLS LAST, source_loaded_at DESC
                ) AS recency_rank
            FROM fundamental_ttm_points
        )
        WHERE recency_rank = 1
        """
    )
    con.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_periods_latest AS
        SELECT *
        FROM (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY period_group_id
                    ORDER BY as_of_date DESC, available_at DESC NULLS LAST, source_loaded_at DESC
                ) AS recency_rank
            FROM fundamental_periods
        )
        WHERE recency_rank = 1
        """
    )
    con.execute(
        """
        CREATE OR REPLACE VIEW v_alpha_daily_panel AS
        SELECT
            b.security_id,
            b.symbol,
            b.trade_date AS as_of_date,
            b.close,
            r.simple_return,
            r.log_return,
            b.volume,
            b.close * b.volume AS dollar_volume
        FROM equity_daily_bars b
        LEFT JOIN v_equity_daily_returns r
          ON r.source = b.source
         AND r.security_id = b.security_id
         AND r.trade_date = b.trade_date
        """
    )
    con.execute(
        """
        CREATE OR REPLACE VIEW v_sec_latest_filings AS
        SELECT *
        FROM (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY security_id, form
                    ORDER BY filing_date DESC NULLS LAST,
                             acceptance_datetime DESC NULLS LAST,
                             source_loaded_at DESC
                ) AS recency_rank
            FROM sec_submissions
        )
        WHERE recency_rank = 1
        """
    )
    con.execute(
        """
        CREATE OR REPLACE VIEW v_macro_latest AS
        SELECT *
        FROM (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY series_id, observation_date
                    ORDER BY as_of_date DESC, source_loaded_at DESC
                ) AS recency_rank
            FROM macro_observations
        )
        WHERE recency_rank = 1
        """
    )


def _seed_catalog(store: DuckDBStore) -> None:
    con = store.con
    source_docs = {
        "sec_edgar": [
            "https://www.sec.gov/search-filings/edgar-application-programming-interfaces",
            "https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets",
        ],
        "finra": [
            "https://developer.finra.org/docs",
            "https://developer.finra.org/catalog",
        ],
        "nasdaq_trader": [
            "https://www.nasdaqtrader.com/trader.aspx?id=symboldirdefs",
            "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt",
            "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt",
            "https://www.nasdaqtrader.com/dynamic/SymDir/TradingSystemAddsDeletes.txt",
        ],
        "tbltickerhistory": [],
        "fred": [
            "https://fred.stlouisfed.org/docs/api/fred/",
            "https://fredhelp.stlouisfed.org/fred/data/downloading/using-the-download-data-link/",
        ],
        "atx_warehouse": [],
    }
    source_rows = [
        ("sec_edgar", "SEC EDGAR APIs", "https://www.sec.gov/", "Public SEC EDGAR data; follow SEC fair access policy.", "daily", False),
        ("finra", "FINRA API", "https://api.finra.org/", "Public FINRA API data.", "semi-monthly", False),
        ("nasdaq_trader", "Nasdaq Trader Symbol Directory", "https://www.nasdaqtrader.com/", "Public symbol directory files.", "intraday", False),
        ("tbltickerhistory", "tbltickerhistory local archive", None, "Local vendor/source archive in Downloads.", "daily", False),
        ("fred", "FRED graph CSV/API", "https://fred.stlouisfed.org/", "Public FRED graph CSV downloads; official API may require a key.", "varies", False),
        ("atx_warehouse", "atx-impl warehouse", None, "Derived internal tables, feature store, catalog, and QA metadata.", "on-demand", False),
    ]
    for row in source_rows:
        con.execute(
            """
            INSERT OR REPLACE INTO source_systems (
                source_system_id,
                name,
                base_url,
                license_note,
                cadence,
                requires_key,
                metadata_json,
                updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, now())
            """,
            list(row) + [json.dumps({"documentation_urls": source_docs.get(row[0], [])}, sort_keys=True)],
        )

    dataset_rows = [
        ("sec_security_master", "sec_edgar", "SEC security master", "Ticker, CIK, and issuer reference data.", "security", "securities", "as_of_date", "available_at"),
        ("tbltickerhistory_daily", "tbltickerhistory", "Daily equity bars", "Local daily OHLCV and vendor-derived equity fields.", "security_id,trade_date", "equity_daily_bars", "trade_date", "available_at"),
        ("corporate_actions", "tbltickerhistory", "Corporate actions", "Dividend and split adjustment fields where public/local source data supports them.", "security_id,ex_date,action_type", "corporate_actions", "ex_date", "available_at"),
        ("corp_action_type_dim", "atx_warehouse", "Corporate-action type dimension", "CRSP/DTCC-style corporate-action type codes used to normalize public action evidence.", "type_code", "corp_action_type_dim", "updated_at", "updated_at"),
        ("adjustment_factor_history", "tbltickerhistory", "Adjustment factor history", "Event-level price/share/volume adjustment factors derived from normalized corporate actions.", "security_id,ex_date,event_type,source", "adjustment_factor_history", "ex_date", "available_at"),
        ("finra_short_interest", "finra", "FINRA short interest", "Consolidated short interest by settlement date, supporting symbol-scoped loads, all-symbol date refreshes, and compact historical backfill controls.", "symbol,settlement_date", "finra_short_interest", "settlement_date", "available_at"),
        ("finra_short_interest_backfills", "finra", "FINRA short-interest backfill manifests", "Audit manifests for compact all-symbol FINRA settlement-date backfill runs.", "manifest,settlement_date_window", "finra_short_interest_backfill_manifests", "finished_at", "finished_at"),
        ("finra_short_interest_features", "finra", "FINRA short-interest features", "PIT feature-store rows for short pressure, days-to-cover, short-interest changes, and cross-sectional ranks derived from FINRA consolidated short interest.", "security_id,feature,settlement_date", "feature_values", "as_of_date", "available_at"),
        ("sec_13f", "sec_edgar", "SEC Form 13F", "Institutional holdings from SEC 13F bulk data.", "accession_number,holding", "thirteenf_holdings", "report_period", "filing_date"),
        ("sec_13f_ownership_features", "sec_edgar", "SEC 13F ownership features", "PIT manager master, filing report, security position, ownership aggregate, and feature-store rows derived from loaded SEC 13F data.", "manager,security,report_period", "thirteenf_security_ownership", "as_of_date", "available_at"),
        ("sec_insider_ownership", "sec_edgar", "SEC Section 16 insider ownership", "Form 3/4/5 ownership XML filings normalized into insider, issuer-role, transaction, holding, and 10b5-1 trading-plan tables.", "insider,security,filing,transaction", "insider_transaction", "as_of_date", "available_at"),
        ("sec_blockholder_ownership", "sec_edgar", "SEC Schedule 13D/G blockholder ownership", "Structured Schedule 13D/G XML filings normalized into blockholder filing and reporting-person beneficial-owner tables.", "security,filing,reporting_person", "blockholder_filing", "event_date", "available_at"),
        ("sec_company_facts", "sec_edgar", "SEC company facts", "XBRL company facts normalized into PIT fundamental points.", "security_id,concept,period", "fundamental_points", "period_end", "available_at"),
        ("shares_outstanding_history", "sec_edgar", "Shares outstanding history", "PIT share-count history derived from SEC XBRL shares outstanding/basic average/diluted average facts.", "security_id,share_count_type,effective_date,accession", "shares_outstanding_history", "effective_date", "available_at"),
        ("daily_adjustment_factors", "atx_warehouse", "Daily adjustment factors", "PIT daily split and total-return adjustment factors materialized from daily bars and event-level corporate actions.", "security_id,trade_date,as_of_date,source", "daily_adjustment_factors", "as_of_date", "available_at"),
        ("xbrl_concept_catalog", "sec_edgar", "XBRL concept catalog", "Concept-level SEC companyfacts metadata with observed units, forms, availability, and fact coverage.", "source,taxonomy,concept", "xbrl_concept_catalog", "updated_at", "updated_at"),
        ("xbrl_taxonomy", "sec_edgar", "XBRL taxonomy relationships", "FASB US GAAP and SEC Reporting Taxonomy package metadata, presentation/calculation/definition relationships, dimensional edges, and observed SEC companyfacts frames.", "taxonomy_package,linkbase,role,concept_relationship", "xbrl_taxonomy_relationships", "release_year", "source_loaded_at"),
        ("xbrl_dimensions", "sec_edgar", "XBRL dimensional taxonomy", "Definition-linkbase dimensional edges for axes, domains, members, tables, and line items.", "taxonomy_package,role,source_concept,target_concept", "xbrl_dimension_edges", "release_year", "source_loaded_at"),
        ("xbrl_fact_frames", "sec_edgar", "SEC companyfacts frames", "Observed SEC companyfacts frame metadata by concept, unit, and calendar frame code.", "source,taxonomy,concept,unit,frame", "xbrl_fact_frames", "last_available_at", "last_available_at"),
        ("xbrl_filing_contexts", "sec_edgar", "SEC filing XBRL contexts", "Issuer filing-instance XBRL contexts and segment/scenario dimension members parsed from inline XBRL primary documents.", "security_id,accession_number,primary_document,context_id", "xbrl_filing_contexts", "acceptance_datetime", "source_loaded_at"),
        ("xbrl_filing_dimensions", "sec_edgar", "SEC filing XBRL dimensions", "Explicit and typed segment/scenario dimension members parsed from issuer inline XBRL filing contexts.", "security_id,accession_number,context_id,dimension,member", "xbrl_filing_dimensions", "acceptance_datetime", "source_loaded_at"),
        ("xbrl_filing_facts", "sec_edgar", "SEC filing inline XBRL facts", "Source filing-instance inline XBRL facts linked to parsed contexts by contextRef.", "security_id,accession_number,primary_document,context_ref,concept", "xbrl_filing_facts", "acceptance_datetime", "source_loaded_at"),
        ("xbrl_validation", "sec_edgar", "XBRL filing validation", "Calculation-linkbase and DQC-style filing validation results by rule, filing, context, and parent concept.", "validation_run_id,rule_family,accession_number,parent_concept,context", "xbrl_validation_results", "acceptance_datetime", "source_loaded_at"),
        ("fundamental_fact_revisions", "sec_edgar", "Fundamental fact revisions", "Accession-level SEC companyfacts revision chains with PIT ordering and value deltas.", "security_id,taxonomy,concept,unit,period,accession", "fundamental_fact_revisions", "filed_date", "available_at"),
        ("fundamental_statement_map", "sec_edgar", "Fundamental statement map", "Canonical statement mappings from SEC XBRL concepts into normalized income statement, balance sheet, cash-flow, per-share, share-count, and industry-template metrics.", "source,taxonomy,concept,industry_template", "fundamental_statement_map", "updated_at", "updated_at"),
        ("fundamental_statement_points", "sec_edgar", "Fundamental statement points", "PIT-safe normalized income, balance sheet, cash-flow, per-share, and share-count facts derived from mapped SEC companyfacts revision rows.", "security_id,statement,metric,period,accession", "fundamental_statement_points", "as_of_date", "available_at"),
        ("fundamental_ttm_points", "sec_edgar", "Fundamental TTM points", "PIT-safe trailing-twelve-month income, cash-flow, and per-share values derived from four available quarter-like normalized statement facts, including YTD-delta quarters where needed.", "security_id,metric,ttm_end_date,accession", "fundamental_ttm_points", "as_of_date", "available_at"),
        ("fundamental_periods", "sec_edgar", "Fundamental fiscal periods", "PIT-safe normalized reporting-period windows derived from SEC statement points with Compustat-style datadate/rdq/pdate/fdate/ldate fields, reported fiscal labels, calendar buckets, and revision coverage.", "security_id,period_start,period_end,accession", "fundamental_periods", "fdate", "available_at"),
        ("sec_fundamental_features", "sec_edgar", "SEC fundamental features", "PIT-safe accounting, valuation, cash-flow, margin, shareholder-yield, growth, and revision features derived from SEC companyfacts, TTM statement points, revision chains, and available market data.", "security_id,feature,as_of_date", "feature_values", "as_of_date", "available_at"),
        ("sec_submissions", "sec_edgar", "SEC submissions", "Company filing metadata by accession.", "security_id,accession_number", "sec_submissions", "filing_date", "acceptance_datetime"),
        ("nasdaq_symbol_directory", "nasdaq_trader", "Nasdaq symbol directory", "NASDAQ and other-listed symbol directory snapshots.", "symbol,as_of_date", "nasdaq_symbol_directory", "as_of_date", "source_loaded_at"),
        ("nasdaq_listing_events", "nasdaq_trader", "Nasdaq listing add/delete events", "Trading system add/delete action rows from Nasdaq Trader.", "symbol,effective_date,source_file_created_at", "nasdaq_listing_events", "effective_date", "source_loaded_at"),
        ("listing_status_intervals", "atx_warehouse", "Listing status intervals", "PIT listing-status intervals derived from Nasdaq directory snapshots and add/delete event checkpoints.", "symbol,listing_venue_code,valid_from", "listing_status_intervals", "valid_from", "available_at"),
        ("delist_code_dim", "atx_warehouse", "Delisting code dimension", "Public delisting proxy code dimension with explicit non-CRSP caveats and optional imputation policy metadata.", "delist_code", "delist_code_dim", "updated_at", "updated_at"),
        ("delisting_events", "atx_warehouse", "Delisting events", "PIT public delisting evidence derived from listing-status intervals, with nullable delisting-return proxy fields and lineage back to source events.", "symbol,listing_venue_code,delist_date", "delisting_events", "delist_date", "available_at"),
        ("delisting_return_observations", "atx_warehouse", "Observed delisting returns", "Injectable observed terminal-return facts such as CRSP DSEDELIST/DLRET rows, keyed by security or vendor identifier and delisting date.", "security_or_vendor_id,delist_date,provider", "delisting_return_observations", "delist_date", "available_at"),
        ("est_measure", "atx_warehouse", "Estimate measure dimension", "Canonical estimate measure codes and SEC XBRL concept mappings for actuals.", "measure_code", "est_measure", "source_loaded_at", "source_loaded_at"),
        ("est_detail", "injected_estimates", "Detail estimate observations", "Injectable analyst/broker-level estimate rows from licensed or public-aggregator files with provider, vendor-ID, revision, and source-file lineage.", "security_or_vendor_id,measure,period_end,broker,analyst,announcement", "est_detail", "as_of_date", "available_at"),
        ("est_consensus", "injected_estimates", "Consensus estimate snapshots", "Injectable IBES-style or normalized consensus snapshots with mean/median/high/low/stdev, revision counts, source-file hashes, provider identifiers, stable snapshot ids, and stale-after dates.", "security_or_vendor_id,measure,period_end,consensus_date", "est_consensus", "as_of_date", "available_at"),
        ("est_actual", "sec_edgar", "Estimate actuals", "Actual reported values mapped from SEC companyfacts into estimate measure codes.", "security_id,measure,period_end,accession", "est_actual", "as_of_date", "available_at"),
        ("est_surprise", "atx_warehouse", "Estimate surprises", "PIT standardized unexpected earnings and actual-versus-consensus surprise metrics derived from est_actual and optional est_consensus.", "security_id,measure,fiscal_period", "est_surprise", "as_of_date", "available_at"),
        ("est_guidance", "sec_edgar", "Management guidance observations", "Company-issued guidance from SEC 8-K Item 2.02/7.01 text extraction or licensed/injectable guidance feeds.", "security_id,measure,period_end,guidance_date,source", "est_guidance", "as_of_date", "available_at"),
        ("est_recommendation", "injected_estimates", "Broker recommendation and price-target observations", "Injectable IBES-style or normalized recommendation and 12-month price-target events with broker/analyst identifiers, rating-scale normalization, source-file hashes, and active-window fields.", "security_or_vendor_id,broker,analyst,event_type,rating_date", "est_recommendation", "as_of_date", "available_at"),
        ("est_recommendation_summary", "injected_estimates", "Recommendation and price-target summary snapshots", "Injectable aggregate recommendation distributions and monthly price-target summaries from IBES recdsum/ptgsum-style or normalized provider files.", "security_or_vendor_id,snapshot_date,source_vendor_table", "est_recommendation_summary", "snapshot_date", "available_at"),
        ("est_security_link", "atx_warehouse", "Estimate security identifier links", "PIT-safe links from estimate vendor identifiers such as IBES tickers, FactSet fsym IDs, CIQ trading items, and CUSIPs to warehouse security_id values.", "provider,vendor_security_id_type,vendor_security_id,target_security_id", "est_security_link", "as_of_date", "available_at"),
        ("fred_macro", "fred", "Macro observations", "Macro series observations from public FRED CSV downloads.", "series_id,observation_date", "macro_observations", "observation_date", "available_at"),
        ("trading_calendar", "atx_warehouse", "Trading calendar", "Open trading dates inferred from loaded daily bars.", "calendar_id,trade_date", "trading_calendar", "trade_date", "source_loaded_at"),
        ("universe_memberships", "atx_warehouse", "Research universes", "Point-in-time universe membership snapshots.", "universe_id,security_id,effective_date", "universe_memberships", "as_of_date", "available_at"),
        ("equity_daily_features", "tbltickerhistory", "Equity daily features", "Computed alpha-ready daily feature values and feature definitions.", "security_id,feature,as_of_date", "feature_values", "as_of_date", "available_at"),
        ("feature_build_manifests", "atx_warehouse", "Feature build manifests", "Feature build lineage, input windows, output windows, and row counts.", "feature_set,run_id", "feature_build_manifests", "output_max_as_of_date", "max_available_at"),
        ("feature_lineage", "atx_warehouse", "Feature lineage graph", "Feature-set catalog and dependency edges derived from feature definitions and source-input metadata.", "feature_set,feature_name,dependency", "feature_dependency_edges", "updated_at", "updated_at"),
        ("alpha_research", "atx_warehouse", "Alpha expression registry and backtest manifests", "PIT-safe alpha expression catalog, signal values, and long/short backtest summary manifests.", "alpha_id,security_id,as_of_date", "alpha_signal_values", "as_of_date", "available_at"),
        ("identifier_resolution_candidates", "sec_edgar", "Identifier resolution candidates", "Non-destructive CUSIP-to-security mapping candidates from public source evidence.", "source_key,target_security_id,source_period", "identifier_resolution_candidates", "as_of_date", "available_at"),
        ("identifier_resolution_decisions", "atx_warehouse", "Identifier resolution decisions", "Accepted, rejected, and review-pending decisions derived from identifier resolution evidence.", "candidate_id,decision_method", "identifier_resolution_decisions", "as_of_date", "available_at"),
        ("warehouse_jobs", "atx_warehouse", "Warehouse ETL jobs", "ETL job definitions, run history, retry policy, and event metadata.", "job_name,job_run_id", "etl_job_runs", "started_at", "finished_at"),
        ("warehouse_quality", "atx_warehouse", "Warehouse quality checks", "Recorded SQL data quality checks and verification outcomes.", "dataset_id,table_name,check_name,checked_at", "data_quality_checks", "checked_at", "checked_at"),
        ("warehouse_lake_exports", "atx_warehouse", "Lake export audit", "Parquet lake export run and file manifests recorded in DuckDB.", "export_run_id,object_name", "lake_export_files", "exported_at", "source_loaded_at"),
        ("warehouse_catalog", "atx_warehouse", "Warehouse catalog", "Source, dataset, table, field, raw artifact, run, and watermark metadata.", "catalog tables", "dataset_catalog", "updated_at", "updated_at"),
        ("provider_parity_matrix", "atx_warehouse", "Provider parity matrix", "Institutional-provider domain targets mapped to open-data substitutes and warehouse tables.", "provider,provider_domain", "provider_parity_matrix", "updated_at", "updated_at"),
    ]
    for row in dataset_rows:
        con.execute(
            """
            INSERT OR REPLACE INTO dataset_catalog (
                dataset_id, source_system_id, name, description, grain,
                primary_table, pit_column, available_at_column, updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, now())
            """,
            list(row),
        )

    table_rows = [
        ("schema_migrations", "control", "schema", "version", "Applied warehouse schema migrations.", '["version"]', "Control metadata only."),
        ("source_systems", "catalog", "source", "source_system_id", "Public/local data source catalog.", '["source_system_id"]', "Source metadata for audit and lineage."),
        ("dataset_catalog", "catalog", "dataset", "dataset_id", "Dataset catalog with PIT columns and ownership.", '["dataset_id"]', "Documents PIT and available-at columns where applicable."),
        ("table_catalog", "catalog", "table", "table_name", "Table and view catalog.", '["table_name"]', "Documents natural keys and PIT notes."),
        ("field_catalog", "catalog", "field", "table_name,field_name", "Column-level catalog populated from DuckDB metadata.", '["table_name","field_name"]', "Column semantics support as-of-safe research."),
        ("provider_parity_matrix", "catalog", "provider_parity", "provider,provider_domain", "Institutional provider parity matrix with open-data substitutes, limits, PIT notes, and public source URLs.", '["provider","provider_domain"]', "Research/catalog table; updated_at records seed refresh."),
        ("raw_source_files", "bronze", "source_artifact", "source_id", "Fetched/local source artifact registry.", '["source_id"]', "Tracks source URL, cache path, hash, byte count, and fetch status."),
        ("dataset_runs", "control", "dataset_run", "run_id", "Dataset loader run history.", '["run_id"]', "Every dataset load receives a run_id."),
        ("dataset_watermarks", "control", "watermark", "dataset_id,watermark_name", "Dataset high-water marks.", '["dataset_id","watermark_name"]', "Used for incremental/reload planning."),
        ("data_quality_checks", "control", "quality_check", "check_id", "Recorded data quality checks.", '["check_id"]', "Append-only check outcomes for warehouse observability."),
        ("etl_job_definitions", "control", "etl_job", "job_name", "Reusable ETL job definitions.", '["job_name"]', "Definitions include params, dependencies, and retry policy."),
        ("etl_job_runs", "control", "etl_job_run", "job_run_id", "ETL job run history.", '["job_run_id"]', "Links job runs to dataset run ids and records retry attempts."),
        ("etl_job_events", "control", "etl_event", "job_run_id,event_time", "ETL event log.", '["job_run_id","event_time"]', "Chronological job events, including retry attempts."),
        ("lake_export_runs", "audit", "lake_export", "export_run_id", "Lake export run audit records.", '["export_run_id"]', "One row per Parquet lake export run."),
        ("lake_export_files", "audit", "lake_export_file", "export_run_id,object_name", "Lake export file manifests recorded in DuckDB.", '["export_run_id","object_name"]', "Use sha256 and schema_sha256 to validate exported Parquet artifacts."),
        ("securities", "core", "security", "security_id", "Stable internal security master with PF-S5 current entity/security split.", '["security_id"]', "Use security_id as stable warehouse key; entity_id is the current sticky corporate entity; never ticker as primary key."),
        ("security_identifier_history", "core", "security_identifier", "security_id,id_type,id_value,valid_from", "PIT identifier bridge for CIK, ticker, ENTITY_ID, FIGI, LEI, ISIN, and internal matching evidence.", '["security_id","id_type","id_value","valid_from"]', "Use valid_from/valid_to and available_at for as-of joins. internal_cusip is non-redistributable support data and must not be exported."),
        ("exchange_listings", "core", "listing", "security_id,ticker,valid_from", "PIT exchange listing and ticker history.", '["security_id","ticker","valid_from"]', "Use valid_from/valid_to and available_at for as-of joins."),
        ("listing_status_intervals", "silver", "listing_status", "symbol,listing_venue_code,valid_from", "PIT listing status intervals derived from Nasdaq snapshot presence and add/delete event checkpoints.", '["listing_status_id"]', "Use valid_from/valid_to plus available_at for as-of-safe survivorship joins; source/evidence fields distinguish snapshot-derived and event-derived intervals."),
        ("delist_code_dim", "dimension", "delist_code", "delist_code", "Public delisting proxy code dimension; CRSP DLSTCD fields remain null unless licensed or reconciled evidence supplies them.", '["delist_code"]', "Imputation fields describe optional research policy and are not applied unless loader options request it."),
        ("delisting_events", "silver", "delisting_event", "source,source_listing_status_id,delist_code", "PIT public delisting evidence derived from listing-status intervals, with nullable delisting return and explicit imputation flags.", '["delisting_event_id"]', "Use delist_date/as_of_date/available_at for survivorship-safe joins; delisting_return is null unless a documented policy is explicitly applied."),
        ("delisting_return_observations", "silver", "delisting_return", "provider,security_or_vendor_id,delist_date", "Observed or licensed terminal-return observations such as CRSP DLRET/DLRETX/DLAMT/DLPRC rows.", '["delisting_return_observation_id"]', "Use available_at to model when the licensed/manual observation became available; delisting_events can link to the latest matching observation."),
        ("est_measure", "dimension", "estimate_measure", "measure_code", "Estimate measure dimension with canonical code labels and SEC concept mappings.", '["measure_code"]', "Seeded dimension; source_loaded_at records seed refresh."),
        ("est_broker", "dimension", "estimate_broker", "broker_id", "Broker/estimator dimension derived from injected detail estimate rows.", '["broker_id"]', "Provider and vendor_broker_id are vintage-scoped; do not treat raw numeric vendor IDs as globally stable identities."),
        ("est_broker_alias", "dimension", "estimate_broker_alias", "broker_id,alias_type,alias_value,valid_from", "Broker alias rows for vendor IDs, mask codes, and names.", '["broker_alias_id"]', "Use valid_from/valid_to and provider to handle vendor identifier reshuffles."),
        ("est_analyst", "dimension", "estimate_analyst", "analyst_id", "Analyst dimension derived from injected detail estimate rows.", '["analyst_id"]', "Provider and vendor_analyst_id are vintage-scoped; do not treat raw numeric vendor IDs as globally stable identities."),
        ("est_analyst_alias", "dimension", "estimate_analyst_alias", "analyst_id,alias_type,alias_value,valid_from", "Analyst alias rows for vendor IDs, mask codes, and names.", '["analyst_alias_id"]', "Use valid_from/valid_to and provider to handle vendor identifier reshuffles."),
        ("est_period_dim", "dimension", "estimate_period", "provider,measure_code,period_end,fpi", "Forecast-period dimension preserving IBES-style FPI, fiscal period labels, period end, and expected report date.", '["est_period_id"]', "Join on period_end, not FPI alone; FPI can be bumped around announcements."),
        ("est_detail", "silver", "estimate_detail", "security_or_vendor_id,measure_code,period_end,broker_id,analyst_id,announce_date", "Analyst/broker-level estimate observations from injectable files.", '["est_detail_id"]', "Use available_at for feed availability and announce/revision/stop dates for active-estimate as-of queries."),
        ("est_consensus", "silver", "estimate_consensus", "security_or_vendor_id,measure_code,period_end,consensus_date", "Consensus estimate snapshots from injectable provider files or adapters.", '["est_consensus_id"]', "Use available_at and consensus_date/as_of_date for PIT visibility; file loads are idempotent per source hash and as-of APIs return latest non-stale snapshots by default."),
        ("est_actual", "silver", "estimate_actual", "security_id,measure_code,fiscal_year,fiscal_period,accession_number", "Reported actual values mapped from SEC companyfacts.", '["security_id","measure_code","fiscal_year","fiscal_period","accession_number"]', "available_at is inherited from the source filing row and must not be restamped."),
        ("est_surprise", "gold", "estimate_surprise", "security_id,measure_code,fiscal_year,fiscal_period", "SUE and actual-versus-consensus surprise features.", '["security_id","measure_code","fiscal_year","fiscal_period"]', "available_at is inherited from originally reported actuals to avoid restatement lookahead."),
        ("est_guidance", "silver", "estimate_guidance", "security_id,measure_code,period_end,guidance_date,source", "Management guidance observations from SEC 8-K text extraction and injectable providers.", '["est_guidance_id"]', "Use available_at and as_of_date to model extraction/feed availability; extraction_confidence and evidence_text preserve public-data QA lineage."),
        ("est_recommendation", "silver", "estimate_recommendation", "security_or_vendor_id,broker_id,analyst_id,event_type,rating_date", "Broker recommendation and price-target events with canonicalized IBES-direction rating fields.", '["est_recommendation_id"]', "Use available_at plus announce/activation/revision/stop dates for as-of-safe active recommendation and price-target queries; Bloomberg-style reversed scales must be converted before mixing providers."),
        ("est_recommendation_summary", "silver", "estimate_recommendation_summary", "security_or_vendor_id,provider,source_vendor_table,snapshot_date", "Aggregate recommendation distributions and price-target summary snapshots with canonicalized IBES-direction rating means.", '["est_recommendation_summary_id"]', "Use available_at and snapshot_date for PIT visibility; scale_direction records provider numeric rating polarity before canonical conversion."),
        ("est_security_link", "core", "estimate_security_identifier", "provider,vendor_security_id_type,vendor_security_id,target_security_id", "Auditable PIT mapping from estimate feed security identifiers to warehouse security_id values.", '["est_security_link_id"]', "Use available_at and valid_from/valid_to before resolving vendor-keyed estimate rows; mappings accepted after a research date must not leak into earlier as-of queries."),
        ("sec_company_tickers", "bronze", "sec_ticker_map", "cik,ticker", "SEC company_tickers mapping snapshot.", '["cik","ticker"]', "SEC map is current-snapshot oriented; bridge into PIT identifier history before as-of use."),
        ("identifier_resolution_candidates", "core", "identifier_resolution", "source_dataset_id,source_key_type,source_key_value,target_security_id,source_period", "Auditable non-destructive identifier mapping candidates.", '["source_dataset_id","source_key_type","source_key_value","target_security_id","source_period"]', "Candidates are evidence, not automatic merges; use confidence/status before accepting."),
        ("identifier_resolution_decisions", "core", "identifier_resolution", "candidate_id,decision_method", "Auditable identifier mapping decisions and promotion evidence.", '["candidate_id","decision_method"]', "Accepted rows may promote PIT identifiers and source holdings; use available_at for as-of-safe consumption."),
        ("nasdaq_symbol_directory", "bronze", "symbol_directory", "directory,symbol,as_of_date", "Nasdaq Trader symbol directory snapshots.", '["directory","symbol","as_of_date"]', "Snapshot as_of_date and load time define availability."),
        ("nasdaq_listing_events", "bronze", "listing_event", "symbol,effective_date,source_file_created_at", "Nasdaq Trader trading-system add/delete rows for listing-status and survivorship evidence.", '["event_id"]', "Use effective_date for the listing action date and source_loaded_at/as_of_date for warehouse availability; repeated checkpoints are needed for full history."),
        ("tbltickerhistory_daily", "bronze", "price", "source,vendor_security_id,trading_date", "Raw-normalized local tbltickerhistory daily rows.", '["source","vendor_security_id","trading_date"]', "Raw local archive slice; available_at defaults to post-close."),
        ("equity_daily_bars", "silver", "price", "security_id,trade_date", "Canonical daily OHLCV bars.", '["source","security_id","trade_date"]', "available_at defaults to post-close."),
        ("corporate_actions", "silver", "corporate_action", "security_id,ex_date,action_type", "Corporate action facts when source data supports them.", '["source","security_id","ex_date","action_type"]', "Use ex_date and available_at to avoid lookahead."),
        ("corp_action_type_dim", "dimension", "corporate_action_type", "type_code", "Corporate-action type-code dimension aligned to CRSP distribution buckets and DTCC CAEV labels where public mappings are available.", '["type_code"]', "Small seed dimension; source_loaded_at records last seed refresh."),
        ("adjustment_factor_history", "silver", "adjustment_factor", "security_id,ex_date,event_type,source", "Event-level price/share/volume adjustment factors derived from corporate_actions.", '["adjustment_factor_id"]', "Use event_ref_id to audit the originating corporate_actions row; cumulative factors are event-chain products, not daily CFACPR/CFACSHR yet."),
        ("daily_adjustment_factors", "silver", "price_adjustment", "security_id,trade_date,as_of_date,source", "Daily split-only and total-return adjustment factors plus adjusted close derivatives.", '["daily_adjustment_id"]', "Use as_of_date and available_at to avoid applying future corporate actions."),
        ("shares_outstanding_history", "silver", "shares_outstanding", "source,security_id,share_count_type,effective_date,accession_number", "PIT share-count history derived from normalized SEC XBRL share-count facts.", '["share_history_id"]', "Use effective_date/as_of_date/available_at for as-of-safe market-cap and float-proxy research."),
        ("sec_company_facts", "bronze", "fundamental", "security_id,taxonomy,concept,unit,period_start,period_end,filed_date,accession_number", "SEC companyfacts XBRL facts.", '["security_id","taxonomy","concept","unit","period_start","period_end","filed_date","accession_number"]', "Append/revision-aware by filing date and accession."),
        ("xbrl_concept_catalog", "catalog", "xbrl_concept", "source,taxonomy,concept", "Observed SEC XBRL concept metadata, units, forms, date ranges, availability ranges, and coverage counts.", '["source","taxonomy","concept"]', "Derived from loaded facts; updated_at records catalog refresh and last_available_at records latest observed filing availability."),
        ("xbrl_taxonomy_packages", "catalog", "xbrl_taxonomy_package", "taxonomy_package_id", "Downloaded public FASB/XBRL taxonomy package metadata and checksums.", '["taxonomy_package_id"]', "source_loaded_at records package ingestion; release_year records the taxonomy vintage."),
        ("xbrl_taxonomy_roles", "catalog", "xbrl_taxonomy_role", "taxonomy_package_id,role_uri,linkbase_type,source_file", "Taxonomy role usage by package, linkbase, and source file.", '["taxonomy_package_id","role_uri","linkbase_type","source_file"]', "Roles are taxonomy metadata, not issuer PIT facts."),
        ("xbrl_taxonomy_relationships", "catalog", "xbrl_taxonomy_relationship", "taxonomy_package_id,linkbase_type,role,parent_concept,child_concept", "Presentation, calculation, and definition arcs parsed from public FASB/XBRL taxonomy linkbases.", '["relationship_id"]', "Use release_year/source_loaded_at to select taxonomy vintage; touches_observed_concept highlights concepts loaded from SEC companyfacts."),
        ("xbrl_dimension_edges", "catalog", "xbrl_dimension_edge", "taxonomy_package_id,role,source_concept,target_concept", "Definition-linkbase dimensional relationships including axes, domains, members, tables, and line items.", '["dimension_edge_id"]', "context_element/closed/usable preserve XBRL dimension metadata for segment/scenario analysis."),
        ("xbrl_fact_frames", "catalog", "xbrl_fact_frame", "source,taxonomy,concept,unit,frame", "Observed SEC companyfacts frame codes by concept/unit with parsed calendar period metadata and availability ranges.", '["source","taxonomy","concept","unit","frame"]', "SEC companyfacts frames are aggregate period metadata; use last_available_at for PIT availability."),
        ("xbrl_filing_contexts", "bronze", "xbrl_filing_context", "security_id,accession_number,primary_document,context_id", "XBRL context periods and entity identifiers parsed from SEC inline XBRL filing primary documents.", '["filing_context_id"]', "acceptance_datetime/filing_date preserve filing availability; source_loaded_at records extraction time."),
        ("xbrl_filing_dimensions", "bronze", "xbrl_filing_dimension", "filing_context_id,dimension_qname,member_qname,typed_member_value", "Explicit and typed segment/scenario dimension members attached to issuer filing contexts.", '["filing_dimension_id"]', "Joined to xbrl_filing_contexts for filing-period availability and to taxonomy dimension edges for taxonomy semantics."),
        ("xbrl_filing_facts", "bronze", "xbrl_filing_fact", "security_id,accession_number,primary_document,fact_ordinal", "Inline XBRL facts parsed from SEC primary filing documents with qname, contextRef, unitRef, raw value, and numeric value when safely normalized.", '["filing_fact_id"]', "filing_context_id links each fact to parsed context period/dimension metadata; acceptance_datetime and source_loaded_at preserve PIT availability."),
        ("xbrl_validation_results", "silver", "xbrl_validation_result", "validation_run_id,rule_family,accession_number,parent_concept,context_ref", "Persisted XBRL calculation-linkbase and DQC-style validation outcomes.", '["validation_id"]', "status/severity expose filing-level validation failures; source_loaded_at records validation execution time."),
        ("fundamental_fact_revisions", "silver", "fundamental_revision", "source,security_id,taxonomy,concept,unit,period_start,period_end,accession_number", "Accession-level SEC companyfacts revision chains with PIT ordering, latest flags, and value deltas.", '["source","security_id","taxonomy","concept","unit","period_start","period_end","accession_number"]', "Use revision_sequence, is_latest_revision, filed_date, and available_at for as-of-safe restatement analysis."),
        ("fundamental_statement_map", "catalog", "fundamental_statement_map", "source,taxonomy,concept,industry_template", "Canonical statement mapping for loaded SEC XBRL concepts, including bank/insurance/REIT overlays.", '["source","taxonomy","concept","industry_template"]', "Seeded public taxonomy mapping; updated_at records mapping refresh."),
        ("fundamental_statement_points", "silver", "fundamental_statement", "source,security_id,canonical_metric,unit,period_start,period_end,accession_number", "Normalized statement facts derived from mapped SEC companyfacts revision chains.", '["source","security_id","canonical_metric","unit","period_start","period_end","accession_number"]', "Use as_of_date/available_at plus revision_sequence/is_latest_revision for PIT-safe statement analysis."),
        ("fundamental_ttm_points", "gold", "fundamental_ttm", "source,security_id,canonical_metric,unit,ttm_end_date,anchor_statement_point_id", "Trailing-twelve-month statement values from four quarter-like PIT statement inputs, with YTD-delta quarter derivations for cumulative SEC facts.", '["source","security_id","canonical_metric","unit","ttm_end_date","anchor_statement_point_id"]', "TTM rows are anchored by the filing availability timestamp of visible quarter facts; use available_at and revision_sequence for PIT-safe restatement analysis."),
        ("fundamental_periods", "silver", "fundamental_period", "source,security_id,period_start,period_end,accession_number", "Normalized fiscal/reporting-period windows derived from statement facts.", '["source","security_id","period_start","period_end","accession_number"]', "Use datadate/rdq/pdate/fdate/ldate plus available_at/revision_sequence for PIT-safe period alignment; rdq/pdate come from matching 8-K Item 2.02 filings when present."),
        ("fundamental_points", "silver", "fundamental", "security_id,metric,period_start,period_end,as_of_date", "PIT fundamental points from SEC XBRL facts.", '["security_id","metric","period_start","period_end","as_of_date","accession_number"]', "Use as_of_date/available_at to avoid restatement lookahead; period_start separates duration facts from same-end-date cumulative facts."),
        ("sec_submissions", "bronze", "filing", "security_id,accession_number", "SEC submissions and filing metadata.", '["security_id","accession_number"]', "acceptance_datetime is the filing availability timestamp when present."),
        ("thirteenf_submissions", "bronze", "13f_submission", "accession_number,source_period", "SEC 13F submission rows from bulk ZIP data.", '["accession_number","source_period"]', "Filing/report periods are source-provided."),
        ("thirteenf_cover_pages", "bronze", "13f_cover", "accession_number,source_period", "SEC 13F cover page rows.", '["accession_number","source_period"]', "Derived from SEC bulk structured data."),
        ("thirteenf_summary_pages", "bronze", "13f_summary", "accession_number,source_period", "SEC 13F summary page rows.", '["accession_number","source_period"]', "Derived from SEC bulk structured data."),
        ("thirteenf_holdings", "silver", "13f_holding", "accession_number,infotable_sk,source_period", "SEC 13F security holdings.", '["accession_number","infotable_sk","source_period"]', "Use report and filing metadata for point-in-time availability."),
        ("thirteenf_managers", "silver", "13f_manager", "manager_id", "SEC 13F filing-manager master derived from cover pages and submissions.", '["manager_id"]', "Manager identity is keyed by normalized filing manager CIK; first/last filing/report fields are derived from visible source rows."),
        ("thirteenf_manager_reports", "silver", "13f_manager_report", "manager_report_id", "SEC 13F manager filing/report-period rows joined across submission, cover page, and summary page data.", '["manager_report_id"]', "Use report_period for holdings date and available_at for as-of-safe visibility after filing."),
        ("thirteenf_security_positions", "silver", "13f_position", "position_id", "Security-level 13F positions with manager metadata, portfolio weights, option flags, and voting authority.", '["position_id"]', "Derived from visible 13F holdings and manager report availability; supports manager/security drill-down."),
        ("thirteenf_security_ownership", "gold", "13f_ownership", "security_id,report_period,source_period", "PIT security-level 13F ownership aggregates and quarter-over-quarter flow fields.", '["ownership_id"]', "Use report_period/as_of_date plus available_at for as-of-safe ownership features; rows reflect currently loaded 13F holdings coverage."),
        ("insider", "silver", "insider", "insider_id", "Section 16 reporting-owner person dimension derived from EDGAR ownership XML.", '["insider_id"]', "Use reporting_owner_cik when present; source_loaded_at and filing-derived dates audit identity evidence."),
        ("filing_form4", "bronze", "ownership_filing", "accession_number", "SEC Forms 3/4/5 ownership XML filing metadata with issuer, period, acceptance, source URL, and footnotes.", '["accession_number"]', "available_at is SEC acceptance time when available, otherwise conservative filing-date end-of-day."),
        ("insider_relationship", "silver", "insider_relationship", "insider_id,security_id,valid_from", "Bitemporal insider-to-issuer role rows with director/officer/10-percent-owner flags and normalized officer titles.", '["relationship_id"]', "Use valid_from/valid_to plus available_at for PIT role membership."),
        ("insider_transaction", "silver", "insider_transaction", "filing_id,insider_id,transaction_ordinal", "Section 16 non-derivative and derivative transaction rows with 28-code transaction taxonomy, direct/indirect ownership, 10b5-1 indicators, and derivative fields.", '["transaction_id"]', "Use transaction_date/as_of_date and available_at to avoid filing lookahead; rule_10b5_1_indicator splits planned sales."),
        ("insider_holding", "silver", "insider_holding", "filing_id,insider_id,holding_ordinal", "Current beneficial-ownership holding rows disclosed on Forms 3/4/5 without a transaction event.", '["holding_id"]', "Use as_of_date and available_at for PIT holdings snapshots."),
        ("tradingplan_10b5_1", "gold", "trading_plan", "insider_id,security_id,adoption_date", "Trading-plan rows reconstructed from post-2023 Form 4/5 10b5-1 indicators and adoption dates.", '["plan_id"]', "Cooling-off days are derived from adoption date to first visible transaction date."),
        ("blockholder_filing", "bronze", "blockholder_filing", "accession_number", "Schedule 13D/G blockholder filing landing table, including post-2024 XML flag and purpose text.", '["accession_number"]', "available_at and filing_date define public visibility; parser coverage is follow-on S3/S7 work."),
        ("blockholder_reporting_person", "silver", "blockholder_person", "filing_id,reporting_person_seq", "Reporting-person rows from Schedule 13D/G beneficial-owner filings.", '["reporting_person_id"]', "Joined to blockholder_filing for filing availability and issuer context."),
        ("fund", "silver", "fund", "registrant_cik,series_id", "N-PORT fund dimension landing table for registered investment-company holdings.", '["fund_id"]', "Fund lifecycle dates and source_loaded_at audit series visibility."),
        ("fund_class", "silver", "fund_class", "fund_id,class_external_id", "N-PORT fund share-class landing table.", '["class_id"]', "Ticker availability is source-reported and not a primary identity key."),
        ("filing_nport", "bronze", "nport_filing", "accession_number", "N-PORT filing metadata landing table.", '["accession_number"]', "available_at captures public filing visibility."),
        ("fund_holding", "silver", "fund_holding", "filing_id,security_id,asset_cat,payoff_profile", "N-PORT fund holding landing table including asset category, derivative/debt payloads, and loan flags.", '["holding_id"]', "Use period_of_report and available_at for PIT fund holdings."),
        ("form144_intent", "bronze", "form144_intent", "accession_number", "Form 144 restricted-stock intent-to-sell landing table.", '["accession_number"]', "Approximate sale date and filing availability support later Form 4 reconciliation."),
        ("form144_to_form4_link", "silver", "form144_reconciliation", "form144_filing_id,insider_transaction_id", "Reconciliation links from Form 144 sale intent to subsequent Form 4 actual transaction rows.", '["form144_filing_id","insider_transaction_id"]', "Match confidence records reconciliation quality."),
        ("proxy_vote", "silver", "proxy_vote", "filing_id,security_id,meeting_date,matter_description", "N-PX proxy-vote landing table with vote category and shares-voted fields.", '["proxy_vote_id"]', "available_at records filing visibility for engagement analytics."),
        ("congressional_disclosure", "bronze", "congressional_disclosure", "disclosure_id", "STOCK Act congressional transaction-disclosure landing table.", '["disclosure_id"]', "filing_date and transaction_date expose disclosure lag as a feature."),
        ("finra_short_interest", "silver", "short_interest", "symbol,settlement_date,market_class_code", "FINRA consolidated short interest with latest-date full-market refresh and compact backfill support.", '["symbol","settlement_date","market_class_code"]', "available_at is conservatively stamped after settlement publication lag."),
        ("finra_short_interest_backfill_manifests", "control", "short_interest_backfill", "manifest_id", "FINRA all-symbol settlement-date backfill audit manifests.", '["manifest_id"]', "Records discovery candidates, selected settlement dates, loaded rows, feature rebuild rows, and run ids for repeatability."),
        ("macro_series", "bronze", "macro_series", "source,series_id", "Macro series metadata.", '["source","series_id"]', "Series metadata layer for macro observations."),
        ("macro_observations", "silver", "macro", "series_id,observation_date,as_of_date", "Macro time series observations.", '["source","series_id","observation_date","as_of_date"]', "FRED graph CSV is latest-revision data; use ALFRED/FRED API vintages for true macro PIT revisions."),
        ("trading_calendar", "silver", "calendar", "calendar_id,trade_date", "Trading calendar dates inferred from bars.", '["calendar_id","trade_date","source"]', "Derived calendar inherits availability from loaded bars."),
        ("universes", "gold", "universe", "universe_id", "Research universe definitions.", '["universe_id"]', "Rules are cataloged as JSON."),
        ("universe_memberships", "gold", "universe_member", "universe_id,security_id,effective_date", "PIT universe memberships.", '["universe_id","security_id","effective_date","as_of_date"]', "Use as_of_date and source_loaded_at for as-of membership."),
        ("feature_definitions", "gold", "feature_definition", "feature_set,feature_name", "Feature metadata, expressions, source inputs, and PIT policy.", '["feature_set","feature_name"]', "Defines how each feature should be interpreted and when it is available."),
        ("feature_values", "gold", "feature", "feature_set,feature_name,security_id,as_of_date", "Alpha feature store.", '["feature_set","feature_name","security_id","as_of_date"]', "Computed from as-of-safe source inputs."),
        ("feature_build_manifests", "gold", "feature_manifest", "feature_set,run_id", "Feature build audit manifests.", '["feature_set","run_id"]', "Use manifests to audit feature value source windows and output counts."),
        ("feature_set_catalog", "gold", "feature_lineage", "feature_set", "Feature-set version/family catalog derived from loaded definitions.", '["feature_set"]', "Use version_label, feature_family, and input_tables_json to audit feature-set compatibility."),
        ("feature_dependency_edges", "gold", "feature_lineage", "feature_set,feature_name,dependency_name", "Feature dependency graph linking features to source tables and derived feature inputs.", '["feature_set","feature_name","dependency_type","dependency_name"]', "Source-table and same-set derived-feature edges are rebuilt from feature definitions."),
        ("alpha_expression_catalog", "gold", "alpha_expression", "alpha_id", "Alpha expression registry with input features, PIT policy, weighting, ranking, and evaluation horizon metadata.", '["alpha_id"]', "Expressions are metadata; use input_features_json, available_at_policy, and params_json to audit signal construction."),
        ("alpha_signal_values", "gold", "alpha_signal", "alpha_id,security_id,as_of_date", "Materialized PIT alpha signals, cross-sectional ranks, and daily long/short weights.", '["alpha_id","security_id","as_of_date"]', "available_at is inherited from feature inputs; weights are computed cross-sectionally with no forward-return inputs."),
        ("alpha_backtest_manifests", "gold", "alpha_backtest", "backtest_id", "Alpha backtest summary manifests with evaluation window, signal counts, rank IC, hit rate, and long/short return metrics.", '["backtest_id"]', "Forward returns appear only in evaluation manifests; signal values remain PIT-safe research inputs."),
        ("security_identifiers", "legacy", "identifier", "symbol,id_type,id_value", "Legacy symbol identifier helper table.", '["symbol","id_type","id_value"]', "Prefer security_identifier_history for PIT research joins."),
        ("tbltickerhistory", "view", "price", "source,vendor_security_id,trading_date", "Compatibility view over tbltickerhistory_daily.", '["source","vendor_security_id","trading_date"]', "Raw local archive view; prefer equity_daily_bars for canonical research joins."),
        ("v_security_master_current", "view", "security", "security_id", "Current security master view.", '["security_id"]', "Convenience view; not point-in-time."),
        ("v_equity_daily_returns", "view", "price_return", "security_id,trade_date", "Daily simple/log return view.", '["source","security_id","trade_date"]', "Uses only same-day and prior closes."),
        ("v_alpha_daily_panel", "view", "alpha_panel", "security_id,as_of_date", "Daily alpha panel convenience view.", '["security_id","as_of_date"]', "Uses canonical bars and daily returns."),
        ("v_fundamental_points_latest", "view", "fundamental", "security_id,metric,period_end,unit", "Latest fundamental points by period.", '["security_id","metric","period_end","unit"]', "Current latest view; use as-of helper for PIT queries."),
        ("v_fundamental_statement_latest", "view", "fundamental_statement", "security_id,canonical_metric,period_end,unit", "Latest normalized statement points by canonical metric and period.", '["security_id","canonical_metric","period_end","unit"]', "Current latest view; use statement as-of helper for PIT queries."),
        ("v_fundamental_ttm_latest", "view", "fundamental_ttm", "security_id,canonical_metric,ttm_end_date,unit", "Latest trailing-twelve-month statement values by canonical metric and TTM end date.", '["security_id","canonical_metric","ttm_end_date","unit"]', "Current latest view; use TTM as-of helper for PIT queries."),
        ("v_fundamental_periods_latest", "view", "fundamental_period", "period_group_id", "Latest normalized reporting-period rows by period group.", '["period_group_id"]', "Current latest view; use fundamental_periods for PIT-safe period alignment."),
        ("v_sec_latest_filings", "view", "filing", "security_id,form", "Latest SEC filing by security and form.", '["security_id","form"]', "Current latest view; use filing dates for as-of workflows."),
        ("v_macro_latest", "view", "macro", "series_id,observation_date", "Latest macro observations.", '["series_id","observation_date"]', "Latest-revision macro view."),
        ("v_finra_short_interest_latest", "view", "short_interest", "symbol", "Latest FINRA short interest by symbol.", '["symbol"]', "Current latest view; use settlement_date/available_at for PIT."),
        ("v_thirteenf_positioning_by_security", "view", "13f_positioning", "source_period,cusip", "Aggregated 13F positioning by CUSIP and source period.", '["source_period","cusip"]', "Use report_period and source_period for availability modeling."),
    ]
    for row in table_rows:
        con.execute(
            """
            INSERT OR REPLACE INTO table_catalog (
                table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, now())
            """,
            list(row),
        )

    seed_fundamental_statement_map(store)
    seed_provider_parity_matrix(store)
    _seed_field_catalog(store)


COMMON_FIELD_DESCRIPTIONS = {
    "security_id": "Stable internal warehouse security identifier; do not use ticker as a primary key.",
    "entity_id": "Sticky PF-S5 corporate entity identifier above share-class security_id.",
    "issuer_id": "Issuer-level identifier where available.",
    "primary_symbol": "Current or source-preferred display ticker.",
    "symbol": "Ticker/symbol as published by the source for the row.",
    "ticker": "Exchange listing ticker or identifier value.",
    "cik": "SEC Central Index Key, zero-padded when normalized.",
    "cusip": "CUSIP identifier as reported or normalized from source data.",
    "internal_cusip": "Internal-only, non-redistributable CUSIP matching support; do not expose in public/lake exports.",
    "accession_number": "SEC accession number identifying a filing.",
    "run_id": "Dataset run id that loaded or computed the row.",
    "source": "Source system or loader name.",
    "source_url": "Source URL used for the row or artifact.",
    "source_loaded_at": "Warehouse load timestamp.",
    "available_at": "Timestamp when the observation is treated as available for PIT research.",
    "as_of_date": "Point-in-time date for the observation or revision.",
    "valid_from": "Inclusive start date for identifier/listing validity.",
    "valid_to": "Exclusive end date for identifier/listing validity; NULL means open ended.",
    "trade_date": "Trading date for daily market data.",
    "effective_date": "Date on which an event or point-in-time observation becomes economically effective.",
    "ex_date": "Ex-date for a distribution or corporate-action event.",
    "event_type": "Normalized event type used for corporate-action and adjustment-factor rows.",
    "classification_reason": "Audit reason explaining how a raw corporate-action row was normalized.",
    "type_code": "Corporate-action type code aligned to CRSP distribution buckets and DTCC CAEV labels where available.",
    "event_ref_id": "Deterministic identifier for the originating corporate-action event evidence.",
    "factor_price": "Event-level price adjustment multiplier.",
    "factor_shares": "Event-level shares-outstanding adjustment multiplier.",
    "factor_volume": "Event-level volume adjustment multiplier.",
    "split_price_factor": "Backward-cumulative split-only multiplier applied to raw daily prices for the snapshot.",
    "split_share_factor": "Backward-cumulative split-only multiplier applied to shares or volume for the snapshot.",
    "dividend_total_return_factor": "Backward-cumulative cash-dividend reinvestment multiplier for total-return adjusted close.",
    "total_return_price_factor": "Combined split and dividend multiplier applied to raw daily prices for total-return adjusted close.",
    "raw_close": "Unadjusted daily close carried from the source bar.",
    "split_adjusted_close": "Raw close adjusted only for visible future split events in the snapshot.",
    "total_return_adjusted_close": "Raw close adjusted for visible future split and cash-dividend events in the snapshot.",
    "split_adjusted_volume": "Daily volume restated into post-split share units for the snapshot.",
    "visible_event_count": "Count of visible future adjustment-factor events considered for the daily factor row.",
    "split_event_count": "Count of visible future split events contributing to split factors.",
    "cash_div_event_count": "Count of visible future cash-dividend events contributing to total-return factors.",
    "last_factor_ex_date": "Latest visible future event ex-date considered for the daily factor row.",
    "cumulative_price_factor": "Product of visible event price factors for the security's adjustment chain at the event row.",
    "cumulative_share_factor": "Product of visible event share factors for the security's adjustment chain at the event row.",
    "share_count_type": "Canonical share-count measure, such as shares_outstanding, shares_basic_avg, or shares_diluted_avg.",
    "share_count": "Share-count value for the row, expressed in shares.",
    "observation_date": "Macro observation date.",
    "period_start": "Fundamental reporting period start date.",
    "period_end": "Fundamental reporting period end date.",
    "datadate": "Compustat-style fiscal period end date; mirrors period_end for fundamental_periods.",
    "rdq": "Earnings report date, inferred from the matching SEC 8-K Item 2.02 report date when available.",
    "pdate": "Preliminary earnings-release date; currently mirrors rdq when an Item 2.02 filing is matched.",
    "fdate": "Formal 10-Q/10-K filing date for the fundamental period.",
    "ldate": "Latest known vintage date for the period revision chain.",
    "filed_date": "SEC filing date.",
    "filing_date": "SEC filing date.",
    "acceptance_datetime": "SEC acceptance timestamp when supplied by submissions metadata.",
    "open": "Daily open price.",
    "high": "Daily high price.",
    "low": "Daily low price.",
    "close": "Daily close price.",
    "adjusted_close": "Source adjusted close price when available.",
    "volume": "Daily share volume.",
    "value": "Numeric observation value.",
    "value_usd": "Holding value in U.S. dollars.",
    "feature_name": "Feature identifier within a feature set.",
    "feature_set": "Feature namespace/version.",
    "input_hash": "Optional deterministic hash of feature inputs.",
}


def _semantic_type(column_name: str, data_type: str) -> str:
    name = column_name.lower()
    dtype = data_type.upper()
    if name in {"security_id", "entity_id", "issuer_id", "source_id", "run_id", "job_run_id", "dataset_id", "source_system_id", "universe_id"}:
        return "identifier"
    if name in {"cik", "cusip", "internal_cusip", "figi", "accession_number", "ticker", "symbol", "vendor_security_id", "series_id"}:
        return "identifier"
    if name.endswith("_date") or dtype == "DATE":
        return "date"
    if name.endswith("_at") or "TIMESTAMP" in dtype or "DATETIME" in dtype:
        return "timestamp"
    if dtype == "BOOLEAN":
        return "flag"
    if any(token in name for token in ("price", "open", "high", "low", "close", "vwap")):
        return "price"
    if any(token in name for token in ("volume", "shares", "quantity")):
        return "quantity"
    if any(token in name for token in ("value", "amount", "factor", "return", "percent", "rate", "weight", "ratio")):
        return "measure"
    if "json" in name:
        return "json"
    if "DOUBLE" in dtype or "INTEGER" in dtype or "BIGINT" in dtype or "DECIMAL" in dtype:
        return "measure"
    return "text"


def _field_unit(column_name: str) -> str | None:
    name = column_name.lower()
    if name in {"value_usd", "cash_amount", "dollar_volume", "table_value_total"} or name.endswith("_usd"):
        return "USD"
    if "volume" in name or "share" in name or "quantity" in name:
        return "shares"
    if "percent" in name or name.endswith("_return") or name.startswith("ret_") or name.startswith("mom_"):
        return "ratio"
    if name.endswith("_date") or name in {"valid_from", "valid_to"}:
        return "date"
    if name.endswith("_at"):
        return "timestamp"
    return None


def _field_description(table_name: str, column_name: str, semantic_type: str) -> str:
    if column_name in COMMON_FIELD_DESCRIPTIONS:
        return COMMON_FIELD_DESCRIPTIONS[column_name]
    cleaned = column_name.replace("_", " ")
    return f"{cleaned.capitalize()} field on {table_name}; inferred semantic type: {semantic_type}."


def _seed_field_catalog(store: DuckDBStore) -> None:
    con = store.con
    rows = con.execute(
        """
        SELECT table_name, column_name, data_type, is_nullable
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND coalesce(internal, false) = false
          AND table_name NOT LIKE 'duckdb_%'
          AND table_name NOT LIKE 'sqlite_%'
          AND table_name NOT LIKE 'pragma_%'
        ORDER BY table_name, column_index
        """
    ).fetchall()
    if not rows:
        return
    # Bulk-insert in a single statement. Looping a per-row INSERT OR REPLACE over
    # the ~2500 warehouse columns cost ~13s of bootstrap (each row = its own
    # append + PK probe). Register the computed rows as one relation and let
    # DuckDB ingest them vectorized instead.
    import pandas as pd

    records = [
        (
            table_name,
            column_name,
            (semantic_type := _semantic_type(column_name, data_type)),
            _field_description(table_name, column_name, semantic_type),
            bool(is_nullable),
            _field_unit(column_name),
            None,
        )
        for table_name, column_name, data_type, is_nullable in rows
    ]
    seed_frame = pd.DataFrame(
        records,
        columns=[
            "table_name",
            "field_name",
            "semantic_type",
            "description",
            "nullable",
            "unit",
            "source_field",
        ],
    )
    con.register("_field_catalog_seed", seed_frame)
    try:
        con.execute(
            """
            INSERT OR REPLACE INTO field_catalog (
                table_name,
                field_name,
                semantic_type,
                description,
                nullable,
                unit,
                source_field,
                updated_at
            )
            SELECT table_name, field_name, semantic_type, description,
                   nullable, unit, source_field, now()
            FROM _field_catalog_seed
            """
        )
    finally:
        con.unregister("_field_catalog_seed")
