"""Deterministic mock universe shared by the adapter mock generators and the contract tests.

The universe has the identity situations a real delivery meets: a ticker change (line 0, FB -> META style), a CUSIP
change after a reorganisation (line 1), a delisting (line 2), ticker reuse by a later line (lines 3 and 4 share
``REUS``), a second share class of one issuer (lines 5 and 6 share a CIK), plus a CUSIP and a ticker that no history
knows. CUSIPs carry valid check digits. ``resolver()`` returns the matching ``IdentityResolver``.
"""

from __future__ import annotations

import datetime as dt
import random
import string
from dataclasses import dataclass

from .identity import IdentityResolver

START = dt.date(2019, 1, 2)
END = dt.date(2026, 6, 30)
UNKNOWN_CUSIP = "99999Z107"
UNKNOWN_TICKER = "ZZZQ"


def cusip_check_digit(base8: str) -> str:
    """Standard CUSIP modulus-10 double-add-double check digit."""
    total = 0
    for i, ch in enumerate(base8.upper()):
        v = int(ch) if ch.isdigit() else (ord(ch) - 55 if ch.isalpha() else {"*": 36, "@": 37, "#": 38}[ch])
        if i % 2 == 1:
            v *= 2
        total += v // 10 + v % 10
    return str((10 - total % 10) % 10)


def make_cusip(issuer6: str, issue2: str = "10") -> str:
    base = (issuer6 + issue2).upper()
    return base + cusip_check_digit(base)


def make_isin(cusip9: str, country: str = "US") -> str:
    """ISIN = country + CUSIP + Luhn check digit over the base-36 digit expansion (US0378331005 for Apple)."""
    s = (country + cusip9).upper()
    digits = "".join(str(int(c, 36)) for c in s)
    total = 0
    for i, ch in enumerate(reversed(digits)):
        v = int(ch) * (2 if i % 2 == 0 else 1)
        total += v // 10 + v % 10
    return s + str((10 - total % 10) % 10)


@dataclass(frozen=True)
class MockLine:
    security_id: int
    cik: int
    name: str
    cusips: tuple[tuple[str, dt.date, dt.date], ...]
    tickers: tuple[tuple[str, dt.date, dt.date], ...]
    first: dt.date
    last: dt.date

    def cusip_on(self, d: dt.date) -> str | None:
        return next((c for c, a, b in self.cusips if a <= d <= b), None)

    def ticker_on(self, d: dt.date) -> str | None:
        return next((t for t, a, b in self.tickers if a <= d <= b), None)

    def alive(self, d: dt.date) -> bool:
        return self.first <= d <= self.last


class MockUniverse:
    def __init__(self, n_lines: int = 16, seed: int = 11) -> None:
        rng = random.Random(seed)
        used: set[str] = {"REUS", UNKNOWN_TICKER}
        lines: list[MockLine] = []
        for i in range(n_lines):
            while True:
                tic = "".join(rng.choice(string.ascii_uppercase) for _ in range(rng.choice((3, 4))))
                if tic not in used:
                    used.add(tic)
                    break
            issuer = f"{rng.randrange(10**5):05d}{rng.choice(string.ascii_uppercase)}"
            sid, cik = 101 + 17 * i, 700001 + 31 * i
            cus = make_cusip(issuer)
            cusips = ((cus, START, END),)
            tickers = ((tic, START, END),)
            first, last = START, END
            if i == 0:  # ticker change
                tickers = ((tic, START, dt.date(2021, 6, 8)), (tic + "X", dt.date(2021, 6, 9), END))
            elif i == 1:  # CUSIP change after a reorganisation
                cusips = ((cus, START, dt.date(2022, 2, 28)), (make_cusip(issuer, "20"), dt.date(2022, 3, 1), END))
            elif i == 2:  # delisted
                last = dt.date(2022, 6, 30)
            elif i == 3:  # ticker reused later by line 4
                last = dt.date(2020, 12, 31)
                tickers = (("REUS", START, last),)
            elif i == 4:
                first = dt.date(2022, 1, 3)
                tickers = (("REUS", first, END),)
            elif i == 6:  # second share class of line 5's issuer
                cik = lines[5].cik
                cusips = ((lines[5].cusips[0][0][:6] + "20" + cusip_check_digit(lines[5].cusips[0][0][:6] + "20"),
                           START, END),)
            cusips = tuple((c, max(a, first), min(b, last)) for c, a, b in cusips)
            tickers = tuple((t, max(a, first), min(b, last)) for t, a, b in tickers)
            lines.append(MockLine(sid, cik, f"MOCK CORP {i}", cusips, tickers, first, last))
        self.lines = lines
        self.rng = random.Random(seed + 1)

    def resolver(self) -> IdentityResolver:
        cus = [(c, ln.security_id, a, b) for ln in self.lines for c, a, b in ln.cusips]
        tic = [(t, ln.security_id, a, b) for ln in self.lines for t, a, b in ln.tickers]
        links = [(ln.security_id, ln.cik, ln.first, ln.last, "strict") for ln in self.lines]
        return IdentityResolver.from_rows(cus, tic, links)

    @staticmethod
    def weekdays(start: dt.date, end: dt.date, step: int = 1) -> list[dt.date]:
        out, d = [], start
        while d <= end:
            if d.weekday() < 5:
                out.append(d)
            d += dt.timedelta(days=1)
        return out[::step]

    @staticmethod
    def third_thursday(y: int, m: int) -> dt.date:
        """I/B/E/S monthly STATPERS: the Thursday before the third Friday."""
        d = dt.date(y, m, 1)
        fridays = [d + dt.timedelta(days=k) for k in range(31)
                   if (d + dt.timedelta(days=k)).month == m and (d + dt.timedelta(days=k)).weekday() == 4]
        return fridays[2] - dt.timedelta(days=1)
