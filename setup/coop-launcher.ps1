<#
.SYNOPSIS
Updates the Ratchet & Clank co-op mod, boots it in RPCS3 and keeps RPCS3's log of every session, so crashes can be
debugged.

.DESCRIPTION
install-coop.ps1 puts this next to rpcs3.exe, with a "Ratchet Co-op" shortcut on the Desktop that runs it. Before
RPCS3 starts, an installed release of the mod is replaced with the release marked latest on GitHub, from that
release's Ratchet-Coop-Mod-files.zip. A mod.sprx that isn't from any release (a build of your own) is left alone,
and without internet the game starts with what's installed.

RPCS3 rewrites log\RPCS3.log every time it starts, so when RPCS3 closes this zips the session's logs into
coop-logs\<start time>.zip, "-crash" added to the name if the game hit a fatal error or RPCS3 didn't exit cleanly.
The newest 30 are kept. A session RPCS3 was started for some other way is saved the next time this runs, unless RPCS3
was opened again in between. Errors go to coop-logs\launcher-errors.txt, since this runs without a window.

  powershell -ExecutionPolicy Bypass -File coop-launcher.ps1

.PARAMETER SaveOnly
Only save the last session's log if it isn't saved yet, and print the zip's path. install-coop.ps1 does this before
it opens RPCS3.
#>
param([switch]$SaveOnly)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Releases = 'https://api.github.com/repos/adamlbunch/rac1-multiplayer/releases'
# What the mod's package installs into the game's folder, attached to every release from a version tag
$FilesAsset = 'Ratchet-Coop-Mod-files.zip'

$RPCS3 = $PSScriptRoot
$Exe = "$RPCS3\rpcs3.exe"
$LogDir = "$RPCS3\log"
$Logs = "$RPCS3\coop-logs"
$Keep = 30
# The write time and size of the last log saved, so a log isn't saved twice
$SavedMarker = "$Logs\last-saved.txt"

# Where RPCS3 keeps dev_hdd0: next to rpcs3.exe unless it was moved in config\vfs.yml
function Get-DevHdd0 {
    $vfs = "$RPCS3\config\vfs.yml"
    if (Test-Path $vfs) {
        $line = Select-String -Path $vfs -Pattern '^/dev_hdd0/:\s*(.+)$' | Select-Object -First 1
        if ($line) {
            $path = $line.Matches[0].Groups[1].Value.Trim().Trim('"') -replace '\$\(EmulatorDir\)', "$RPCS3\"
            return $path.TrimEnd('/', '\')
        }
    }
    return "$RPCS3\dev_hdd0"
}

function Get-RPCS3Process { Get-Process rpcs3 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } }

function Format-Time($time) { $time.ToString('yyyy-MM-dd HH:mm:ss') }

# Zips RPCS3.log and TTY.log with a session.txt summary into coop-logs and returns the zip's path, unless that log
#  is saved already. RPCS3 must be closed.
function Save-Session($exitCode) {
    $log = "$LogDir\RPCS3.log"
    if (-not (Test-Path $log)) { return }
    $file = Get-Item $log
    $id = "$($file.LastWriteTimeUtc.Ticks) $($file.Length)"
    if ((Test-Path $SavedMarker) -and [IO.File]::ReadAllText($SavedMarker) -eq $id) { return }

    # RPCS3 writes when it started near the top: "Current Time: 2026-10-27T12:29:56"
    $started = $file.LastWriteTime
    foreach ($line in Get-Content $log -TotalCount 10 -Encoding UTF8) {
        if ($line -match '^Current Time: (\S+)') {
            try { $started = [DateTime]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) } catch {}
            break
        }
    }
    # Fatal errors are the lines starting with a middle dot and "F", like the game's access violations
    $fatal = @(Select-String -Path $log -Pattern "^$([char]0xB7)F " -Encoding UTF8)
    $crashed = $fatal.Count -gt 0 -or ($null -ne $exitCode -and $exitCode -ne 0)

    $exitText = if ($null -eq $exitCode) { 'unknown (not started by coop-launcher.ps1)' } else { '{0} (0x{0:X8})' -f $exitCode }
    # The mod's hash tells which release it was
    $mod = "$(Get-DevHdd0)\game\BORD00001\USRDIR\mod.sprx"
    $modText = if (Test-Path $mod) {
        "mod.sprx SHA256 $((Get-FileHash $mod).Hash), written $(Format-Time (Get-Item $mod).LastWriteTime)"
    } else { 'not installed' }
    $summary = @(
        "RPCS3 started: $(Format-Time $started)",
        "Log last written: $(Format-Time $file.LastWriteTime)",
        "RPCS3 exit code: $exitText",
        "Mod: $modText",
        "Fatal errors: $($fatal.Count)"
    ) + @($fatal | Select-Object -First 10 | ForEach-Object { "  $($_.Line)" })

    New-Item -ItemType Directory -Force $Logs | Out-Null
    $zipPath = "$Logs\$($started.ToString('yyyy-MM-dd_HH-mm-ss'))$(if ($crashed) { '-crash' }).zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($zipPath, 'Create')
    try {
        foreach ($name in 'RPCS3.log', 'TTY.log') {
            if (Test-Path "$LogDir\$name") {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$LogDir\$name", $name, 'Optimal')
            }
        }
        $writer = New-Object IO.StreamWriter($zip.CreateEntry('session.txt').Open())
        $writer.Write(($summary -join "`r`n") + "`r`n")
        $writer.Dispose()
    } finally {
        $zip.Dispose()
    }
    [IO.File]::WriteAllText($SavedMarker, $id)

    Get-ChildItem $Logs -Filter *.zip | Sort-Object LastWriteTime -Descending | Select-Object -Skip $Keep | Remove-Item
    return $zipPath
}

# The SHA256 GitHub lists for one of a release's files, as "sha256:<hex>"
function Get-AssetDigest($release, $name) { ($release.assets | Where-Object name -eq $name).digest }

# Replaces an installed release with the one marked latest, so marking an older release latest rolls players back.
#  Returns the old and new release's tags, or nothing if it changed nothing. The new files are written next to the
#  old ones and only swapped in once they're all there, so a failed download leaves the installed release working.
function Update-Mod {
    $game = [IO.Path]::GetFullPath("$(Get-DevHdd0)\game\BORD00001")
    $mod = "$game\USRDIR\mod.sprx"
    if (-not (Test-Path $mod)) { return }
    $installed = "sha256:$((Get-FileHash $mod).Hash)"
    $latest = Invoke-RestMethod "$Releases/latest" -TimeoutSec 10
    if ((Get-AssetDigest $latest 'mod.sprx') -eq $installed) { return }
    # Windows PowerShell passes the list on as one object: ForEach-Object splits it into the releases
    $from = Invoke-RestMethod "$($Releases)?per_page=100" -TimeoutSec 10 |
        ForEach-Object { $_ } | Where-Object { (Get-AssetDigest $_ 'mod.sprx') -eq $installed } | Select-Object -First 1
    $files = $latest.assets | Where-Object name -eq $FilesAsset
    if (-not $from -or -not $files) { return }

    $download = "$env:TEMP\$FilesAsset"
    Invoke-WebRequest $files.browser_download_url -OutFile $download -TimeoutSec 120
    try {
        if ("sha256:$((Get-FileHash $download).Hash)" -ne $files.digest) { throw "$FilesAsset didn't download whole" }
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($download)
        try {
            $paths = foreach ($entry in $zip.Entries | Where-Object Name) {
                $path = [IO.Path]::GetFullPath((Join-Path $game $entry.FullName))
                if (-not $path.StartsWith("$game\", 'OrdinalIgnoreCase')) { throw "$FilesAsset has $($entry.FullName)" }
                New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$path.new", $true)
                $path
            }
        } finally {
            $zip.Dispose()
        }
    } finally {
        Remove-Item $download
    }
    foreach ($path in $paths) { Move-Item -Force "$path.new" $path }
    return $from.tag_name, $latest.tag_name
}

function Write-LauncherError($err) {
    New-Item -ItemType Directory -Force $Logs | Out-Null
    [IO.File]::AppendAllText("$Logs\launcher-errors.txt", "$(Format-Time (Get-Date)) $err`r`n$($err.ScriptStackTrace)`r`n`r`n")
}

function Show-Message($text) {
    Add-Type -AssemblyName System.Windows.Forms
    [void][Windows.Forms.MessageBox]::Show($text, 'Ratchet Co-op')
}

if ($SaveOnly) {
    # A running RPCS3 is still writing its log
    if (-not (Get-RPCS3Process)) { Save-Session $null }
    return
}

try {
    if (Get-RPCS3Process) {
        Show-Message 'RPCS3 is already open. Close it, then start Ratchet Co-op again.'
        return
    }
    [void](Save-Session $null)

    # Without internet, or with GitHub down, the game starts with the release it has
    try {
        $from, $to = Update-Mod
        if ($to) {
            Show-Message ("The co-op mod was updated to $to (you had $from). Everyone needs the same version to play " +
                'together: the other players get it the next time they start Ratchet Co-op.')
        }
    } catch {
        Write-LauncherError $_
    }

    $start = @{ FilePath = $Exe; WorkingDirectory = $RPCS3; PassThru = $true }
    $eboot = "$(Get-DevHdd0)\game\BORD00001\USRDIR\EBOOT.BIN"
    if (Test-Path $eboot) { $start.ArgumentList = "`"$eboot`"" }
    $process = Start-Process @start
    # Reading the handle now keeps the exit code readable after RPCS3 exits
    [void]$process.Handle
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    # RPCS3 starts itself again after updating, and it may have been opened a second time: wait for all of them
    while ($running = Get-RPCS3Process) { $running | Wait-Process }
    [void](Save-Session $exitCode)
} catch {
    Write-LauncherError $_
    Show-Message "Ratchet Co-op: $_`n`nDetails are in $Logs\launcher-errors.txt"
}
