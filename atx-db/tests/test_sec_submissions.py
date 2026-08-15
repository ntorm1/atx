from __future__ import annotations

from atx_db.sec_submissions import _targets


def test_submission_targets_support_legacy_cik_after_ticker_successor_change(tmp_store) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO sec_company_tickers (cik,ticker,title,security_id)
        VALUES ('0002115436','XOM','EXXON MOBIL CORPORATION','SEC-CIK-0002115436')
        """
    )

    targets = _targets(tmp_store, ("XOM",), ("34088",))

    assert targets == [
        ("CIK-0000034088", "0000034088", "SEC-CIK-0000034088"),
        ("XOM", "0002115436", "SEC-CIK-0002115436"),
    ]
