# Asserts the shipped DLL's ABI surface and imports:
#  - exactly the twelve exports MQL5 programs #import (seven frozen legacy names + GetVersion, and the
#    four binary-message exports of Protocol "fbe": DoSendBinary, DoFetchBinary, DoPostBinary,
#    DoListenBinary);
#  - no redistributable runtime imported (static CRT: no MSVCP/VCRUNTIME/ucrtbase, no .NET).
# Usage (x64 developer prompt, dumpbin on PATH): ./check-dll.ps1 <path-to-MQLConnector.dll>
param([Parameter(Mandatory = $true)][string]$Dll)
$ErrorActionPreference = 'Stop'

$expected = @('DestroyLib', 'DoFetchBinary', 'DoListenBinary', 'DoListenMessage', 'DoPostBinary',
              'DoPostMessage', 'DoSendBinary', 'DoSendMessage', 'GetLevels', 'GetSignalCache', 'GetVersion',
              'InitLib')

# dumpbin /exports rows: "ordinal hint RVA name [= internal]"
$exports = @(dumpbin /nologo /exports $Dll |
    ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $Matches[1] } } |
    Sort-Object)
if (Compare-Object $expected $exports) {
    throw "Unexpected exports: $($exports -join ', ') (expected $($expected -join ', '))"
}
Write-Host "Exports OK: $($exports -join ', ')"

$self = Split-Path $Dll -Leaf
# OS API sets (api-ms-win-core-*, e.g. WaitOnAddress behind std::atomic::wait) are system DLLs;
# the redistributable CRT (incl. api-ms-win-crt-*) and the .NET runtime are not.
$forbidden = '^(MSVCP.*|VCRUNTIME.*|UCRTBASE.*|api-ms-win-crt-.*|MSCOREE|VCOMP.*)\.dll$'
$deps = @(dumpbin /nologo /dependents $Dll |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_ -match '^\S+\.dll$' -and $_ -ne $self })
$bad = @($deps | Where-Object { $_ -match $forbidden })
if ($bad.Count -gt 0) { throw "Non-system dependencies: $($bad -join ', ')" }
Write-Host "Dependencies OK: $($deps -join ', ')"
