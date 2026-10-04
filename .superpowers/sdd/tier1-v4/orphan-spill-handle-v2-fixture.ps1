$ErrorActionPreference='Stop'
if(-not $env:ATX_GUARD_JOB){throw 'External guard required'}
$base='C:\atx\.superpowers\sdd\tier1-v4'
$out=Join-Path $base 'logs\orphan-spill-handle-v2-fixture.json'
if(Test-Path -LiteralPath $out){throw 'Preserve prior fixture evidence'}
$controller=Join-Path $base 'orphan-duckdb-spill-cleanup-v2.ps1'
$source=[IO.File]::ReadAllText($controller)
$match=[regex]::Match($source,"(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@")
if(-not $match.Success -or [regex]::Matches($source,'Add-Type -TypeDefinition').Count -ne 1){throw 'Exact single native adapter required'}
# Compile the exact controller adapter; do not execute its Audit/Apply body.
Add-Type -TypeDefinition $match.Groups[1].Value
$scratch=Join-Path $base ('scratch\orphan-spill-handle-v2-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$expectedPrefix=[IO.Path]::GetFullPath((Join-Path $base 'scratch'))+'\'
if(-not [IO.Path]::GetFullPath($scratch).StartsWith($expectedPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture escaped scratch'}
$a=Join-Path $scratch 'audited.tmp'
$b=Join-Path $scratch 'untouched.tmp'
$replacement=Join-Path $scratch 'renamed.tmp'
[IO.File]::WriteAllText($a,'fixture-a',[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($b,'fixture-b',[Text.UTF8Encoding]::new($false))
$originalB=(Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash
$handle=$null
try{
    $handle=[OrphanSpillHandleV2]::Open($a)
    $renameRefused=$false
    try{[IO.File]::Move($a,$replacement)}catch [IO.IOException]{$renameRefused=$true}
    if(-not $renameRefused -or -not (Test-Path -LiteralPath $a) -or (Test-Path -LiteralPath $replacement)){throw 'Held object permitted rename/replacement'}
    $concurrentReadRefused=$false
    try{$other=[IO.File]::OpenRead($a);$other.Dispose()}catch [IO.IOException]{$concurrentReadRefused=$true}
    if(-not $concurrentReadRefused){throw 'Held object permitted another reader'}
    $handle.Dispose();$handle=$null
    if([IO.File]::ReadAllText($a) -cne 'fixture-a'){throw 'Unmarked disposal deleted/changed audit object'}
    $handle=[OrphanSpillHandleV2]::Open($a)
    [OrphanSpillHandleV2]::MarkDelete($handle)
    $handle.Dispose();$handle=$null
    if(Test-Path -LiteralPath $a){throw 'Exact marked object not deleted on close'}
    if((Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash -cne $originalB){throw 'Unrelated file changed'}
    $record=[ordered]@{status='NATIVE_HANDLE_RENAME_READ_REFUSAL_AND_EXACT_DELETE_PASS';controller_sha256=(Get-FileHash -LiteralPath $controller -Algorithm SHA256).Hash.ToLowerInvariant();rename_refused=$renameRefused;concurrent_read_refused=$concurrentReadRefused;unmarked_close_preserved=$true;marked_object_deleted=$true;unrelated_object_preserved=$true;actual_spill_or_source_files_targeted=$false;fixture_directory=$scratch}
    [IO.File]::WriteAllText($out,($record | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    $record | ConvertTo-Json -Compress
}finally{
    if($null -ne $handle){$handle.Dispose()}
}
# Only the exact known disposable fixture remains; no recursive deletion.
if((Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash -cne $originalB){throw 'Do not clean changed fixture'}
Remove-Item -LiteralPath $b
if(@(Get-ChildItem -LiteralPath $scratch -Force).Count -ne 0){throw 'Preserve unexpected fixture content'}
Remove-Item -LiteralPath $scratch
