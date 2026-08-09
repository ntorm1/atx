<#
  Shared helper for the atx-vol CI gate scripts (Task E3). Dot-sourced, never
  invoked directly.

  Get-PresetBinaryDir mirrors scripts\atx-build.ps1's function of the same
  name byte-for-byte in behavior (deliberately NOT hardcoded as a name->dir
  table -- see that script's own comment on why: a hardcoded table can desync
  from CMakePresets.json the moment someone edits a preset's binaryDir or
  inherits chain without also touching every script that hardcoded it. Reads
  CMakePresets.json and walks the `inherits` chain until a binaryDir is
  found, exactly like CMake itself resolves it).
#>

function Get-PresetBinaryDir {
  param(
    [Parameter(Mandatory = $true)][string] $RepoRoot,
    [Parameter(Mandatory = $true)][string] $Name
  )
  $presetsFile = Join-Path $RepoRoot "CMakePresets.json"
  if (-not (Test-Path $presetsFile)) { return "build" }
  $presets = (Get-Content $presetsFile -Raw | ConvertFrom-Json).configurePresets

  $current = $Name
  for ($hop = 0; $hop -lt $presets.Count; $hop++) {
    $preset = $presets | Where-Object { $_.name -eq $current } | Select-Object -First 1
    if ($null -eq $preset) { break }
    if ($preset.binaryDir) {
      return ($preset.binaryDir -replace '\$\{sourceDir\}/?', '') -replace '/', '\'
    }
    if (-not $preset.inherits) { break }
    $current = @($preset.inherits)[0]
  }
  throw "atx-vol/ci: cannot resolve binaryDir for preset '$Name' from CMakePresets.json"
}
