"""Pure rules of the alpha panel security master (U3) and vendor corporate-action events (D10)."""

from atx_db.alpha_panel.corporate_actions import classify, split_like
from atx_db.alpha_panel.security_master import classify_finra, share_class_of


def test_vendor_factor_classes() -> None:
    assert classify(0.25, "none") == "split"            # AAPL 2020-08-31 4:1
    assert classify(1 / 1.5, "none") == "split"         # 3:2
    assert classify(10.0, "none") == "reverse_split"    # 1:10
    assert classify(0.9982, "none") == "cash_distribution"  # AAPL 2020-08-07 dividend
    assert classify(0.7, "none") == "large_distribution"    # spin-off or special dividend, not split-like
    assert classify(0.25, "repaired") == "vendor_factor_break"
    assert classify(None, "none") is None


def test_split_like_ratios() -> None:
    assert split_like(2.0) and split_like(3.0) and split_like(4 / 3) and split_like(20.0)
    assert not split_like(1.43) and not split_like(None)


def test_finra_names_resolve_truncated_derivative_lines() -> None:
    assert classify_finra("Armada Acquisition Corp. III W", "AACIW") == "warrant"
    assert classify_finra("Abony Acquisition Corp. I Unit", "AACOU") == "unit"
    assert classify_finra("Apple Inc. Common Stock", "AAPL") == "common"
    assert classify_finra("SPDR S&P 500 ETF Trust", "SPY") == "fund"
    assert classify_finra("BP p.l.c. American Depositary", "BP") == "ADR"
    assert classify_finra(None, "XYZ") == "unknown"


def test_share_class_word() -> None:
    assert share_class_of("Alphabet Inc. Class C Capital S") == "C"
    assert share_class_of("Paramount Global Class B Commo") == "B"
    assert share_class_of("Apple Inc. Common Stock") is None
    assert share_class_of(None) is None


def test_etp_and_spac_names_without_vendor_flags() -> None:
    assert classify_finra("VelocityShares 3x Long Crude", "UWT") == "ETF"
    assert classify_finra("ProShares Ultra Bloomberg Natu", "BOIL") == "ETF"
    assert classify_finra("Direxion Daily Gold Miners Ind", "NUGT") == "ETF"
    assert classify_finra("iPath S&P 500 VIX Short-Term F", "VXX") == "ETF"
    assert classify_finra("Pershing Square Tontine Holdin", "PSTH") == "spac"
    assert classify_finra("Ares Acquisition Corporation", "AAC") == "spac"
    assert classify_finra("NVIDIA Corporation", "NVDA") == "common_unverified"
    assert classify_finra("United States Steel Corporatio", "X") == "common_unverified"


def test_truncated_and_suffix_evidence() -> None:
    assert classify_finra("BHP Group Plc American Deposit", "BBL") == "ADR"
    assert classify_finra("Unilever NV New York Registry", "UN") == "ADR"
    assert classify_finra("Sempra Energy 6% Mandatory Con", "SRE.PRA") == "preferred"
    assert classify_finra("Broadcom Inc. 8.00% Mandatory", "AVGOP") == "preferred"
    assert classify_finra("Luminar Technologies, Inc. War", "LAZRW") == "warrant"
    assert classify_finra("QuantumScape Corporation Warra", "QS.WS") == "warrant"
    assert classify_finra("Social Capital Hedosophia Hold", "IPOC.U") == "unit"
    assert classify_finra("Burgundy Technology Acquisitio", "BTAQ") == "spac"
    assert classify_finra("VelocityShares3x Long Crude Oi", "UWT") == "ETF"
    assert classify_finra("Invesco BulletShares 2020 Corp", "BSCK") == "ETF"
    assert classify_finra("Microsoft Corporation Common S", "MSFT") == "common_unverified"
    assert classify_finra("Berkshire Hathaway Inc. Class", "BRK.B") == "common_unverified"
