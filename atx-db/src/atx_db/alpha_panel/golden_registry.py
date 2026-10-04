"""Complete raw features, controls and lineage contracts; no data IO."""
from . import char_monthly_registry as M
from .golden_sources import digest, UNSUPPORTED


def registry():
    fields = {}
    for f in M.REGISTRY:
        record = f.record()
        fields[f.name] = {"name": f.name, "type": "DOUBLE", "unit": f.unit,
            "definition": f.formula, "source": "char_monthly", "source_columns": list(f.dependencies),
            "clock": f.source_clock, "staleness": f.staleness, "history": f.history,
            "structural_null": record["sector_applicability"], "point_in_time": True,
            "non_pit_aspects": [], "kind": "raw_feature", "economic_sign": f.economic_sign,
            "stored_sign": "natural", "unsupported_reason": f.unsupported_reason,
            "bronze_to_gold": record["primitive_dependencies"], "missingness": f.missingness}
    for name, source, definition, unit in (
        ("ret", "panel", "latest return whose source clock is strictly before cutoff; no forward label", "decimal"),
        ("ret_excess", "factors", "return less independently clocked comparable risk-free observation", "decimal"),
        ("me", "universe_release_v1", "latest independently admitted USD cap strictly before cutoff", "USD"),
        ("sue_eps_ann", "ea_actuals", "seasonal earnings surprise scaled by eight prior visible surprises", "dimensionless"),
        ("guid_surprise", "events_guidance_v2", "comparable actual EPS less earlier guidance / prior admitted share-basis price", "dimensionless"),
        ("guid_revision", "events_guidance_v2", "comparable current minus strictly earlier guidance midpoint", "native guidance units"),
        ("ownership_breadth_change", "thirteenf_features", "breadth change on consistent reporting-manager cohort", "count"),
        ("institutional_ownership_change_split_adjusted", "thirteenf_features", "split-adjusted held-share ownership change with dated denominator and consistent cohort", "fraction"),
        ("bseg_n", "fundamentals_notes", "independently pinned curated reportable member count", "count"),
        ("bseg_rev_hhi", "fundamentals_notes", "reported-segment-measure HHI; independently admitted compatible contexts/bridge", "dimensionless"),
        ("foreign_sales_share", "fundamentals_notes", "independently admitted external geographic sales share", "fraction"),
        ("tax_dtl", "fundamentals_notes", "deferred tax liabilities; ambiguous public key excluded", "USD"),
        ("short_interest_ratio", "short_interest", "short shares / dated independently qualified shares", "fraction"),
        ("short_volume_ratio", "short_volume_ext", "qualified short volume / comparable total volume", "fraction"),
        ("event_score", "events", "native registry event score; definition admission pending", "dimensionless"),
        ("text_score", "text_features", "native registry text score; definition admission pending", "dimensionless"),
    ):
        fields[name] = {"name": name, "type": "DOUBLE", "unit": unit, "definition": definition,
            "source": source, "source_columns": [name], "clock": "strict original available_at < cutoff",
            "staleness": "native contract; admission pending" if source in UNSUPPORTED else "400 calendar days",
            "history": "native complete history required", "structural_null": "unknown source or applicability -> NULL",
            "point_in_time": True, "non_pit_aspects": [], "kind": "raw_feature", "stored_sign": "natural",
            "bronze_to_gold": {"source": source}, "missingness": "qualified native source required; never zero-fill"}
    # Full actual standardized/ratio registries, with their unchanged formulas.
    from . import compustat_std as S, ratios as R, char_registry as D
    standardized=(*S.FUNDQ_ITEMS,*S.FUNDA_ITEMS,*S.catalog_names(),*((n,n,u,d) for n,u,d in S.DERIVED_Q))
    for name, native_input, unit, definition in sorted(set(standardized)):
        key = "std_" + name
        fields[key] = {**fields["tax_dtl"], "name": key, "source": "standardized_v11", "source_columns": [name],
            "unit": unit, "definition": definition, "native_name": name,
            "bronze_to_gold": {"primitive": native_input, "native_mnemonic": name}}
    for r in (*R.RATIOS, *R.COMPOSITES):
        key = "ratio_" + r["name"]
        fields[key] = {**fields["tax_dtl"], "name": key, "source": "ratios_v11", "source_columns": list(r["inputs"]),
            "unit": "native ratio unit", "definition": r["definition"], "formula": r["expr"],
            "structural_null": str(r["undef"]), "bronze_to_gold": {"ratio": r["name"]}}
    for f in D.REGISTRY:
        key = "daily_" + f.name
        fields[key] = {**fields["ret_excess"], "name": key, "source": "characteristics",
            "source_columns": list(f.inputs), "definition": f.notes or str(f.sql_or_callable),
            "kind": f.kind, "stored_sign": f.prior_sign, "history": f.lookback_sessions,
            "bronze_to_gold": {"native_registry": f.as_dict()}}
    ownership={'inst_shares':'shares','inst_n_holders':'count','inst_ownership':'fraction',
        'inst_shares_change':'shares','inst_ownership_change':'fraction','inst_breadth_change':'count',
        'inst_new_positions':'count','inst_closed_positions':'count','inst_persistent_holders':'count',
        'inst_hhi':'dimensionless','inst_top5_share':'fraction','observed_shares':'shares',
        'contributing_managers':'count','common_manager_share_change':'shares','denominator_shares':'shares',
        'observed_n_holders':'count','comparison_population_managers':'count'}
    for name,unit in ownership.items():
        fields[name]={**fields['ownership_breadth_change'],'name':name,'source_columns':[name],
            'unit':unit,'definition':'unchanged native thirteenf_features '+name+' on dated admitted class links and consistent reporting cohort',
            'bronze_to_gold':{'native_stage':'thirteenf_features','native_column':name}}
    for name,unit in {'si_shares':'shares','si_prev':'shares','si_dtc':'days','adv_finra':'shares/day',
                      'short_volume':'shares','short_exempt_volume':'shares','total_volume':'shares'}.items():
        source='short_interest' if name in {'si_shares','si_prev','si_dtc','adv_finra'} else 'short_volume_ext'
        fields[name]={**fields['short_interest_ratio'],'name':name,'source':source,'source_columns':[name],
            'unit':unit,'definition':'native '+source+' '+name+'; dissemination/dated mapping admission required',
            'bronze_to_gold':{'native_stage':source,'native_column':name}}
    from . import ea_actuals as EA
    import pyarrow as pa
    for field in EA.FEATURE_SCHEMA:
        if not pa.types.is_floating(field.type) or field.name=='sue_eps_ann':continue
        key='ea_'+field.name
        fields[key]={**fields['sue_eps_ann'],'name':key,'source_columns':[field.name],
            'definition':'unchanged native ea_actuals '+field.name+'; original fiscal/calendar/currency/basis and source clock',
            'unit':'native original units; earnings comparability admission required',
            'bronze_to_gold':{'native_product':'ann_features.parquet','native_column':field.name}}
    for name, direction in PRIMARY:
        fields['score_'+name] = {**fields[name], 'name':'score_'+name,
            'source':'golden_cross_section', 'source_columns':[name], 'unit':'dimensionless',
            'kind':'normalized_score', 'stored_sign':'natural', 'economic_sign':1,
            'definition':f'{direction} * (2 * (average tie rank - 1)/(qualified count - 1) - 1); all_common qualified cross section; at least 100 names',
            'clock':'maximum source clock of participating qualified cross section, strictly before cutoff',
            'missingness':'unqualified, absent, outside all_common or fewer than 100 qualified names -> NULL',
            'bronze_to_gold':{'raw_field':name,'direction':direction}}
    return fields


PRIMARY = [("be_me", 1), ("gp_at", 1), ("ret_12_1", 1), ("ni_be", 1), ("at_gr1", -1),
           ("oaccruals_at", -1), ("rvol_21d", -1), ("ivol_capm_252d", -1), ("sue_eps_ann", 1),
           ("guid_surprise", 1), ("ownership_breadth_change", 1),
           ("institutional_ownership_change_split_adjusted", 1), ("op_at", 1)]


def evaluation_config():
    return {"schema": "atx.golden-evaluation/v1", "primary": [{"name": n, "direction": s} for n, s in PRIMARY],
        "owner_directions": {"ownership breadth change": "ownership_breadth_change",
            "split-adjusted institutional ownership change": "institutional_ownership_change_split_adjusted",
            "operating profitability": "op_at", "annual asset growth": "at_gr1"},
        "deduplication": {"annual asset growth": "same economic definition as at_gr1; counted once"},
        "exploratory": [{"name":n,"direction":c.get("economic_sign",1),"stored_sign":c.get("stored_sign","natural")} for n,c in sorted(registry().items())], "universes": ["univ_all_common", "univ_r3000p"],
        "start": "2020-01-01", "end_exclusive": "2023-01-01", "horizons": [5, 21, 63],
        "entry": "next session close; returns of subsequent h complete sessions; exit feature index+h+1",
        "delisting": "qualified total return plus independently admitted dlret proceeds and residual cash convention; unknown -> missing",
        "minimum_dates": 126, "minimum_names": 100, "nyse_minimum_names": 20,
        "weight_cap": 0.05, "hac_lag": "max(horizon-1,21)", "fdr_q": 0.05,
        "endpoints": ["IC", "spread"], "cost_bps": [0, 10, 25, 50],
        "development_subset": None, "purge_embargo_sessions": 63,
        "history_qualification": "prior research exposure possible; no untouched holdout claim",
        "turnover": "sum absolute signed-long-short weight changes, include closing missing names; initial from cash",
        "incremental_controls": ["log_me", "ret_12_1", "rvol_21d"],
        "monthly_carry": "native monthly value strictly after documented availability; bounded staleness",
        "liquidity": "pre-entry independently admitted ADV; missing excluded from cost/liquidity evidence"}
