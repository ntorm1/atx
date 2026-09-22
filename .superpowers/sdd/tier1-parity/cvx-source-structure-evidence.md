# CVX source structure verification - 2026-09-22

Official [SEC index](https://www.sec.gov/Archives/edgar/data/93410/000009341026000019/0000093410-26-000019-index.htm)
records filing and report dates2026-01-30. The [EX-99.1](https://www.sec.gov/Archives/edgar/data/93410/000009341026000019/a12312025ex9918-k.htm)
reports Q42025 and Q42024 diluted EPS1.39 and1.84, with statement quarter
endingDecember31. Submission `report_date` is not this accounting quarter end.

Root fetched the public exhibit as a bounded reference with the dummy project
SEC User-Agent. Decoded UTF-8 reference file (not a production source receipt):
`cvx-q4-2025-official-exhibit.html`,571787bytes,SHA256
`4B2BA83859EEB59A5ED94DCA29A4A22F7C1D5409166B34C0AE4A2C4690494B00`.
This hash identifies the saved decoded reference text, not the original HTTP
response bytes. The original raw body hash was not measured.

The actual statement has multilevel quarter/annual headings, year headings
spanning currency/value/spacer cells, and a per-share section followed by an
issuer-net-income parent, numeric Basic sibling, then Diluted. Comparative
quarter labels also occur in the document. A simplified one-cell-per-year
fixture misses the currency/spacer shape; the source repair must preserve the
qualified quarter's actual numeric leaf and fiscal identity. A compact generic
structural fixture should capture this, without issuer-specific parsing.

No source-loader or warehouse runtime was used for this markup inspection.
