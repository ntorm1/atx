param(
  [Parameter(Mandatory = $true)] [string] $Binary,
  [Parameter(Mandatory = $true)] [string] $Reference,
  [Parameter(Mandatory = $true)] [string] $SourceRoot,
  [Parameter(Mandatory = $true)] [string] $OutDir,
  [ValidateRange(1, 100)] [int] $Trials = 3
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Hash([string] $Path) {
  (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Host-Jobs {
  # Conservative process gate, in addition to native CPU/SMT interval samples.
  # No process is killed or suspended. Unknown external load is also reflected
  # by the mandatory native whole-system busy share.
  @(Get-Process | Where-Object {
      $_.ProcessName -match '^(clang-cl|clang|lld-link|ninja|cmake|msbuild|cl|rustc|cargo|vcpkg|atx-.*)$'
    } | ForEach-Object { [ordered]@{ pid = $_.Id; name = $_.ProcessName } })
}

foreach ($path in @($Binary, $Reference)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" }
}
if (Test-Path -LiteralPath $OutDir) { throw "Refusing existing output: $OutDir" }
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$Reference = (Resolve-Path -LiteralPath $Reference).Path
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$sourceSha = (& git -C $SourceRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source SHA' }
$dirty = @(& git -C $SourceRoot status --porcelain)
if ($dirty.Count -ne 0) { throw 'Source must be frozen and clean before timing' }
$referenceSha = Hash $Reference
if ($referenceSha -ne 'd4b5c2e81c8971cd9d37abd601139217ba8e71161eee38c3ada7c1dba60343cc') {
  throw 'Wrong frozen reference replay'
}
New-Item -ItemType Directory -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$binarySha = Hash $Binary
Copy-Item -LiteralPath $Binary -Destination (Join-Path $OutDir 'runner.exe')
$manifest = [ordered]@{
  schema = 'cash-pde-cold-trials-v1'; source_sha = $sourceSha; source_root = $SourceRoot
  binary = $Binary; binary_sha256 = $binarySha; reference_sha256 = $referenceSha
  trials = $Trials; planned_processes = 9 * $Trials; records = @(); status = 'incomplete'
  timing_qualified = $false
  declared_gate = 'ThinLTO Release AVX2/FMA precise binary, CPU6 P-core, known SMT siblings each <5%, system busy <10%, no detected build/native jobs before or after any run; all nine route/grid groups and exact outputs required'
  order = '400/800/1600 grids; rotate original,batch-scalar,batch-avx2 by trial; fresh process each'
}
$reports = @()
try {
  $modes = @('original', 'batch-scalar', 'batch-avx2')
  foreach ($grid in @(400, 800, 1600)) {
    for ($trial = 0; $trial -lt $Trials; ++$trial) {
      for ($turn = 0; $turn -lt 3; ++$turn) {
        $mode = $modes[($turn + $trial) % 3]
        $name = '{0}-{1:D3}-{2}' -f $grid, $trial, $mode
        $before = @(Host-Jobs)
        if ($before.Count -ne 0) {
          $manifest.records += [ordered]@{ name = $name; launched = $false; host_jobs = $before }
          throw 'Host has active build/native jobs; retained blocked trial, no process launched'
        }
        if ((Hash $Binary) -ne $binarySha) { throw 'Binary changed during campaign' }
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Binary
        $psi.Arguments = "--mode $mode --grid $grid --affinity 6 --require-pcore"
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.EnvironmentVariables['ATX_VOL_FIT_WORKERS'] = '1'
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $process = [System.Diagnostics.Process]::Start($psi)
        $identity = [ordered]@{ pid = $process.Id
          creation_filetime = $process.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $watch.Stop()
        $path = Join-Path $OutDir "$name.json"
        [System.IO.File]::WriteAllText($path, $stdout.Result)
        [System.IO.File]::WriteAllText((Join-Path $OutDir "$name.stderr.txt"), $stderr.Result)
        $after = @(Host-Jobs)
        $record = [ordered]@{ name = $name; launched = $true; grid = $grid; mode = $mode
          trial = $trial; turn = $turn; exit_code = $process.ExitCode; process = $identity
          launcher_seconds = $watch.Elapsed.TotalSeconds; report = "$name.json"
          report_sha256 = (Hash $path); host_jobs_before = $before; host_jobs_after = $after }
        $manifest.records += $record
        if ($process.ExitCode -ne 0) { throw "Native failure retained: $name" }
        $native = $stdout.Result | ConvertFrom-Json
        if ($native.process.pid -ne $identity.pid -or
            $native.process.creation_filetime -ne $identity.creation_filetime) {
          throw 'Native/launcher process identity mismatch'
        }
        if ($native.mode -ne $mode -or $native.grid.space_base -ne $grid -or
            $native.timed -ne $true) { throw 'Native requested route/grid mismatch' }
        $reports += $path
        if ($after.Count -ne 0) { throw 'Host jobs appeared during trial; receipt retained' }
      }
    }
  }
  if ((Hash $Binary) -ne $binarySha -or
      (& git -C $SourceRoot rev-parse HEAD).Trim() -ne $sourceSha -or
      @(& git -C $SourceRoot status --porcelain).Count -ne 0) {
    throw 'Source or binary changed before campaign completion'
  }
  $summary = Join-Path $OutDir 'numerical-audit.json'
  & python (Join-Path $PSScriptRoot 'audit.py') --reference $Reference --output $summary @reports
  if ($LASTEXITCODE -ne 0) { throw 'Exact numerical/accuracy audit failed' }
  $audit = Get-Content -LiteralPath $summary -Raw | ConvertFrom-Json
  $manifest.timing_qualified = ($audit.native_quiet_all -eq $true)
  $manifest.status = $(if ($manifest.timing_qualified) { 'complete-host-gates-passed' }
                       else { 'complete-unqualified-host' })
} catch {
  $manifest.status = 'blocked-or-failed'
  $manifest['error'] = $_.Exception.Message
  throw
} finally {
  [System.IO.File]::WriteAllText((Join-Path $OutDir 'manifest.json'),
      ($manifest | ConvertTo-Json -Depth 20))
}
if (-not $manifest.timing_qualified) { exit 4 }
