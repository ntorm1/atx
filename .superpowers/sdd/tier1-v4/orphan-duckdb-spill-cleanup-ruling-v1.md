# Exact orphan DuckDB spill cleanup

The user's explicit request covers unnecessary temporary caches. Earlier size
inventories alone did not establish that generic `_tmp/spill-*` paths were safe
to remove, so those paths were retained. New producer/runtime evidence establishes
a narrower, disposable set, subject to independent review before application.

Frozen `alpha_panel/common.py` SHA256
`9411975069075772e123bf570e85420681de830787191cdbcd358c7d9a110172`, lines48-74,
assigns `build_root()/_tmp/spill-{os.getpid()}` as DuckDB's process-private
`temp_directory`. It separately creates a database when requested. The proposed
targets are only the native temporary storage block files in these exact process
directories, not the databases or their WALs. An ended process cannot resume an
in-memory query from these orphan spill blocks. This is a provenance/usage finding,
not an inference that an old filename or a failed run's outputs are disposable.

The new audit fixed14 literal PID directories and found80 flat files totalling
7,451,869,184 logical bytes. Every target matches the native
`duckdb_temp_storage_(DEFAULT|S[0-9]+K)-[0-9]+.tmp` pattern. All14 PIDs were absent.
All80 files were opened simultaneously with read access and only DELETE sharing,
denying concurrent reader/writer access. The inventories remained unchanged while
handles were held. Every member/ancestor was checked for reparses and the resolved
absolute parent was checked against the exact `_tmp` whitelist. The read-only
audit completed0/6s/190,664,704peak bytes under0.4GiB, without a cap hit.

The original71 retained financial members, the10,411,360byte source snapshot,
original producer and pinned interruption inventory were verified before/after.
No frozen source-snapshot path referred to these exact spill directories. The
audit does not hash all original inputs again, and makes no semantic revalidation
or new financial admission claim. It checks cleanup disjointness and the specified
protected byte identities.

Application is a separate reviewed operation. It must bind the unchanged controller,
audit/inventory and exact independent review; reacquire and retain all handles,
repeat PID/path/inventory checks, record a durable start, and delete only the80
literal files without recursion. Empty directories are retained. Source/checkpoint
identities must match afterward. A nonzero guard, partial operation or leftover
file cannot be reported as completed cleanup. Any failed attempt is preserved.

Original source archives, extracted TSVs, Parquet, database/WAL files, accepted
units, failed-attempt ledgers/logs and other session outputs remain outside the
target set. In particular, `_tmp/shortflow/nport/2023q4` stays intact: the original
producer deletes downloaded range objects before finishing the parse, so its
retained TSVs may be the only surviving original-content copy. Earlier policy-
blocked pytest233/234 paths are not targeted or retried.

This ruling changes cleanup eligibility only through new source/runtime evidence.
It does not relax the40GiB floor, memory guard, source-quality requirements, repair
caps or release gates. At this document's creation, application was NOTRUN;
authoritative later application state belongs to its exact guard/result records.

## Independent finding and correction

T17's first Apply review found ImportantI1: FileShare.Delete allowed the held
file's name to be replaced before pathname deletion. V1 Apply was never run.
Its audit and original code remain preserved as metadata, not cleanup approval.

V2 instead acquires delete-capable handles with no read/write/delete sharing,
opens reparse objects without following them, rejects directory/reparse/multiple-
hardlink objects and checks the opened object's final absolute path. It does not
set DELETE_ON_CLOSE while acquiring a handle. Only after the exact successful
audit guard, audit/controller/inventory, independent review, protected identities
and durable Apply start are bound does it mark the exact held object for deletion.
There is no filename lookup in that deletion operation. This uses Windows'
[SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle)
with [FILE_DISPOSITION_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_disposition_info),
preserving the original80-file target set and all exclusion rules.

The fresh read-only v2 audit completed0/6s/190,435,328bytes under0.4GiB,
minimum3.8478GiB free commit; all80 exclusive delete-capable handles were opened
and closed without marking deletion. A separate real Windows fixture compiles
the exact v2 controller adapter and uses only two private9byte files. It proves
rename and concurrent-read refusal while held, unchanged bytes after unmarked
close, exact marked-file deletion after close, and an unchanged unrelated file.
The tiny known fixture and empty directory were then removed without recursion.
This fixture targets no actual spill or original source. V2 Apply remains NOTRUN
at this addendum, pending scoped independent re-review.

Host observerv9 completed301.2s/27,578,368bytes but failed the unchanged original
31sample/300s scheduling condition: minimum free commit2.5589GiB versus3 required,
physical1.6266GiB versus1 required. Its finer guard saw2.3148GiB commit. This is
preserved failed scheduling evidence, not permission to launch financialaudit7.
Lightweight cleanup retains its original external guard; a new qualifying host
window is still required before resuming the held financial work.

## Actual application after approval

Scoped independent v2 re-review PASS/PASS, I1 addressed. The original rejected
v1 verdict is retained. Approved review identity
`022af2590792e99cf5d6faec73ce0899b2283ef116d41f04cebdfed0cc9886f8`
was bound to the exact Apply command. The Windows fixture completed0/3s at
83,329,024peak bytes; the v2 controller remained unchanged from audit through
fixture, review and application.

Applyv2 completed0/6s at190,017,536peak bytes under0.4GiB, no cap hit. All80
identified files were removed through their verified native handles. Empty
directories remain. All71 retained financial members, original source snapshot,
producer and inspection evidence passed the unchanged before/after byte checks.
No source, database/WAL, TSV/Parquet, admitted unit or failure log was removed.

Logical removal was7,451,869,184bytes; observed drive-free increase was
7,451,750,400bytes, from38,554,353,664 to46,006,104,064bytes. With the prior six
cleanups, the seven observed changes sum14,132,310,016bytes. This cumulative
operation observation is not a claim that all concurrent disk changes belong
to this goal. Fresh follow-up free space was45,992,775,680bytes (42.83GiB),
restoring the original40GiB disk condition without changing its threshold.

Evidence: `logs/orphan-duckdb-spill-apply-v2.receipt.json`, immutable start,
`logs/orphan-duckdb-spill-applied-v2.json`, exact auditv2, fixture and independent
review. No cleanup retry is needed or authorized by this completed result.

The held author source fixtures may now retry with fresh tags under their
unchanged external disk/memory guards, serialized T16 then T17. Financialaudit7
still has no new credit: observerv9 failed, so a fresh original300s qualifying
window is required before its newv2 attempt. Creator8 and9..15 remain NOTRUN.
