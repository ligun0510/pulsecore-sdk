<#
.SYNOPSIS
  Integrate the PulseCore HL2 mod into a Source SDK 2013 (singleplayer) checkout.

.DESCRIPTION
  Copies pulsecore_hl2_integration.cpp + vendor/pulsecore_client.h into the client project
  (sp/src/game/client) and registers the .cpp in client_hl2.vpc. After running this, regenerate the
  projects and build the Release client, per the README:

    <SdkRoot>\sp\src\creategameprojects.bat
    # then open sp/src/games.sln, build Release of the "client" project -> client.dll

.PARAMETER SdkRoot
  Path to your source-sdk-2013 checkout (the folder that contains "sp\src").

.EXAMPLE
  ./integrate.ps1 -SdkRoot 'C:\dev\source-sdk-2013'
#>
param(
    [Parameter(Mandatory = $true)] [string]$SdkRoot
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$clientDir = Join-Path $SdkRoot 'sp\src\game\client'
if (-not (Test-Path $clientDir)) {
    throw "Not a Source SDK 2013 singleplayer checkout: '$clientDir' not found. Pass -SdkRoot pointing at the folder that contains sp\src (clone https://github.com/ValveSoftware/source-sdk-2013)."
}
$vpc = Join-Path $clientDir 'client_hl2.vpc'
if (-not (Test-Path $vpc)) { throw "client_hl2.vpc not found in $clientDir" }

# 1) copy the mod sources into the client project
Copy-Item (Join-Path $here 'src\pulsecore_hl2_integration.cpp') $clientDir -Force
Copy-Item (Join-Path $here 'vendor\pulsecore_client.h')          $clientDir -Force
Write-Host "Copied pulsecore_hl2_integration.cpp + pulsecore_client.h -> $clientDir" -ForegroundColor Green

# 2) register the .cpp in client_hl2.vpc (idempotent)
$utf8 = New-Object System.Text.UTF8Encoding($false)
$text = [System.IO.File]::ReadAllText($vpc)
if ($text -match 'pulsecore_hl2_integration\.cpp') {
    Write-Host "client_hl2.vpc already references the file - left unchanged." -ForegroundColor Yellow
} else {
    $block = "`t`$Folder`t`"PulseCore Integration`"`r`n`t{`r`n`t`t`$File`t`"pulsecore_hl2_integration.cpp`"`r`n`t}`r`n"
    $idx = $text.LastIndexOf('}')
    if ($idx -lt 0) { throw "client_hl2.vpc has no closing brace to insert before." }
    $text = $text.Substring(0, $idx) + $block + $text.Substring($idx)
    [System.IO.File]::WriteAllText($vpc, $text, $utf8)
    Write-Host "Registered pulsecore_hl2_integration.cpp in client_hl2.vpc." -ForegroundColor Green
}

Write-Host ""
Write-Host "Next:" -ForegroundColor Cyan
Write-Host "  1. Regenerate projects:  `"$SdkRoot\sp\src\creategameprojects.bat`""
Write-Host "  2. Open sp/src/games.sln, build Release of the 'client' project -> client.dll"
Write-Host "  3. Install per README (copy client.dll + gameinfo.txt into steamapps/sourcemods/PulseCore_HL2)."
Write-Host "  4. Start PulseCore, connect your DualSense, launch the mod, switch weapons."
