# Orphan DuckDB spill cleanup independent review

Spec Needs fixes; quality Needs fixes for Apply. Audit evidence is valid metadata
evidence only. Apply remains NOTRUN; no cleanup/source hash/job was performed by
this reviewer.

## Important I1 — delete-sharing does not bind pathname deletion to held object

`orphan-duckdb-spill-cleanup-v1.ps1:76` opens each file with FileShare.Delete.
This denies simultaneous readers/writers of that file object, but deliberately
permits another handle to rename/unlink it. `:112` subsequently calls Remove-Item
by pathname. A namespace replacement between the inventory/reparse check and
Remove-Item therefore targets the replacement, not necessarily the audited held
file. PID absence checks do not make pathname lookup atomic, and held streams do
not prevent rename/unlink under this sharing mode. The comment at75 claiming
unlink of the same locked object without a race is consequently too strong.

Bind deletion to the already audited file object: acquire appropriate native
delete-capable handles without delete-sharing after approval/preconditions, then
mark those exact handles for deletion only after the complete inventory/start
record/protected checks pass. Preserve original failed/audit artifacts, all-owner
absence/reparse/stat checks, no recursive operation, and before/after source
verification. Do not introduce DeleteOnClose during unapproved auditing or before
all Apply acceptance checks, because error cleanup could otherwise delete files
before the authorized Apply boundary. A handle-bound deletion also avoids deleting
an unrelated replacement after a directory/path namespace race.

## Checked boundaries

Original common.connect48–74 creates these per-PID paths only as DuckDB
temp_directory, separately from scratch database/WAL paths. The audited80 flat
files in14 literal PID directories match the native spill-name allowlist and
7,451,869,184 logical bytes (<8GiB). All14 owners absent and all80 simultaneous
reader/writer-denying handles were actually acquired in the preserved Audit run.
Audit guard completed0/6s/190,664,704B under.4GiB, no cap/stop. Producer hash,
independently pinned v5 inspection, all71 native members and physical frozen
snapshot are protected before/after; snapshot spill references refuse. Native
admissions, DB/WAL, TSV/Parquet/source/failure logs and directories are not targets.
Apply checks unchanged audit/controller/inventory and exact review identity,
rejects previous start/result, records Apply start before mutations and refuses
failed postconditions. Successful cleanup cannot grant any source/financial gate.

Covering commands: full controller Get-Content (exit0,1.46s); original producer
connect snippet plus Audit/guard metadata and named deletion/share call sites
(exit0,1.95s). Static review only; no Apply, filesystem repro, actual source
inspection/hash scan, fixture, network or index action. Required successful
external guard outcome and root's new scheduling window remain separate.

## Scoped v2 I1 re-review

Spec PASS; quality PASS for the v2 correction and exact audited cleanup scope.
I1 ADDRESSED. The historical v1 Apply verdict remains Needs fixes and its
artifacts are preserved. Apply v2 remains NOTRUN at review time.

The new adapter opens each exact existing file with GENERIC_READ | DELETE,
share mode zero and OPEN_REPARSE_POINT, verifies opened attributes, one hard
link and exact final handle path, and retains all80 handles through acceptance.
There is no DELETE_ON_CLOSE. Audit/error disposal therefore does not request
removal. Apply records its start only after audit/code/inventory/review/protected
checks, then uses FileDispositionInfo on the held SafeFileHandle; it performs
no subsequent pathname deletion. This resolves the identified replacement race
and preserves the original bounded nonrecursive target and protected-byte rules.

Actual Audit v2 and the source-free Windows fixture bind the same controller
SHA950bc812a3298293369fb5cc603e352361af1922c9596104bb7df97059f566c9.
Audit acquired80 exclusive delete-capable handles, completed0/6s/190435328B
under.4GiB, no cap/stop/test overrides. The fixture compiled the exact adapter
extracted from that controller. It demonstrated held-object rename and second
reader refusal, unmarked close preserving bytes, marked close deleting only the
audited object and the peer remaining unchanged. It completed0/3s/83329024B
under.3GiB, no cap/test overrides; actual spill/source files were not targeted.

Covering commands: Get-Content v2 controller, audit/receipt and existing review
(exit0,1.86s); Get-Content exact adapter fixture and its result/receipt
(exit0,0.89s). No reviewer execution, source scan/hash, fixture rerun, Apply or
index operation. The current exact successful audit receipt was inspected;
this verdict does not approve arbitrary replacement receipts. Cleanup success
cannot grant source/product gates or supersede the independently required fresh
heavy-job scheduling window. Observer v9 scheduling failure remains binding.
