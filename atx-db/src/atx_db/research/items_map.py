"""Seed-pinned accounting item vocabulary shared by F.1 and 2.9.

Only mnemonic-to-canonical-item bridges live here. XBRL aliases, validity windows,
signs, scales and canonical composition rules come from the existing seed readers.
Missing components never silently acquire the seed engine's optional zero fills.
"""
from __future__ import annotations

import datetime as dt
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from .fundamental_sources import canonical_sha256, file_sha256

MAP_VERSION = "fundamental_items_v1"
SEEDS = Path(__file__).resolve().parents[1] / "seeds"
UNIT_DISCREPANCIES = {1106: ("quantity", "monetary"),
                      1203: ("quantity", "monetary"),
                      1301: ("ratio", "monetary")}


@dataclass(frozen=True)
class ItemChain:
    mnemonic: str
    item_ids: tuple[int, ...]
    additive: bool = True
    magnitude: bool = False
    missing_policy: str = "null"
    unsupported_reason: str | None = None


_ITEM_IDS = {
    "AT": (1101,), "ACT": (1102,), "LCT": (1202,), "CHE": (1103,),
    "CASH": (1104,), "IVST": (1105,), "RECT": (1106,), "INVT": (1107,),
    "XPP": (1108,), "PPENT": (1110,), "PPEGT": (1111,), "INTAN": (1113,),
    "GDWL": (1114,), "IVAO": (1117,), "LT": (1201,), "DLC": (1205,),
    "DLTT": (1207,), "DEBT": (1208,), "TXDITC": (1211,), "PSTK": (1214,),
    "SEQ": (1221,), "CEQ": (1220,), "TEQ": (1222,), "MIB": (1213,), "BE": (-100004,),
    "AP": (1203,), "XACC": (1204,), "DRC": (1210,), "RE": (1217,),
    "SALE": (1001,), "COGS": (1003,), "GP": (1004,), "XSGA": (1005,),
    "XRD": (1008,), "XAD": (1009,), "DP": (1307, 1011), "OIADP": (1014, 1017, -100002),
    "EBIT_BEST": (1014, 1017, -100002),
    "OI": (1014,), "EBITDA": (1016, 1015), "XINT": (1018,), "PI": (1023,),
    "TXT": (1024,), "IB": (1029,), "NI": (1031,), "OANCF": (1301, 1302),
    "IVNCF": (1303,), "FINCF": (1304,), "CAPX": (1305,), "AQC": (1309,),
    "DVC": (1316,), "PRSTKC": (1312,), "SSTK": (1311,), "DLTIS": (1313,),
    "DLTR": (1314,), "DLCCH": (-100003,), "TXPD": (-100001,), "CSHO": (1039,),
    "WAB": (1040,), "WAD": (1041,), "EPSPI": (1034,), "EPSFI": (1035,),
    "WCAR": (1319,), "WCINV": (1320,), "WCAP": (1321,), "WC": (1322,),
}
COMPUSTAT_ANALOG: dict[str, ItemChain] = {
    key: ItemChain(key, ids, additive=key not in {"WAB", "WAD", "EPSPI", "EPSFI", "CSHO"},
                   magnitude=key in {"CAPX", "AQC", "DVC", "PRSTKC", "DLTR"},
                   missing_policy="xrd_zero_requires_visible_policy" if key == "XRD" else "null",
                   unsupported_reason="no_seeded_canonical_definition" if not ids else None)
    for key, ids in _ITEM_IDS.items()
}


def load_mapping() -> dict[str, Any]:
    """Compile small metadata using the authoritative seed readers, never bulk pandas."""
    from atx_db.item_registry import read_fundamental_item_seed
    from atx_db.standardization import read_standardization_rules, rule_input_kinds
    from atx_db.statement_map_seed import read_statement_map_seed

    seed = read_fundamental_item_seed()
    statements = read_statement_map_seed()
    rules = read_standardization_rules()
    items = {r.item_id: r for r in seed}
    needed = {i for c in COMPUSTAT_ANALOG.values() for i in c.item_ids}
    active = {(r.item_id, r.basis): r for r in rules if r.is_active}
    while True:
        extra = {i for r in active.values() if r.item_id in needed for i in r.source_item_ids}
        if extra <= needed:
            break
        needed |= extra
    validity = {(r.item_id, r.alias_scheme, r.alias_code): (r.valid_from, r.valid_to)
                for r in seed if r.alias_code}
    statement = {}
    for row in sorted(statements, key=lambda r: r.industry_template != "ALL"):
        if row.is_active and row.item_id is not None:
            statement.setdefault((row.item_id, row.taxonomy, row.concept), row)
    output, aliases = [], []
    for (item_id, basis), rule in sorted(active.items()):
        if item_id not in needed or basis not in {"instant", "annual", "quarterly", "ttm"}:
            continue
        raw = items[item_id]
        unit = UNIT_DISCREPANCIES.get(item_id, (None, raw.unit_type))[1]
        entry = {**asdict(rule), "period_kind": raw.data_type, "unit_type": unit,
                 "seed_unit_type": raw.unit_type, "input_kinds": rule_input_kinds(rule)}
        output.append(entry)
        for alias in rule.source_aliases:
            key = item_id, alias.alias_scheme, alias.alias_code
            srow = statement.get(key)
            # Missing statement contexts cannot silently supply a sign or scale.
            if srow is None:
                continue
            if item_id in UNIT_DISCREPANCIES and srow.unit_type != unit:
                raise ValueError(f"unit authority disagreement for {key}")
            start, end = validity.get(key, (None, None))
            aliases.append({"item_id": item_id, "basis": basis, "canonical_code": rule.canonical_code,
                            "taxonomy": alias.alias_scheme, "concept": alias.alias_code,
                            "priority": alias.priority, "valid_from": start or "1900-01-01",
                            "valid_to": end, "unit_type": unit, "period_kind": raw.data_type,
                            "multiplier": srow.value_multiplier, "rule_id": rule.rule_id,
                            "sign_rule": rule.sign_rule, "scale_rule": rule.scale_rule,
                            "rule_valid_from": str(rule.valid_from or dt.date(1900, 1, 1)),
                            "rule_valid_to": str(rule.valid_to) if rule.valid_to else None})
    # C-107 authorizes only these exact already-approved definitions as local
    # recipes. Negative identifiers explicitly cannot masquerade as durable STD IDs.
    for basis in ("annual", "quarterly", "ttm"):
        for local_id, code, operands, combination in (
            (-100001, "cash_taxes_paid", [], "coalesce_priority"),
            (-100002, "ebit_best_pretax_plus_interest", [1023, 1018], "sum"),
            (-100003, "short_term_debt_net_cash_borrowing", [], "coalesce_priority"),
        ):
            rule_id = f"C{108 if local_id == -100003 else 107}_{basis}_{code}"
            output.append({"item_id": local_id, "canonical_code": code, "basis": basis,
                           "rule_id": rule_id, "source_item_ids": operands,
                           "input_kinds": ["output"]*len(operands), "combination_rule": combination,
                           "missing_policy": "skip", "sign_rule": "statement_normalized",
                           "scale_rule": "identity", "valid_from": "1900-01-01", "valid_to": None,
                           "unit_type": "monetary", "seed_unit_type": None,
                           "period_kind": "duration", "source_aliases": [], "is_active": True})
            if local_id in {-100001, -100003}:
                concepts = ((10, "IncomeTaxesPaidNet"), (20, "IncomeTaxesPaid")) if local_id == -100001 else (
                    (10, "ProceedsFromRepaymentsOfShortTermDebt"),)
                for priority, concept in concepts:
                    aliases.append({"item_id": local_id, "basis": basis, "canonical_code": code,
                                    "taxonomy": "us-gaap", "concept": concept, "priority": priority,
                                    "valid_from": "1900-01-01", "valid_to": None, "unit_type": "monetary",
                                    "period_kind": "duration", "multiplier": 1.0, "rule_id": rule_id,
                                    "sign_rule": "statement_normalized", "scale_rule": "identity",
                                    "rule_valid_from": "1900-01-01", "rule_valid_to": None})
                    if local_id == -100003:
                        aliases[-1]["taxonomy_versions"] = [f"us-gaap/{year}" for year in [2008, 2009, *range(2011, 2027)]]
    output.append({"item_id": -100004, "canonical_code": "book_equity_jkp", "basis": "instant",
                   "rule_id": "C109_instant_book_equity_jkp", "source_item_ids": [1221, 1220, 1214, 1101, 1201, 1211],
                   "input_kinds": ["item", "output", "item", "item", "item", "output"],
                   "combination_rule": "book_equity_jkp", "missing_policy": "C109_ordinary_absence_only",
                   "period_kind": "instant", "unit_type": "monetary", "seed_unit_type": None})
    metadata = {"version": MAP_VERSION, "chains": {k: asdict(v) for k, v in COMPUSTAT_ANALOG.items()},
                "rules": output, "aliases": aliases,
                "seed_hashes": {name: file_sha256(SEEDS / name) for name in
                                ("fundamental_items.csv", "statement_map.csv", "standardization_rules.csv")},
                "unit_authority": "C-105_statement_map_context_and_approved_F1_definitions",
                "unit_discrepancies": UNIT_DISCREPANCIES,
                "source_unit_validation": {"monetary": ["USD"], "quantity": ["shares"],
                                           "per_share": ["USD/shares"]},
                "missing_policy": "null_unless_explicit_historical_policy_evidence",
                "unsupported": {k: v.unsupported_reason for k, v in COMPUSTAT_ANALOG.items()
                                if v.unsupported_reason},
                "local_recipe_authority": {"ruling": "C-107", "durable_STD_ids": "absent",
                    "DLCCH": "C-108: positive net short-term cash borrowing, verified FSDS taxonomy versions only",
                    "design_sha256": file_sha256(Path(__file__).resolve().parents[4] /
                                                 "docs/superpowers/plans/2026-09-25-tier1-v2-s2-fundamentals.md"),
                    "TXPD": "IncomeTaxesPaidNet -> IncomeTaxesPaid (FSDS-only where CF allowlist lacks tags)",
                    "OIADP_and_EBIT_BEST": "operating_income -> ebit__1017 -> pretax_income+interest_expense_total",
                    "BE": "C-109: SEQ+TXDITC-PSTK, ordinary absent supported TXDITC/PSTK only may be zero; explicit invalid/NULL retained",
                    "TXDITC": "C-109 existing seeded item1211 deferred-tax-liability analog; total-net then noncurrent fallback, no separate investment-credit addition",
                    "SEQ_chain_for_BE": "stockholders_equity -> common_equity+preferred_stock -> total_assets-total_liabilities"},
                "limitations": ["F2 ebit_to_ev uses OI operating-income, not OIADP/EBIT_BEST",
                                "TXDITC is seeded deferred-tax-liability analog; fallback is noncurrent, not a newly verified complete Compustat stock",
                                "DEI cover-page shares cannot prove fiscal balance dates",
                                "XRD missing-zero convention requires explicit then-visible evidence"]}
    evidence_root = Path(__file__).resolve().parents[4] / ".superpowers/sdd/tier1-v2/receipts"
    metadata["local_tag_authority"] = {name: file_sha256(evidence_root / name) for name in
                                       ("session8-taxonomy.out", "session8-taxonomy-remaining.out")}
    # Normalize dates/tuples before serializing and hashing the portable plan.
    import json
    metadata = json.loads(json.dumps(metadata, default=str))
    metadata["sha256"] = canonical_sha256(metadata)
    return metadata


def live_alias(alias: dict[str, Any], end: dt.date) -> bool:
    return ((not alias.get("valid_from") or str(end) >= alias["valid_from"])
            and (not alias.get("valid_to") or str(end) < alias["valid_to"]))
