"""13F filer-type rules: name normalisation, ordered classification, private-fund split, family aggregation."""

from __future__ import annotations

from pathlib import Path

from atx_db.alpha_panel import filer_type as F


def test_norm_name_strips_forms_and_state_tags() -> None:
    assert F.norm_name("VANGUARD GROUP INC") == "VANGUARD GROUP"
    assert F.norm_name("BlackRock, Inc.") == "BLACKROCK"
    assert F.norm_name("BANK OF AMERICA CORP /DE/") == "BANK OF AMERICA"
    assert F.norm_name("The Goldman Sachs Group, Inc.") == "GOLDMAN SACHS GROUP"
    assert F.norm_name("Smith & Jones L.P.") == "SMITH AND JONES"
    assert F.norm_name("DEUTSCHE BANK AG\\") == "DEUTSCHE BANK"
    assert F.norm_name(None) is None and F.norm_name("Inc.") is None


def test_name_root_only_distinctive_tokens() -> None:
    assert F.name_root("JANE STREET GROUP") == "JANE"
    assert F.name_root("SUSQUEHANNA INTERNATIONAL GROUP") == "SUSQUEHANNA"
    assert F.name_root("FIRST MANHATTAN") is None      # generic first word
    assert F.name_root("IMC CHICAGO") is None          # too short


def _adv(**kw):
    base = {"adv_linked": True, "raum_5d_sum": 100.0}
    base.update(kw)
    return base


def test_classify_adviser_rules() -> None:
    hedge = _adv(raum_f=90.0, private_fund_adviser=True, roster_has_fund_types=True, n_hedge_funds=37)
    assert F.classify_adviser(hedge, use_bulk=True) == ("hedge_fund", "registered_adviser", "adv_roster_fund_counts")
    pe = _adv(raum_f=90.0, private_fund_adviser=True, roster_has_fund_types=True, n_hedge_funds=1, n_pe_funds=5)
    assert F.classify_adviser(pe, use_bulk=True)[:2] == ("other", "private_equity")
    bulk = _adv(raum_f=80.0, private_fund_adviser=True, bulk_hedge_gav=70.0, bulk_pe_gav=10.0, bulk_total_gav=80.0)
    assert F.classify_adviser(bulk, use_bulk=True)[2] == "adv_schedule_d_7b1_gav"
    # PIT (no bulk): no fund types -> PIV share + performance fee
    assert F.classify_adviser({**bulk, "fee_performance": True}, use_bulk=False) == \
        ("hedge_fund", "registered_adviser", "adv_piv_performance_fee")
    ric = _adv(raum_d=55.0, raum_e=5.0, raum_a=40.0)
    assert F.classify_adviser(ric, use_bulk=True)[0] == "mutual_fund"
    bank = _adv(raum_d=90.0, act_bank=True)
    assert F.classify_adviser(bank, use_bulk=True)[0] == "bank_trust"          # 6A bank wins
    bd = _adv(raum_a=60.0, raum_b=10.0, raum_g=30.0, act_broker_dealer=True)
    assert F.classify_adviser(bd, use_bulk=True)[0] == "broker_dealer"
    wealth = _adv(raum_a=40.0, raum_b=30.0, raum_g=30.0)
    assert F.classify_adviser(wealth, use_bulk=True) == ("other", "ria_wealth", "adv_item_5d_mix")
    inst = _adv(raum_g=60.0, raum_i=40.0)
    assert F.classify_adviser(inst, use_bulk=True)[1] == "ria_institutional"
    era = {"adv_linked": True, "era": True, "roster_has_fund_types": True, "n_hedge_funds": 3}
    assert F.classify_adviser(era, use_bulk=True) == ("hedge_fund", "exempt_reporting_adviser", "adv_roster_fund_counts")
    assert F.classify_adviser({}, use_bulk=True) is None


def test_classify_order_and_edgar_rules() -> None:
    assert F.classify({"name_key": "NORGES BANK"})[:2] == ("pension_endowment", "sovereign_central_bank")
    assert F.classify({"name_key": "CALIFORNIA PUBLIC EMPLOYEES RETIREMENT SYSTEM"})[0] == "pension_endowment"
    assert F.classify({"name_key": "PRESIDENT AND FELLOWS OF HARVARD COLLEGE"})[1] == "endowment_foundation"
    assert F.classify({"name_key": "STATE STREET CORP", "sic": "6022"}) == ("bank_trust", "bank", "edgar_sic")
    assert F.classify({"name_key": "MEMBERS TRUST CO"})[0] == "bank_trust"
    assert F.classify({"name_key": "BERKSHIRE HATHAWAY INC", "sic": "6331"})[0] == "insurance"
    assert F.classify({"name_key": "LIFE INSURANCE CO OF X"})[0] == "insurance"
    fam = {"adv_linked": True, "family": True, "raum_5d_sum": 100.0, "raum_d": 70.0, "raum_g": 30.0}
    # a holding company with SIC 6211 whose included managers are RIC advisers is not a broker-dealer
    assert F.classify({"name_key": "BLACKROCK INC", "sic": "6211", "family": fam})[:2] == ("mutual_fund", "ric_adviser")
    assert F.classify({"name_key": "MORGAN STANLEY", "sic": "6211"})[0] == "broker_dealer"
    assert F.classify({"name_key": "JANE STREET GROUP LLC", "x17a5_root": True})[2] == "name_root_x17a5_filer"
    assert F.classify({"name_key": "SOME FUND TRUST", "fund_registrant": True})[0] == "mutual_fund"
    assert F.classify({"name_key": "INVESCO LTD", "sic": "6282"})[1] == "investment_adviser_unlinked"
    assert F.classify({"name_key": "APPLE INC", "sic": "3571"})[1] == "corporate"
    assert F.classify({"name_key": "SUSQUEHANNA INTERNATIONAL GROUP LLP"}) == ("unclassified", "unclassified", "no_evidence")
    # an own ADV link wins over every name / SIC rule
    own = {"adv_linked": True, "raum_5d_sum": 100.0, "raum_d": 80.0, "raum_a": 20.0}
    assert F.classify({"name_key": "X BANK ADVISORS", "sic": "6022", "own": own})[0] == "mutual_fund"


def test_adviser_ev_family_weights_by_raum() -> None:
    rows = [{"roster_date": 1, "roster_kind": "registered", "raum_total": 900.0, "raum_d": 900.0, "act_broker_dealer": False},
            {"roster_date": 1, "roster_kind": "registered", "raum_total": 100.0, "raum_a": 100.0, "act_broker_dealer": True},
            {"roster_date": None}]
    ev = F._adviser_ev(rows, family=True)
    assert ev["raum_5d_sum"] == 1000.0 and ev["raum_d"] == 900.0
    assert abs(ev["bd_raum_share"] - 0.1) < 1e-12 and ev["act_broker_dealer"] is False and ev["era"] is False
    own = F._adviser_ev(rows[1:2], family=False)
    assert own["act_broker_dealer"] is True and own["bd_raum_share"] is None
    assert F._adviser_ev([{"roster_date": None}], family=True) == {}


def test_read_tsv_rows_keeps_named_columns(tmp_path: Path) -> None:
    p = tmp_path / "OTHERMANAGER2.tsv"
    p.write_text("ACCESSION_NUMBER\tSEQUENCENUMBER\tCIK\tFORM13FFILENUMBER\tCRDNUMBER\tSECFILENUMBER\tNAME\n"
                 "0001-25-1\t1\t0000035368\t028-00450\t000108281\t801-7884\tFidelity Management & Research Co LLC\n"
                 "0001-25-1\t2\t\t028-01054\t\t\tFIDELITY MANAGEMENT TRUST CO\n", encoding="utf-8")
    rows = F._read_tsv_rows(p, F.INCLUDED_KEEP)
    assert rows[0]["crd_number"] == "000108281" and rows[0]["sec_file_number"] == "801-7884"
    assert rows[1]["crd_number"] is None and rows[1]["name"] == "FIDELITY MANAGEMENT TRUST CO"
