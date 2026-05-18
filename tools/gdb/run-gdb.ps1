# Launch debug_app under gdb with sculptcore pretty-printers + helpers.
#
#   tools\gdb\run-gdb.ps1 tests\scripts\repro.txt [-BreakOnThrow]

param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$Script,
  [switch]$BreakOnThrow
)

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$bin = Join-Path $repoRoot "build\native\source\debug\debug_app.exe"

if (-not (Test-Path $bin)) {
  Write-Error "debug_app.exe not built. Run: node make.mjs build native"
  exit 2
}

$gdbArgs = @(
  "-q",
  "-ex", "source $repoRoot\tools\gdb\sculptcore.py",
  "-ex", "source $repoRoot\tools\gdb\helpers.gdb"
)
if ($BreakOnThrow) {
  $gdbArgs += @("-ex", "catch throw")
}
$shots = Join-Path $repoRoot "build\shots"
New-Item -ItemType Directory -Force -Path $shots | Out-Null
$gdbArgs += @("--args", $bin, "--script", $Script, "--out", $shots)

& gdb @gdbArgs
