param(
  [string]$OutputName = "FlowDropContextMenu.dll"
)

$ErrorActionPreference = "Stop"
if ([IO.Path]::GetFileName($OutputName) -ne $OutputName) {
  throw "OutputName must be a file name, not a path."
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  throw "cl.exe was not found. Run this from a Visual Studio Developer PowerShell with the Desktop C++ workload installed."
}

Push-Location $PSScriptRoot
try {
  & cl.exe /nologo /std:c++17 /EHsc /O2 /LD FlowDropContextMenu.cpp `
    /Fe:$OutputName /link /DEF:FlowDropContextMenu.def ole32.lib shell32.lib advapi32.lib
  if ($LASTEXITCODE -ne 0) {
    throw "Context-menu extension build failed with exit code $LASTEXITCODE."
  }
} finally {
  Pop-Location
}

Write-Output "Built $PSScriptRoot\$OutputName"
Write-Output "Register with: regsvr32 /s `"$PSScriptRoot\$OutputName`""
