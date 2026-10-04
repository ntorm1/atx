$ErrorActionPreference='Stop'
if(-not $env:ATX_GUARD_JOB){throw 'External guard required'}
$cacheExpected='C:\Users\natha\AppData\Local\uv\cache'
$cacheResolved=(Resolve-Path -LiteralPath $cacheExpected -ErrorAction Stop).ProviderPath.TrimEnd('\')
if(-not [String]::Equals($cacheResolved,$cacheExpected,[StringComparison]::OrdinalIgnoreCase)){throw 'Cache resolved outside exact whitelist'}
$ancestor=Get-Item -LiteralPath $cacheResolved -Force
while($null -ne $ancestor){
    if($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse cache ancestor'}
    $ancestor=$ancestor.Parent
}
$tag=[IO.File]::ReadAllText((Join-Path $cacheResolved 'CACHEDIR.TAG'))
if(-not $tag.StartsWith('Signature: 8a477f597d28d172789f06886806bc55')){throw 'Unrecognized package cache signature'}
$uvExe='C:\Users\natha\.local\bin\uv.exe'
if((Get-Item -LiteralPath $uvExe).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse executable'}
$resultPath='C:\atx\.superpowers\sdd\tier1-v4\logs\prune-unreachable-uv-cache-v1.json'
$startPath='C:\atx\.superpowers\sdd\tier1-v4\logs\prune-unreachable-uv-cache-v1.start.json'
if((Test-Path -LiteralPath $resultPath) -or (Test-Path -LiteralPath $startPath)){throw 'Preserve prior attempt'}
$record=[ordered]@{
    status='STARTED_NOT_SUCCESS'; cache_path=$cacheResolved
    operation='uv cache prune: unreachable objects only, normal in-use checks'
    authorization='Explicit user request to clean unnecessary artifacts and temporary caches'
    original_sources_targeted=$false; installed_environments_targeted=$false
    force=$false; ci_mode=$false; network_allowed=$false
    command=@($uvExe,'--offline','--no-config','--no-progress','cache','prune','--cache-dir',$cacheResolved)
    uv_executable_sha256=(Get-FileHash -LiteralPath $uvExe -Algorithm SHA256).Hash.ToLowerInvariant()
    started_at_utc=[DateTime]::UtcNow.ToString('o')
    free_before_bytes=(Get-PSDrive -Name C).Free
}
[IO.File]::WriteAllText($startPath,($record | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
# The package manager owns reachability and in-use validation. No hand deletion,
# --force, --ci, interpreter cleanup, env removal, install or network operation.
$process=Start-Process -FilePath $uvExe -ArgumentList @('--offline','--no-config','--no-progress','cache','prune','--cache-dir',$cacheResolved) -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput 'C:\atx\.superpowers\sdd\tier1-v4\logs\prune-unreachable-uv-cache-v1.uv.out.log' -RedirectStandardError 'C:\atx\.superpowers\sdd\tier1-v4\logs\prune-unreachable-uv-cache-v1.uv.err.log'
$record.returncode=$process.ExitCode
$record.status=if($process.ExitCode -eq 0){'COMPLETED_PACKAGE_MANAGER_PRUNE'}else{'FAILED_PRESERVE_LOGS'}
$record.finished_at_utc=[DateTime]::UtcNow.ToString('o')
$record.free_after_bytes=(Get-PSDrive -Name C).Free
$record.observed_free_change_bytes=$record.free_after_bytes-$record.free_before_bytes
$record.qualification='Observed free-space change may include concurrent disk activity; exact pruned object count/size is the package-manager log. This does not change source or release gates.'
[IO.File]::WriteAllText($resultPath,($record | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
$record | ConvertTo-Json -Depth 6
exit $process.ExitCode
