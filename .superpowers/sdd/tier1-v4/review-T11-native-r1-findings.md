# Independent Task11 round1/5: Needs fixes

Reviewer /root/t5_prebuild_review, commit9dfce082. I2-I4 closed: NULL filing clock
preserved, affirmative original/source qualification required, and original/
amendment/notice accessions/public clocks/blocking reasons persisted. Daily cutoff
now rejects previously demonstrated future/mismatched cases.

Important I1 remains: thirteenf_features.py:370-374 requires last_day(session_date)
and only any session within that month. GC1 requires final exchange session.
Pinned-calendar reproduction ending Friday2020-05-29: valid May29stamp/22UTC
rejected; SundayMay31stamp/22UTC accepted. New regression test file:153-155 blesses
Sunday. Derive final session from pinned calendar; require that stamp and its
22UTCcutoff. Fix fixtures to cover weekend and holiday month ends.

Tiny temporary fixtures128MB/one thread only; no broad/actualsource/hash/build/
HTTP jobs, no live review jobs. Full Task11/source/economic/coverage acceptance
NOT_MEASURED. Scoped spec and quality Needs fixes.
