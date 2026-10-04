# R24: Preserve the original reproduced TNIC-style method and strict peer gate

Task15's binding gap-analysis item13 references the existing classification_tnic
producer and prior task-TXT report. Current ALPHA_PANEL_TEXT.md lines89-103 and
the original report lines85-87 distinguish strict tnic3 peers from nearest-five
fallback. The original tnic.py method is explicitly a documented Hoberg-Phillips
style reproduction, not an official external-data import: binary word cosine,
prior-year document-frequency filtering, and a prior-year SIC3-calibrated cutoff.
Its known limitations include simplified vocabulary and a future SIC snapshot.

Ruling: retain that required reproduced method, its original numeric constants,
strict comparison rule and full linked-filer yearly denominator. The original
strict peer coverage gate can be measured on independently source-qualified
reproduced tnic3 pairs; official external TNIC data is not a newly imposed
mandatory dependency. Neither a positive-cosine all-word method nor nearest-five
fallback satisfies that gate. Identify the source/method plainly as reproduced
TNIC-style and never claim that it equals the authors' official database.

Before qualification, repair the actual PIT defects rather than copying the
legacy assertions: every calibration document, vocabulary/filter input and SIC
observation must have qualified original availability, and every result must
respect the maximum clock of all its dependencies. A 2026 SIC snapshot cannot
qualify historical calibration. Latest-filing-in-year selection must not erase
or revise a previously visible source vintage. Preserve missing calibration,
historical SIC, missing original sections and unproved clocks with explicit
reasons and full coverage exclusions. Do not invent an early clock merely because
a record is labeled with the previous year. Full source replay and corpus
closure remain necessary for the required native implementation.

The controller's earlier request to make an official external adapter mandatory
is superseded by this source-based clarification. An optional official adapter
may be retained only if already useful and independently qualified; it must not
displace or delay the required reproduced method. If present, preserve its
different score/identifier/year meanings and publication clocks. Primary public
documentation notes in tnic-primary-documentation-v1.json are not data admission,
historical availability or permission to substitute an external source.

Cost if wrong: the intended TNIC-style approximation remains less comparable to
the academic source than an exact reproduction, and qualified historical SIC
and complete corpus clocks may reduce coverage. Those limitations remain visible;
the original95%/90% gates, complete populations, actual source qualification and
release requirements are unchanged. No return measurements or source acquisitions
have occurred in this initial Task15 implementation, and no gate is promoted.
