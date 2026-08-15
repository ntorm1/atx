# Worktree pool lease manager. Windows PowerShell 5.1 compatible; ASCII only.
#
# Lease ownership is a hard contract. Acquisition uses FileMode.CreateNew for an
# atomic marker and a short, crash-recoverable pool-selection mutex. Release
# requires the run_id that acquired the lease. A different run may release only
# with -RecoverStale after PID + process-start identity proves the local owner is
# gone. No lease is reclaimed automatically.

param(
  [string]$Branch,
  [string]$Base = 'main',
  [string]$Agent = $env:USERNAME,
  [string]$RunId,
  [switch]$Shared,
  [string]$Release,
  [switch]$RecoverStale,
  [switch]$Status,
  [int]$MaxPool = 20,
  [string]$TestPoolRoot,
  [switch]$TestLeaseOnly,
  [int]$TestSelectionDelayMs = 0
)
$ErrorActionPreference = 'Stop'

function Get-UtcStamp([datetime]$Value) {
  return $Value.ToUniversalTime().ToString('o', [Globalization.CultureInfo]::InvariantCulture)
}

function Get-CurrentProcessStartUtc {
  return Get-UtcStamp (Get-Process -Id $PID -ErrorAction Stop).StartTime
}

function Assert-RecordValue([string]$Name, [string]$Value) {
  if ([string]::IsNullOrWhiteSpace($Value)) { throw ($Name + ' must not be empty') }
  if ($Value -match "[`r`n=]") { throw ($Name + ' contains an invalid character') }
}

function New-LeaseFields(
  [string]$LeaseRunId,
  [string]$LeaseAgent,
  [string]$LeaseBranch,
  [string]$BaseRef,
  [string]$BaseSha
) {
  Assert-RecordValue 'RunId' $LeaseRunId
  Assert-RecordValue 'Agent' $LeaseAgent
  Assert-RecordValue 'Branch' $LeaseBranch
  Assert-RecordValue 'Base' $BaseRef
  Assert-RecordValue 'BaseSha' $BaseSha
  return [ordered]@{
    version = '2'
    lease_token = [guid]::NewGuid().ToString('N')
    run_id = $LeaseRunId
    agent = $LeaseAgent
    owner_host = [Environment]::MachineName
    owner_pid = [string]$PID
    owner_process_started_utc = Get-CurrentProcessStartUtc
    branch = $LeaseBranch
    base_ref = $BaseRef
    base_sha = $BaseSha
    acquired_utc = Get-UtcStamp (Get-Date)
  }
}

function ConvertTo-RecordLines($Fields) {
  $lines = New-Object System.Collections.Generic.List[string]
  foreach ($entry in $Fields.GetEnumerator()) {
    Assert-RecordValue ([string]$entry.Key) ([string]$entry.Value)
    $lines.Add(([string]$entry.Key + '=' + [string]$entry.Value))
  }
  return $lines.ToArray()
}

function Try-NewAtomicRecord([string]$Path, $Fields) {
  $stream = $null
  $writer = $null
  try {
    $stream = New-Object System.IO.FileStream(
      $Path,
      [System.IO.FileMode]::CreateNew,
      [System.IO.FileAccess]::Write,
      [System.IO.FileShare]::Read
    )
    $writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.ASCIIEncoding))
    foreach ($line in (ConvertTo-RecordLines $Fields)) { $writer.WriteLine($line) }
    $writer.Flush()
    $stream.Flush()
    return $true
  } catch [System.IO.IOException] {
    return $false
  } finally {
    if ($writer) { $writer.Dispose() }
    elseif ($stream) { $stream.Dispose() }
  }
}

function Read-LeaseRecord([string]$Path) {
  if (-not [System.IO.File]::Exists($Path)) { return $null }
  $record = @{}
  foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
    $split = $line.IndexOf('=')
    if ($split -gt 0) {
      $record[$line.Substring(0, $split)] = $line.Substring($split + 1)
    }
  }
  return $record
}

function Get-OwnerState($Record) {
  if (-not $Record -or -not $Record.owner_host -or -not $Record.owner_pid -or
      -not $Record.owner_process_started_utc) { return 'unknown' }
  if ($Record.owner_host -ne [Environment]::MachineName) { return 'foreign' }
  $ownerPid = 0
  if (-not [int]::TryParse([string]$Record.owner_pid, [ref]$ownerPid)) { return 'unknown' }
  try {
    $process = Get-Process -Id $ownerPid -ErrorAction Stop
    if ((Get-UtcStamp $process.StartTime) -eq [string]$Record.owner_process_started_utc) {
      return 'alive'
    }
    # PID exists but belongs to a later process: the recorded owner is dead.
    return 'dead'
  } catch {
    return 'dead'
  }
}

function Remove-RecordByRename([string]$Path, [string]$ExpectedToken, [string]$Reason) {
  $record = Read-LeaseRecord $Path
  if (-not $record -or $record.lease_token -ne $ExpectedToken) {
    throw ('lease identity changed before ' + $Reason + ': ' + $Path)
  }
  $tombstone = $Path + '.' + $Reason + '.' + $ExpectedToken
  [System.IO.File]::Move($Path, $tombstone)
  [System.IO.File]::Delete($tombstone)
}

function Get-PoolTrees([string]$PoolRoot) {
  return @(Get-ChildItem $PoolRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^pool-([0-9]+)$' } |
    Sort-Object { [int]($_.Name.Substring(5)) })
}

function Get-LeasePath([string]$Worktree) { return Join-Path $Worktree '.atx-lease' }

function Enter-SelectionMutex([string]$PoolRoot, [string]$MutexRunId) {
  $path = Join-Path $PoolRoot '.atx-pool-acquire.lock'
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    $fields = New-LeaseFields $MutexRunId 'pool-selector' 'POOL_SELECTION' 'none' ('0' * 40)
    if (Try-NewAtomicRecord $path $fields) {
      return [pscustomobject]@{ Path = $path; Token = $fields.lease_token }
    }
    $existing = Read-LeaseRecord $path
    if ((Get-OwnerState $existing) -eq 'dead' -and $existing.lease_token) {
      try { Remove-RecordByRename $path $existing.lease_token 'stale' } catch { }
    } else {
      Start-Sleep -Milliseconds 50
    }
  }
  throw ('timed out waiting for atomic pool-selection lock: ' + $path)
}

function Exit-SelectionMutex($Mutex) {
  if ($Mutex) { Remove-RecordByRename $Mutex.Path $Mutex.Token 'released' }
}

function Find-FreePool([string]$PoolRoot) {
  foreach ($tree in (Get-PoolTrees $PoolRoot)) {
    if (-not [System.IO.File]::Exists((Get-LeasePath $tree.FullName))) { return $tree }
  }
  return $null
}

function Find-MissingPoolNumber([string]$PoolRoot, [int]$PoolLimit) {
  $used = @{}
  foreach ($tree in (Get-PoolTrees $PoolRoot)) { $used[[int]$tree.Name.Substring(5)] = $true }
  for ($n = 1; $n -le $PoolLimit; $n++) {
    if (-not $used.ContainsKey($n)) { return $n }
  }
  return 0
}

# Pester imports the safety helpers without running command mode.
if ($MyInvocation.InvocationName -eq '.') { return }

if ($MaxPool -lt 1) { throw 'MaxPool must be at least 1' }
if ($TestLeaseOnly -and -not $TestPoolRoot) { throw 'TestLeaseOnly requires TestPoolRoot' }
if ($TestPoolRoot -and -not $TestLeaseOnly) { throw 'TestPoolRoot is valid only with TestLeaseOnly' }
if ($TestSelectionDelayMs -gt 0 -and -not $TestLeaseOnly) { throw 'TestSelectionDelayMs is test-only' }
if ($RecoverStale -and -not $Release) { throw 'RecoverStale is valid only with Release' }

$repoRoot = $null
if ($TestPoolRoot) {
  $wtRoot = [System.IO.Path]::GetFullPath($TestPoolRoot)
} else {
  $repoRoot = (git rev-parse --show-toplevel).Trim()
  if ($LASTEXITCODE -ne 0 -or -not $repoRoot) { throw 'not inside a git repository' }
  # Linked worktrees live under atx-wt, so deriving from the current top-level
  # would incorrectly produce atx-wt\atx-wt. The common git dir always points
  # back to the primary checkout's .git directory.
  $gitCommonDir = (git rev-parse --path-format=absolute --git-common-dir).Trim()
  if ($LASTEXITCODE -ne 0 -or -not $gitCommonDir) { throw 'cannot resolve git common directory' }
  $primaryRoot = Split-Path $gitCommonDir -Parent
  $wtRoot = Join-Path (Split-Path $primaryRoot -Parent) 'atx-wt'
}
New-Item -ItemType Directory -Force $wtRoot | Out-Null

if ($Status) {
  $trees = @(Get-PoolTrees $wtRoot)
  if ($trees.Count -eq 0) {
    Write-Host ('pool empty (' + $wtRoot + '); lease one with -Branch <name> -RunId <id>')
    return
  }
  foreach ($tree in $trees) {
    $leasePath = Get-LeasePath $tree.FullName
    $record = Read-LeaseRecord $leasePath
    if ($record -and $record.version -eq '2') {
      $state = 'LEASED run_id=' + $record.run_id + ' | agent=' + $record.agent +
        ' | pid=' + $record.owner_pid + ' | owner=' + (Get-OwnerState $record) +
        ' | branch=' + $record.branch + ' | acquired=' + $record.acquired_utc
    } elseif ($record) {
      $state = 'LEASED LEGACY (no run_id/PID guard) | agent=' + $record.agent +
        ' | branch=' + $record.branch + ' | since=' + $record.since
    } else {
      $state = 'free'
    }
    if ($TestLeaseOnly) {
      $branchName = if ($record) { $record.branch } else { 'HEAD' }
    } else {
      $branchName = (git -C $tree.FullName rev-parse --abbrev-ref HEAD 2>$null)
    }
    $warm = if (Test-Path (Join-Path $tree.FullName 'build\build.ninja')) { 'warm' } else { 'cold' }
    Write-Host ($tree.Name + '  [' + $warm + ']  branch=' + $branchName + '  ' + $state)
  }
  return
}

if ($Release) {
  Assert-RecordValue 'RunId' $RunId
  if ($Release -notmatch '^pool-[0-9]+$') { throw 'Release must name one pool-N slot' }
  $worktree = Join-Path $wtRoot $Release
  if (-not (Test-Path -LiteralPath $worktree -PathType Container)) {
    throw ('no such pool tree: ' + $worktree)
  }
  $leasePath = Get-LeasePath $worktree
  $record = Read-LeaseRecord $leasePath
  if (-not $record -or -not $record.lease_token) { throw ('no v2 lease record: ' + $leasePath) }
  if ($record.run_id -ne $RunId) {
    if (-not $RecoverStale) {
      throw ('run_id mismatch: lease belongs to ' + $record.run_id + '; refusing release')
    }
    $ownerState = Get-OwnerState $record
    if ($ownerState -ne 'dead') {
      throw ('refusing stale recovery: owner state is ' + $ownerState + ' (PID/start guard)')
    }
  }
  if (-not $TestLeaseOnly) {
    $dirty = @(git -C $worktree status --porcelain) |
      Where-Object { $_ -notmatch '\.atx-lease($|\.)' }
    if ($dirty) {
      throw ('refusing to release ' + $Release + ': tree is dirty:' + "`n" + ($dirty -join "`n"))
    }
    $detachSha = [string]$record.base_sha
    if ($detachSha -notmatch '^[0-9a-fA-F]{40}$') { throw 'lease has no valid frozen base SHA' }
    git -C $worktree switch --detach $detachSha | Out-Null
    if ($LASTEXITCODE -ne 0) { throw ('git switch --detach failed in ' + $worktree) }
  }
  Remove-RecordByRename $leasePath $record.lease_token 'released'
  Write-Host ('released ' + $Release + ' for run_id=' + $record.run_id + '; build/ kept warm') -ForegroundColor Green
  return
}

Assert-RecordValue 'Branch' $Branch
Assert-RecordValue 'RunId' $RunId

if ($TestLeaseOnly) {
  $baseSha = '0' * 40
} else {
  $baseSha = (git -C $repoRoot rev-parse ($Base + '^{commit}')).Trim()
  if ($LASTEXITCODE -ne 0 -or $baseSha -notmatch '^[0-9a-fA-F]{40}$') {
    throw ('cannot resolve base ref to a commit: ' + $Base)
  }
}

$fields = New-LeaseFields $RunId $Agent $Branch $Base $baseSha
$mutex = $null
$claimed = $false
try {
  $mutex = Enter-SelectionMutex $wtRoot $RunId
  if ($TestSelectionDelayMs -gt 0) { Start-Sleep -Milliseconds $TestSelectionDelayMs }

  $free = Find-FreePool $wtRoot
  if ($free) {
    $worktree = $free.FullName
    $poolName = $free.Name
  } else {
    $number = Find-MissingPoolNumber $wtRoot $MaxPool
    if ($number -eq 0) {
      throw ('pool exhausted (' + $MaxPool + ' slots, all leased)')
    }
    $poolName = 'pool-' + $number
    $worktree = Join-Path $wtRoot $poolName
    if ($TestLeaseOnly) {
      New-Item -ItemType Directory -Path $worktree -ErrorAction Stop | Out-Null
    } else {
      Write-Host ('growing pool: git worktree add --detach ' + $worktree + ' ' + $baseSha) -ForegroundColor Cyan
      git worktree add --detach $worktree $baseSha
      if ($LASTEXITCODE -ne 0) { throw 'git worktree add failed' }
    }
  }

  $leasePath = Get-LeasePath $worktree
  if (-not (Try-NewAtomicRecord $leasePath $fields)) {
    throw ('atomic lease claim lost for ' + $poolName + '; retry with the same run_id')
  }
  $claimed = $true
} finally {
  if ($mutex) { Exit-SelectionMutex $mutex }
}

if (-not $claimed) { throw 'lease acquisition failed before a slot was claimed' }

if ($TestLeaseOnly) {
  Write-Output ('LEASED pool=' + $poolName + ' path=' + $worktree + ' run_id=' + $RunId + ' token=' + $fields.lease_token)
  return
}

# The lease is held before any branch mutation. Setup failures intentionally keep
# it held so another run cannot enter a partially switched/configured worktree.
git -C $repoRoot show-ref --verify --quiet ('refs/heads/' + $Branch)
if ($LASTEXITCODE -eq 0) {
  git -C $worktree switch $Branch
} else {
  git -C $worktree switch -c $Branch $baseSha
}
if ($LASTEXITCODE -ne 0) { throw ('git switch failed; lease remains held: ' + $worktree) }

git -C $worktree submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw ('submodule update failed; lease remains held: ' + $worktree) }

$preset = if ($Shared) { 'dev-shared' } else { 'dev' }
if (-not (Test-Path (Join-Path $worktree 'build\build.ninja'))) {
  Write-Host ('cold tree: configuring preset ' + $preset + ' (one-time for this slot)') -ForegroundColor Cyan
  Push-Location $worktree
  try { & (Join-Path $worktree 'scripts\atx-build.ps1') configure -Preset $preset }
  finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) { throw ('configure failed; lease remains held: ' + $worktree) }
} elseif ($Shared) {
  Write-Host 'NOTE: tree already configured; -Shared ignored.' -ForegroundColor Yellow
}

Write-Host ''
Write-Host ('leased ' + $poolName + ' -> ' + $worktree + ' (branch ' + $Branch + ', base ' + $baseSha + ')') -ForegroundColor Green
Write-Host ('  release: powershell scripts\lease-worktree.ps1 -Release ' + $poolName + ' -RunId ' + $RunId)
