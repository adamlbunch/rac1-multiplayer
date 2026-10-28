<#
.SYNOPSIS
Installs or updates the Ratchet & Clank co-op mod in RPCS3 and gets RPCS3 ready to play online. Both players run it.

.DESCRIPTION
Finds RPCS3 (or downloads it with -InstallRPCS3), checks the PS3 system software and the game are installed, installs
the latest release of the mod, turns RPCS3's networking on, and optionally saves the username and server address so
the game connects by itself. Run it again to update the mod.

It also puts coop-launcher.ps1 next to rpcs3.exe, with a "Ratchet Co-op" shortcut on the Desktop that runs it. The
launcher boots the mod and keeps RPCS3's log of every session in the coop-logs folder there, so crashes can be
debugged.

Without parameters it asks for what it needs:
  powershell -ExecutionPolicy Bypass -File install-coop.ps1

.PARAMETER RPCS3
The RPCS3 folder (the one with rpcs3.exe). Found automatically if left out.

.PARAMETER InstallRPCS3
Download the latest RPCS3 into $HOME\Emulators\RPCS3 if no RPCS3 is found. Needs 7-Zip.

.PARAMETER Username
Your name in the game. The two players need different names.

.PARAMETER Server
The server to connect to: 127.0.0.1 on the host's own PC, the host's Tailscale IP (100.x.y.z) for the guest.

.PARAMETER NoShortcut
Don't put a "Ratchet Co-op" shortcut on the Desktop.
#>
param(
    [string]$RPCS3,
    [switch]$InstallRPCS3,
    [string]$Username,
    [string]$Server,
    [switch]$NoShortcut
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$ModUrl = 'https://github.com/adamlbunch/rac1-multiplayer/releases/latest/download/Ratchet-Coop-Mod-sync.pkg'
$LauncherUrl = 'https://raw.githubusercontent.com/adamlbunch/rac1-multiplayer/main/setup/coop-launcher.ps1'
$GameIds = 'NPEA00385', 'NPUA80643', 'BCES01503'
$FirmwarePage = 'https://www.playstation.com/support/hardware/ps3/system-software/'

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Ok($text) { Write-Host "   $text" -ForegroundColor Green }
function Warn($text) { Write-Host "   $text" -ForegroundColor Yellow }

# Writes a file the way the game and RPCS3 read it: UTF-8 without a byte order mark
function Write-Plain($path, $text) { [IO.File]::WriteAllText($path, $text) }

function Find-RPCS3 {
    $running = Get-Process rpcs3 -ErrorAction SilentlyContinue | Select-Object -First 1
    $candidates = @()
    if ($running) { $candidates += Split-Path $running.Path }
    $candidates += "$HOME\Emulators\RPCS3", "$HOME\RPCS3", "$HOME\Desktop\RPCS3", "$HOME\Downloads\RPCS3", 'C:\RPCS3',
                   "$env:ProgramFiles\RPCS3", "$env:LOCALAPPDATA\RPCS3"
    foreach ($folder in $candidates) {
        if ($folder -and (Test-Path "$folder\rpcs3.exe")) { return (Resolve-Path $folder).Path }
    }
    return $null
}

function Install-RPCS3Build($folder) {
    $sevenZip = "$env:ProgramFiles\7-Zip\7z.exe"
    if (-not (Test-Path $sevenZip)) {
        throw "Unpacking RPCS3 needs 7-Zip. Install it (winget install --id 7zip.7zip -e) and run this again."
    }
    $release = Invoke-RestMethod https://api.github.com/repos/RPCS3/rpcs3-binaries-win/releases/latest
    $asset = $release.assets | Where-Object name -like '*win64*.7z' | Select-Object -First 1
    $archive = Join-Path $env:TEMP $asset.name
    Write-Host "   Downloading $($asset.name)..."
    Invoke-WebRequest $asset.browser_download_url -OutFile $archive
    New-Item -ItemType Directory -Force $folder | Out-Null
    & $sevenZip x $archive "-o$folder" -y | Out-Null
    Remove-Item $archive
}

# Where RPCS3 keeps dev_hdd0: next to rpcs3.exe unless it was moved in config\vfs.yml
function Get-DevHdd0($folder) {
    $vfs = "$folder\config\vfs.yml"
    if (Test-Path $vfs) {
        $line = Select-String -Path $vfs -Pattern '^/dev_hdd0/:\s*(.+)$' | Select-Object -First 1
        if ($line) {
            $path = $line.Matches[0].Groups[1].Value.Trim().Trim('"') -replace '\$\(EmulatorDir\)', "$folder\"
            return $path.TrimEnd('/', '\')
        }
    }
    return "$folder\dev_hdd0"
}

function Wait-RPCS3Closed($folder) {
    $exe = "$folder\rpcs3.exe"
    while (Get-Process rpcs3 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
        Read-Host "   RPCS3 is open. Close it, then press Enter" | Out-Null
        Start-Sleep 1
    }
}

Step 'RPCS3'
if (-not $RPCS3) { $RPCS3 = Find-RPCS3 }
if (-not $RPCS3) {
    if (-not $InstallRPCS3) {
        $answer = Read-Host "   No RPCS3 found. Type the folder it's in, or press Enter to download it to $HOME\Emulators\RPCS3"
        if ($answer) { $RPCS3 = $answer } else { $InstallRPCS3 = $true }
    }
    if ($InstallRPCS3) {
        $RPCS3 = "$HOME\Emulators\RPCS3"
        Install-RPCS3Build $RPCS3
    }
}
if (-not (Test-Path "$RPCS3\rpcs3.exe")) { throw "No rpcs3.exe in $RPCS3" }
$RPCS3 = (Resolve-Path $RPCS3).Path
Ok "Using $RPCS3"
Wait-RPCS3Closed $RPCS3

Step 'Launcher and crash logs'
$launcher = "$RPCS3\coop-launcher.ps1"
if (Test-Path "$PSScriptRoot\coop-launcher.ps1") {
    if ($PSScriptRoot -ne $RPCS3) { Copy-Item "$PSScriptRoot\coop-launcher.ps1" $launcher -Force }
} else {
    Invoke-WebRequest $LauncherUrl -OutFile $launcher
}
# RPCS3 rewrites its log each time it starts, and this starts it below: save the last session's log first
try {
    $saved = & $launcher -SaveOnly
    if ($saved) { Ok "Saved RPCS3's log of the last session: $saved" }
} catch {
    Warn "Couldn't save RPCS3's log of the last session: $_"
}
$shell = New-Object -ComObject WScript.Shell
$link = "$([Environment]::GetFolderPath('Desktop'))\Ratchet Co-op.lnk"
$linkTarget = if (Test-Path $link) { $shell.CreateShortcut($link).TargetPath } else { '' }
if ($linkTarget -like '*\Play Co-op.cmd') {
    # The host's shortcut from setup-server.ps1: it starts the server too, then this launcher
    if ((Test-Path $linkTarget) -and (Select-String -Path $linkTarget -Pattern 'coop-launcher' -Quiet)) {
        Ok 'Wrote coop-launcher.ps1, which the "Ratchet Co-op" shortcut (Play Co-op.cmd) starts'
    } else {
        Warn 'Wrote coop-launcher.ps1. Run setup-server.ps1 again so the "Ratchet Co-op" shortcut uses it.'
    }
} elseif ($NoShortcut) {
    Ok "Wrote coop-launcher.ps1"
} else {
    $shortcut = $shell.CreateShortcut($link)
    $shortcut.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$launcher`""
    $shortcut.WorkingDirectory = $RPCS3
    $shortcut.IconLocation = "$RPCS3\rpcs3.exe,0"
    # Minimized, so the hidden PowerShell window doesn't flash up
    $shortcut.WindowStyle = 7
    $shortcut.Save()
    Ok 'Wrote coop-launcher.ps1 and a "Ratchet Co-op" shortcut on the Desktop that runs it'
}
Ok "The log of every session the launcher starts is kept in $RPCS3\coop-logs"

if (-not (Test-Path "$RPCS3\config\config.yml")) {
    Write-Host '   Starting RPCS3 once so it sets itself up. Close its window (and the welcome window) when it has opened.'
    Start-Process "$RPCS3\rpcs3.exe" -Wait
}
$DevHdd0 = Get-DevHdd0 $RPCS3

Step 'PS3 system software'
if (Test-Path "$RPCS3\dev_flash\vsh\module\vsh.self") {
    Ok 'Installed'
} else {
    Warn "Not installed. Download PS3UPDAT.PUP from Sony ($FirmwarePage), then"
    Warn "install it in RPCS3 with File -> Install Firmware. The game won't start without it."
}

Step 'The game'
$games = $GameIds | Where-Object { Test-Path "$DevHdd0\game\$_\USRDIR\EBOOT.BIN" }
if ($games) {
    Ok "Found $($games -join ', ')"
} else {
    Warn "None of $($GameIds -join ', ') is installed. Install your own copy of the PS3 Ratchet & Clank:"
    Warn "a PSN .pkg with File -> Install Packages/Raps/Edats (and its .rap), or the Trilogy disc's PS3_GAME folder"
    Warn "copied to $DevHdd0\game\BCES01503."
}

Step 'Co-op mod'
$package = Join-Path $env:TEMP 'Ratchet-Coop-Mod-sync.pkg'
Write-Host '   Downloading the latest release...'
Invoke-WebRequest $ModUrl -OutFile $package
$modFile = "$DevHdd0\game\BORD00001\USRDIR\mod.sprx"
$before = if (Test-Path $modFile) { (Get-Item $modFile).LastWriteTimeUtc } else { [DateTime]::MinValue }
Write-Host '   RPCS3 opens and asks "Do you want to install this package?": click Install.' -ForegroundColor White
Write-Host '   When it says the installation finished, close RPCS3.' -ForegroundColor White
Start-Process "$RPCS3\rpcs3.exe" -ArgumentList '--installpkg', "`"$package`"" -Wait
Remove-Item $package
if ((Test-Path $modFile) -and (Get-Item $modFile).LastWriteTimeUtc -gt $before) {
    Ok 'Installed: "Ratchet Multiplayer" in the game list'
} elseif (Test-Path $modFile) {
    Warn 'The mod files didn''t change. If you didn''t click Install, run this again.'
} else {
    throw 'The mod isn''t installed. Run this again and click Install in the RPCS3 window.'
}

Step 'Networking'
$config = "$RPCS3\config\config.yml"
$text = [IO.File]::ReadAllText($config)
if ($text -match '(?m)^\s+Internet enabled:\s*Connected\s*$') {
    Ok 'Already on'
} else {
    Write-Plain $config ($text -replace '(?m)^(\s+Internet enabled:)[^\r\n]*', '$1 Connected')
    Ok 'Turned on (Configuration -> Network -> Network Status: Connected)'
}

Step 'Username and server'
$settingsFile = "$DevHdd0\game\BORD00001\USRDIR\settings.conf"
$settings = [ordered]@{}
if (Test-Path $settingsFile) {
    foreach ($line in [IO.File]::ReadAllText($settingsFile) -split "`n") {
        if ($line.TrimEnd("`r") -match '^([^=]+)=(.*)$') { $settings[$Matches[1]] = $Matches[2] }
    }
}
if (-not $PSBoundParameters.ContainsKey('Username') -and -not $PSBoundParameters.ContainsKey('Server')) {
    $Username = Read-Host '   Your name in the game (Enter to type it in the game instead)'
    if ($Username) {
        $Server = Read-Host '   Server address: 127.0.0.1 if you host, else the host''s Tailscale IP (Enter to skip)'
    }
}
if ($Username) { $settings['username'] = "`"$Username`"" }
if ($Server) {
    if ($Server -notmatch ':\d+$') { $Server = "${Server}:2407" }
    $settings['last_direct_ip'] = "`"$Server`""
    $settings['autoconnect'] = 'true'
}
if ($Username -or $Server) {
    Write-Plain $settingsFile ((($settings.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join "`n") + "`n")
    Ok ($(if ($Server) { "Saved: connects to $Server as soon as you press Start" } else { 'Saved' }))
} else {
    Ok 'Skipped: the game asks when you start it'
}

Step 'Done'
Write-Host '   Start the game with the "Ratchet Co-op" shortcut on the Desktop. Press Start; the host presses Circle to'
Write-Host '   make a lobby, the guest picks it with Cross and presses Circle. Full guide:'
Write-Host '   https://github.com/adamlbunch/rac1-multiplayer'
Write-Host "   If the game crashes, send the newest zip in $RPCS3\coop-logs (its name ends in -crash)."
