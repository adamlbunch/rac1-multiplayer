<#
.SYNOPSIS
Builds and sets up the Ratchet & Clank co-op server on the host's PC. Only the host runs it.

.DESCRIPTION
Gets the server's source (adamlbunch/Lawrence, branch coop-fixes), builds it, writes its settings and two launchers:
"Start Co-op Server.cmd" and "Play Co-op.cmd" (starts the server if needed and boots the mod in RPCS3). Run it again
to update the server; settings, saved campaign and launchers are kept.

  powershell -ExecutionPolicy Bypass -File setup-server.ps1

.PARAMETER Folder
Where the server goes. Default: $HOME\Emulators\Lawrence

.PARAMETER Source
Where the server's source is cloned. Default: $HOME\src\Lawrence

.PARAMETER RPCS3
The RPCS3 folder, for "Play Co-op.cmd". Found automatically if left out.

.PARAMETER Port
The port the server listens on (UDP and TCP). Default: 2407

.PARAMETER InstallTools
Install Git and the .NET 8 SDK with winget if they're missing, without asking first.

.PARAMETER NoShortcut
Don't put a "Ratchet Co-op" shortcut on the Desktop.

.PARAMETER AddFirewallRule
Let the server through Windows Firewall. Needs PowerShell started as administrator. Without it, Windows asks the
first time the server starts: click Allow.
#>
param(
    [string]$Folder = "$HOME\Emulators\Lawrence",
    [string]$Source = "$HOME\src\Lawrence",
    [string]$RPCS3,
    [int]$Port = 2407,
    [switch]$NoShortcut,
    [switch]$InstallTools,
    [switch]$AddFirewallRule
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Repository = 'https://github.com/adamlbunch/Lawrence'
$Branch = 'coop-fixes'

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Ok($text) { Write-Host "   $text" -ForegroundColor Green }
function Warn($text) { Write-Host "   $text" -ForegroundColor Yellow }

function Write-Plain($path, $text) { [IO.File]::WriteAllText($path, $text) }

function Test-Command($name) { [bool](Get-Command $name -ErrorAction SilentlyContinue) }

function Install-Tool($name, $wingetId) {
    if (-not $InstallTools) {
        $answer = Read-Host "   $name is needed to build the server. Install it now with winget? (y/N)"
        if ($answer -notmatch '^[Yy]') { throw "$name is needed: winget install --id $wingetId -e" }
    }
    winget install --id $wingetId -e --accept-source-agreements --accept-package-agreements
    # Pick up the PATH the installer changed
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}

Step 'Build tools'
if (-not (Test-Command git)) { Install-Tool 'Git' 'Git.Git' }
# Any SDK from .NET 8 on builds it (the source's global.json rolls forward to the newest)
$hasSdk = (Test-Command dotnet) -and (@(dotnet --list-sdks) | Where-Object { [int]($_ -split '\.')[0] -ge 8 })
if (-not $hasSdk) { Install-Tool '.NET 8 SDK' 'Microsoft.DotNet.SDK.8' }
Ok 'Git and the .NET SDK ready'

Step 'Server source'
if (Test-Path "$Source\.git") {
    git -C $Source fetch --quiet origin
    git -C $Source checkout --quiet $Branch
    git -C $Source pull --quiet --ff-only origin $Branch
    Ok "Updated $Source"
} else {
    git clone --quiet --branch $Branch $Repository $Source
    Ok "Cloned to $Source"
}

Step 'Build'
$running = Get-Process Lawrence -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$Folder\Lawrence.exe" }
while ($running) {
    Read-Host '   The server is running. Close its window, then press Enter' | Out-Null
    Start-Sleep 1
    $running = Get-Process Lawrence -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$Folder\Lawrence.exe" }
}
dotnet publish "$Source\Lawrence" -c Release -f net8.0 -r win-x64 --self-contained -p:PublishSingleFile=true -o $Folder --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw 'The build failed' }
if (-not (Test-Path "$Folder\mods\coop")) { throw "The build has no mods\coop in $Folder" }
Ok "Built into $Folder"

Step 'Settings'
$settingsFile = "$Folder\settings.toml"
if (Test-Path $settingsFile) {
    Ok 'Kept the existing settings.toml'
} else {
    Write-Plain $settingsFile @"
[Server]
name = "Ratchet Co-op"
port = $Port
max_players = 4
address = "0.0.0.0"
# Keep this server out of the public server list. Friends join with Direct IP.
advertise = false
# Plain console log instead of the full-screen UI
interactive = false

[Logger]
path = "lawrence.log"

[Mod.coop]
enabled = true

# Only co-op is used. Listing the rest stops the server from rewriting this file.
[Mod.hide-seek]
enabled = false

[Mod.mgb]
enabled = false

[Mod.race]
enabled = false

[Mod.randomizer]
enabled = false

[Mod.sandbox]
enabled = false

[Mod.tag]
enabled = false

"@
    Ok "Wrote settings.toml (port $Port)"
}

Step 'Launchers'
Write-Plain "$Folder\Start Co-op Server.cmd" @"
@echo off
title Ratchet ^& Clank Co-op Server
cd /d "%~dp0"
echo Co-op server running. Keep this window open while you play. Close it to stop the server.
echo.
"%~dp0Lawrence.exe"
pause

"@
if (-not $RPCS3) {
    $process = Get-Process rpcs3 -ErrorAction SilentlyContinue | Select-Object -First 1
    $RPCS3 = @($(if ($process) { Split-Path $process.Path }), "$HOME\Emulators\RPCS3", "$HOME\RPCS3", 'C:\RPCS3') |
        Where-Object { $_ -and (Test-Path "$_\rpcs3.exe") } | Select-Object -First 1
}
if ($RPCS3) {
    Write-Plain "$Folder\Play Co-op.cmd" @"
@echo off
rem Starts the co-op server (if it isn't running yet) and boots the co-op mod in RPCS3.
cd /d "%~dp0"
tasklist /fi "imagename eq Lawrence.exe" | find /i "Lawrence.exe" >nul
if errorlevel 1 start "Ratchet Co-op Server" "%~dp0Start Co-op Server.cmd"
start "" "$RPCS3\rpcs3.exe" "$RPCS3\dev_hdd0\game\BORD00001\USRDIR\EBOOT.BIN"

"@
    if ($NoShortcut) {
        Ok 'Wrote Start Co-op Server.cmd and Play Co-op.cmd'
    } else {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut("$([Environment]::GetFolderPath('Desktop'))\Ratchet Co-op.lnk")
        $shortcut.TargetPath = "$Folder\Play Co-op.cmd"
        $shortcut.WorkingDirectory = $Folder
        $shortcut.IconLocation = "$RPCS3\rpcs3.exe,0"
        $shortcut.Save()
        Ok 'Wrote Start Co-op Server.cmd, Play Co-op.cmd and a "Ratchet Co-op" shortcut on the Desktop'
    }
} else {
    Ok 'Wrote Start Co-op Server.cmd (no RPCS3 found for Play Co-op.cmd: run this again with -RPCS3 <folder>)'
}

Step 'Firewall'
if ($AddFirewallRule) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { throw '-AddFirewallRule needs PowerShell started as administrator' }
    Get-NetFirewallRule -DisplayName 'Ratchet Co-op Server' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName 'Ratchet Co-op Server' -Direction Inbound -Program "$Folder\Lawrence.exe" -Action Allow | Out-Null
    Ok 'Allowed Lawrence.exe through Windows Firewall'
} else {
    Ok 'The first time the server starts, Windows asks whether to allow Lawrence.exe: click Allow'
}

Step 'Done'
$tailscaleIp = if (Test-Command tailscale) { (tailscale ip -4 2>$null | Select-Object -First 1) } else { $null }
Write-Host '   Start the game with the "Ratchet Co-op" shortcut (or Play Co-op.cmd), and connect to 127.0.0.1.'
if ($tailscaleIp) {
    Write-Host "   Your friend connects to your Tailscale IP: $tailscaleIp" -ForegroundColor White
} else {
    Warn 'Install Tailscale (winget install --id Tailscale.Tailscale -e) and share this PC with your friend;'
    Warn 'they connect to its Tailscale IP (tailscale ip -4).'
}
