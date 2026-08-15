#Requires -Modules Pester

$scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'lease-worktree.ps1'

# Import pure safety helpers. The script has an explicit dot-source guard.
. $scriptPath

Describe 'lease-worktree atomic ownership' {
  It 'allows exactly one of two concurrent processes to acquire one slot' {
    $poolRoot = Join-Path $TestDrive 'double-acquire'
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-1') -Force | Out-Null

    $jobBody = {
      param($Path, $Root, $Run)
      try {
        $output = & $Path -Branch 'test/atomic' -RunId $Run -Agent $Run `
          -MaxPool 1 -TestPoolRoot $Root -TestLeaseOnly -TestSelectionDelayMs 150
        [pscustomobject]@{ ok = $true; run_id = $Run; output = ($output -join "`n") }
      } catch {
        [pscustomobject]@{ ok = $false; run_id = $Run; output = $_.Exception.Message }
      }
    }

    $jobs = @(
      Start-Job -ScriptBlock $jobBody -ArgumentList $scriptPath, $poolRoot, 'run-a'
      Start-Job -ScriptBlock $jobBody -ArgumentList $scriptPath, $poolRoot, 'run-b'
    )
    try {
      $jobs | Wait-Job | Out-Null
      $results = @($jobs | Receive-Job)
    } finally {
      $jobs | Remove-Job -Force -ErrorAction SilentlyContinue
    }

    @($results | Where-Object { $_.ok }).Count | Should Be 1
    @($results | Where-Object { -not $_.ok }).Count | Should Be 1
    ($results | Where-Object { -not $_.ok }).output | Should Match 'pool exhausted|atomic lease claim lost'

    $record = Read-LeaseRecord (Join-Path $poolRoot 'pool-1\.atx-lease')
    $record.version | Should Be '2'
    @('run-a', 'run-b') -contains $record.run_id | Should Be $true
    [int]$record.owner_pid -gt 0 | Should Be $true
    [string]::IsNullOrWhiteSpace($record.owner_process_started_utc) | Should Be $false
    $record.branch | Should Be 'test/atomic'
    [string]::IsNullOrWhiteSpace($record.acquired_utc) | Should Be $false
  }

  It 'fills the first missing numeric pool slot instead of using directory count plus one' {
    $poolRoot = Join-Path $TestDrive 'numeric-gap'
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-3') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-4') -Force | Out-Null
    Set-Content -Path (Join-Path $poolRoot 'pool-3\.atx-lease') -Value 'legacy=occupied'
    Set-Content -Path (Join-Path $poolRoot 'pool-4\.atx-lease') -Value 'legacy=occupied'

    $output = & $scriptPath -Branch 'test/gap' -RunId 'gap-run' -Agent 'pester' `
      -MaxPool 4 -TestPoolRoot $poolRoot -TestLeaseOnly

    ($output -join "`n") | Should Match 'pool=pool-1'
    Test-Path (Join-Path $poolRoot 'pool-1\.atx-lease') | Should Be $true
  }
}

Describe 'lease-worktree owner-death and release guards' {
  It 'uses PID plus process-start timestamp to distinguish a live owner from PID reuse' {
    $record = New-LeaseFields 'owner-run' 'pester' 'test/owner' 'main' ('0' * 40)
    Get-OwnerState $record | Should Be 'alive'

    $record.owner_process_started_utc = '2000-01-01T00:00:00.0000000Z'
    Get-OwnerState $record | Should Be 'dead'
  }

  It 'refuses another run stale recovery while the recorded owner identity is alive' {
    $poolRoot = Join-Path $TestDrive 'live-owner'
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-1') -Force | Out-Null
    & $scriptPath -Branch 'test/live' -RunId 'live-run' -Agent 'pester' `
      -MaxPool 1 -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null

    {
      & $scriptPath -Release 'pool-1' -RunId 'intruder-run' -RecoverStale `
        -TestPoolRoot $poolRoot -TestLeaseOnly
    } | Should Throw 'refusing stale recovery'

    & $scriptPath -Release 'pool-1' -RunId 'live-run' `
      -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
    Test-Path (Join-Path $poolRoot 'pool-1\.atx-lease') | Should Be $false
  }

  It 'permits explicit stale recovery when PID start identity no longer matches' {
    $poolRoot = Join-Path $TestDrive 'dead-owner'
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-1') -Force | Out-Null
    & $scriptPath -Branch 'test/dead' -RunId 'dead-run' -Agent 'pester' `
      -MaxPool 1 -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null

    $leasePath = Join-Path $poolRoot 'pool-1\.atx-lease'
    $lines = Get-Content $leasePath | ForEach-Object {
      if ($_ -match '^owner_process_started_utc=') {
        'owner_process_started_utc=2000-01-01T00:00:00.0000000Z'
      } else { $_ }
    }
    Set-Content -Path $leasePath -Encoding Ascii -Value $lines

    & $scriptPath -Release 'pool-1' -RunId 'recovery-run' -RecoverStale `
      -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
    Test-Path $leasePath | Should Be $false
  }
}
