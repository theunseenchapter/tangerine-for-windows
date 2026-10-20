param(
  [string]$Configuration = "Release",
  [string]$OutputName = "FlowDropOlePoc.exe"
)

$ErrorActionPreference = "Stop"
if ([IO.Path]::GetFileName($OutputName) -ne $OutputName) {
  throw "OutputName must be a file name, not a path."
}
$output = Join-Path $PSScriptRoot $OutputName

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  throw "cl.exe was not found. Run this from a Visual Studio Developer PowerShell with the Desktop C++ workload installed."
}

Push-Location $PSScriptRoot
try {
  & cl.exe /nologo /std:c++17 /EHsc /O2 FlowDropOlePoc.cpp `
    /Fe:$output /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib advapi32.lib shcore.lib
  if ($LASTEXITCODE -ne 0) {
    throw "Native build failed with exit code $LASTEXITCODE."
  }
} finally {
  Pop-Location
}

Write-Output "Built $output"
