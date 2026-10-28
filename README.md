# Ratchet & Clank Co-op: setup guide for AI agents

**People:** give this page's link to your AI agent (Claude Code, Codex, Cursor or similar) and ask it to set up
Ratchet & Clank co-op for you. Everything below is written for the agent.

**Agents:** this guide is for you. It sets up two-player online co-op of the PS3 Ratchet & Clank (2002) for your user,
end to end, with a check for each step. If your web tool summarizes or shortens long pages, read the plain text
instead: https://raw.githubusercontent.com/adamlbunch/rac1-multiplayer/main/README.md

The game runs in the RPCS3 emulator with this repository's co-op build of the
[rac1-multiplayer](https://github.com/bordplate/rac1-multiplayer) mod. One player (the **host**) also runs the co-op
server, built from the `main` branch of [adamlbunch/Lawrence](https://github.com/adamlbunch/Lawrence). The other player
is the **guest**. Both need this build of the mod. The official mod from bordplate's releases connects but doesn't
sync the game the same way, so don't install it. In co-op, weapons, gadgets, bolts, planets, gold bolts and skill
points are shared, and the campaign saves on the host's PC.

Commands are for Windows (PowerShell), which is what has been tested. Linux and macOS notes are near the end.

## Start here

Ask the user first:
1. **Host or guest?** Only the host runs the server (step 8). The guest does everything else.
2. **Guest only:** the host's Tailscale IP (`100.x.y.z`) and the Tailscale share link or invite the host sent (step 7).
   Without them the guest can install everything but can't connect yet.
3. **Their name in the game.** It must differ from the other player's.

If the user played an earlier version, most of it is already there: run `install-coop.ps1` (fast path below), which
updates the mod and keeps their settings, then check the table.

## Ground rules

- **The user must own the game.** Never download Ratchet & Clank, a `.rap` license, or PS3 system software from
  anywhere but the user's own files and Sony's official site. If the user doesn't have the game, stop and tell them.
- **Ask the user before** installing software, changing firewall rules, or anything they sign in to (Tailscale). They
  do the sign-ins themselves in their browser.
- Close RPCS3 before editing its files or installing into it from the command line.
- Never delete anything inside the RPCS3 folder that you didn't create.

## What the finished setup looks like

| Piece | Where (Windows, portable RPCS3 in `$RPCS3`) | Check |
|---|---|---|
| RPCS3 | `$RPCS3\rpcs3.exe` | file exists |
| PS3 system software | `$RPCS3\dev_flash` | `$RPCS3\dev_flash\vsh\module\vsh.self` exists |
| The game (user's copy) | `$RPCS3\dev_hdd0\game\<ID>` with ID `NPEA00385`, `NPUA80643` or `BCES01503` | `...\<ID>\USRDIR\EBOOT.BIN` exists |
| The co-op mod | `$RPCS3\dev_hdd0\game\BORD00001` ("Ratchet Multiplayer" in the game list) | `...\BORD00001\USRDIR\mod.sprx` exists |
| Networking on | `$RPCS3\config\config.yml`, section `Net:` | `Internet enabled: Connected` |
| Private network between the two PCs | Tailscale | `tailscale ping <other PC's 100.x IP>` answers |
| Co-op server (host only) | e.g. `$HOME\Emulators\Lawrence` | log says `Started Lawrence on 0.0.0.0:2407` |
| Launcher that keeps crash logs | `$RPCS3\coop-launcher.ps1`, run by the "Ratchet Co-op" Desktop shortcut | file exists |

## Fast path: the setup scripts

Two scripts in [`setup/`](setup) do most of the steps below and check their results. Run them, then use the numbered
steps for whatever they report as missing (system software, the game, Tailscale) and to check things.

```powershell
# Both players: finds RPCS3 (-InstallRPCS3 downloads it if there's none), checks the system software and the game,
#  installs the latest mod, turns networking on, saves the name and server address, and adds the launcher
Invoke-WebRequest https://raw.githubusercontent.com/adamlbunch/rac1-multiplayer/main/setup/install-coop.ps1 -OutFile "$env:TEMP\install-coop.ps1"
powershell -ExecutionPolicy Bypass -File "$env:TEMP\install-coop.ps1" -RPCS3 "C:\path\to\RPCS3" -Username "TheirName" -Server "100.x.y.z"

# Host only: builds the server, writes its settings and launchers and a Desktop shortcut
Invoke-WebRequest https://raw.githubusercontent.com/adamlbunch/rac1-multiplayer/main/setup/setup-server.ps1 -OutFile "$env:TEMP\setup-server.ps1"
powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-server.ps1" -RPCS3 "C:\path\to\RPCS3"
```

- `install-coop.ps1` opens RPCS3 to install the package, and RPCS3 asks "Do you want to install this package?". The
  user clicks Install, then closes RPCS3 when it's done (on a first start RPCS3 shows a welcome window first, which
  they close). Pass `-Username` and `-Server` (the host uses `127.0.0.1`) so it doesn't stop to ask.
- It also puts `coop-launcher.ps1` in the RPCS3 folder and a "Ratchet Co-op" shortcut on the Desktop that runs it
  (`-NoShortcut` skips the shortcut). The launcher boots the mod and keeps RPCS3's log of each session, see
  [Crashes](#crashes). On the host, `setup-server.ps1` makes "Ratchet Co-op" start the server first, then the launcher.
- `setup-server.ps1` needs Git and a .NET SDK 8 or newer. It asks before installing them, or `-InstallTools` installs
  them without asking (only with the user's OK). `-AddFirewallRule` lets the server through Windows Firewall, which
  needs an administrator PowerShell. Run it again to update the server; settings and the saved campaign are kept.

## 1. RPCS3 (both)

1. Install the Visual C++ runtime RPCS3 needs, and 7-Zip to unpack it:
   ```powershell
   winget install --id Microsoft.VCRedist.2015+.x64 -e
   winget install --id 7zip.7zip -e
   ```
2. Download the latest Windows build from the official builds repository and unpack it to a folder of its own:
   ```powershell
   $RPCS3 = "$HOME\Emulators\RPCS3"
   $release = Invoke-RestMethod https://api.github.com/repos/RPCS3/rpcs3-binaries-win/releases/latest
   $asset = $release.assets | Where-Object name -like '*win64*.7z' | Select-Object -First 1
   Invoke-WebRequest $asset.browser_download_url -OutFile "$env:TEMP\$($asset.name)"
   & "$env:ProgramFiles\7-Zip\7z.exe" x "$env:TEMP\$($asset.name)" "-o$RPCS3" -y
   ```
   If the user already has RPCS3, use their folder as `$RPCS3` and skip this.
3. Start `$RPCS3\rpcs3.exe` once and close it, so it creates its folders and `config\config.yml`.

## 2. PS3 system software (both)

The user downloads `PS3UPDAT.PUP` from Sony: https://www.playstation.com/support/hardware/ps3/system-software/
(the "PS3 System Software update" file). Then:

```powershell
& "$RPCS3\rpcs3.exe" --installfw "C:\path\to\PS3UPDAT.PUP"
```

RPCS3 opens and shows the install progress. Close it when it's done.
Check: `Test-Path "$RPCS3\dev_flash\vsh\module\vsh.self"`.

## 3. The game (both, the user's own copy)

Any of these works. The two players don't have to have the same one, but it's best if they do.

- **European PSN release `NPEA00385`** or **US PSN release `NPUA80643`**: the user's `.pkg`, plus its `.rap` license
  copied to `$RPCS3\dev_hdd0\home\00000001\exdata\`. Install the `.pkg` with
  `& "$RPCS3\rpcs3.exe" --installpkg "C:\path\to\game.pkg"`.
- **European Trilogy disc `BCES01503`**: copy the disc's `PS3_GAME` folder to `$RPCS3\dev_hdd0\game\BCES01503` (so
  `PARAM.SFO` and `USRDIR` sit directly inside it).

Check: `Test-Path "$RPCS3\dev_hdd0\game\<ID>\USRDIR\EBOOT.BIN"` for the ID the user has. The PS2 disc doesn't work.

## 4. The co-op mod (both)

```powershell
Invoke-WebRequest https://github.com/adamlbunch/rac1-multiplayer/releases/latest/download/Ratchet-Coop-Mod-sync.pkg -OutFile "$env:TEMP\Ratchet-Coop-Mod-sync.pkg"
& "$RPCS3\rpcs3.exe" --installpkg "$env:TEMP\Ratchet-Coop-Mod-sync.pkg"
```

Close RPCS3 when the install finishes. This also updates an earlier version, or replaces the official mod.
Check: `Test-Path "$RPCS3\dev_hdd0\game\BORD00001\USRDIR\mod.sprx"`.

## 5. Networking on in RPCS3 (both)

In `$RPCS3\config\config.yml`, under `Net:`, set `Internet enabled: Connected` (RPCS3 closed):

```powershell
$config = "$RPCS3\config\config.yml"
$text = [IO.File]::ReadAllText($config) -replace '(?m)^(\s+Internet enabled:)[^\r\n]*', '$1 Connected'
[IO.File]::WriteAllText($config, $text)   # UTF-8 without a byte order mark, line endings kept
Select-String -Path $config -Pattern 'Internet enabled'
```

Without this the mod can't reach any server.

## 6. Controller (both)

The user sets up their controller in RPCS3 under **Configuration → Pads**: DualShock 4 or DualSense handler for a
PlayStation controller, XInput for an Xbox one. If the game says "Please connect controller number 1", the controller
is off or isn't the one configured there.

## 7. Tailscale (both)

Tailscale puts both PCs on a private network, so the server isn't open to the internet and no router changes are
needed.

1. `winget install --id Tailscale.Tailscale -e`, then the user signs in (`tailscale up` opens the browser).
2. The host shares their PC with the guest: in the Tailscale admin console (https://login.tailscale.com/admin/machines),
   **Share** the host's machine and send the guest the link, or invite the guest to the host's tailnet. The guest
   accepts in their browser.
3. Host's IP for the guest: run `tailscale ip -4` on the host (starts with `100.`).
4. Check from the guest: `tailscale ping <host IP>`. "via <address>" is a direct connection; "via DERP" works but
   with more lag.

## 8. The co-op server (host only)

1. Tools to build it: Git (`winget install --id Git.Git -e`) and a .NET SDK 8 or newer (`dotnet --list-sdks`; if
   there's none, `winget install --id Microsoft.DotNet.SDK.8 -e`).
2. Build:
   ```powershell
   $SERVER = "$HOME\Emulators\Lawrence"
   git clone https://github.com/adamlbunch/Lawrence "$HOME\src\Lawrence"
   cd "$HOME\src\Lawrence"
   dotnet publish Lawrence -c Release -f net8.0 -r win-x64 --self-contained -p:PublishSingleFile=true -o $SERVER
   ```
   Check: `$SERVER\Lawrence.exe`, `$SERVER\lua54.dll`, `$SERVER\mods\coop` and `$SERVER\runtime` exist.
3. Write `$SERVER\settings.toml` **before the first start** (with `[IO.File]::WriteAllText`, as in step 9). Without
   a server name the server stops at a prompt:
   ```toml
   [Server]
   name = "Ratchet Co-op"
   port = 2407
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
   ```
4. Two launchers for the user, in `$SERVER`:
   - `Start Co-op Server.cmd`:
     ```bat
     @echo off
     title Ratchet ^& Clank Co-op Server
     cd /d "%~dp0"
     echo Co-op server running. Keep this window open while you play. Close it to stop the server.
     "%~dp0Lawrence.exe"
     pause
     ```
   - `Play Co-op.cmd` (put the user's real RPCS3 path in place of `C:\path\to\RPCS3`):
     ```bat
     @echo off
     cd /d "%~dp0"
     tasklist /fi "imagename eq Lawrence.exe" | find /i "Lawrence.exe" >nul
     if errorlevel 1 start "Ratchet Co-op Server" "%~dp0Start Co-op Server.cmd"
     start "" "C:\path\to\RPCS3\rpcs3.exe" "C:\path\to\RPCS3\dev_hdd0\game\BORD00001\USRDIR\EBOOT.BIN"
     ```
5. Firewall: the server listens on UDP and TCP port 2407. The first start makes Windows ask whether to allow
   `Lawrence.exe` on the network, and the user clicks Allow. Or, with the user's OK, add the rule yourself from an
   elevated PowerShell:
   `New-NetFirewallRule -DisplayName "Ratchet Co-op Server" -Direction Inbound -Program "$SERVER\Lawrence.exe" -Action Allow`
6. Start `Start Co-op Server.cmd`. Check: `Select-String -Path "$SERVER\lawrence.log" -Pattern 'Started Lawrence'`.

The campaign is saved in `$SERVER\saves\coop` and continues across restarts.

## 9. Username and server address (optional, both)

The game asks for a username and the server address the first time. To skip that, write
`$RPCS3\dev_hdd0\game\BORD00001\USRDIR\settings.conf` with RPCS3 closed. Then the game connects by itself as soon as
the user presses Start. The game only understands `\n` line endings and no byte order mark, so don't use
`Set-Content` or `Out-File`:

```powershell
$settings = "username=`"TheirName`"`nlast_direct_ip=`"100.x.y.z:2407`"`nautoconnect=true`n"
[IO.File]::WriteAllText("$RPCS3\dev_hdd0\game\BORD00001\USRDIR\settings.conf", $settings)
```

The host uses `127.0.0.1:2407`, the guest the host's Tailscale IP. Usernames must differ between the two players.
The game adds a `userid` line itself. If a `settings.conf` is already there, change only those lines and keep the rest.

## 10. Playing

1. Both: double-click "Ratchet Co-op" on the Desktop (the host's also starts the server). Without it: the host runs
   `Play Co-op.cmd`, the guest `$RPCS3\coop-launcher.ps1`. Starting RPCS3 any other way works too, but then the
   session's log is only kept if the launcher runs before RPCS3 is opened again (see [Crashes](#crashes)).
2. Both press Start. Without `settings.conf`: enter a username, then **Circle → Direct IP**, and enter `127.0.0.1`
   (host) or the host's Tailscale IP (guest).
3. Host: **Circle** to create a lobby, and enter a password or leave it blank. The **Campaign** option continues the
   saved campaign or starts a new one (the old one is kept).
4. Guest: pick the lobby with **Cross**, then **Circle** to get ready. Host: **Circle** to start.
5. Joining a game that's already going: pick the lobby marked "In game", **Cross**, then **Circle**.
6. A cutscene or movie you're both watching (the opening, arriving on a planet together): either player's **Start**
   skips it for both.

## Updating

- Mod: run `install-coop.ps1` again, or repeat step 4, on both PCs. Both players must run the same release.
- Server: run `setup-server.ps1` again with the server stopped. By hand: `git pull` in the clone, then the
  `dotnet publish` command from step 8. A clone still on the old `coop-fixes` branch (it's gone)
  needs `git fetch --prune` and `git switch main` first. `settings.toml`, `saves` and the launchers stay.

## Troubleshooting

| Symptom | Cause |
|---|---|
| "Please connect controller number 1" | Controller off, or not the one set up in Configuration → Pads |
| "Ratchet Multiplayer" isn't in the game list | The mod isn't installed (step 4) |
| The mod starts but the game won't | None of the three game IDs is installed (step 3) |
| Can't connect to the server | `Internet enabled: Connected` missing (step 5); server not running; firewall (step 8.5); wrong IP or port; `tailscale ping` fails |
| Server window waits for "Server Name" | `settings.toml` has no `name` (step 8.3) |
| The server says the mod is out of date | That player has the official mod or an older release: repeat step 4 |

### Crashes

RPCS3 rewrites `$RPCS3\log\RPCS3.log` every time it starts, so a crash's log is lost once RPCS3 is opened again.
`coop-launcher.ps1` keeps it: when RPCS3 closes, it zips the session's `RPCS3.log` and `TTY.log` with a
`session.txt` summary into `$RPCS3\coop-logs\<date>_<time>.zip`. The name ends in `-crash` if the game hit a fatal
error (an RPCS3 log line starting `·F`) or RPCS3 didn't exit cleanly. It keeps the newest 30. A session RPCS3 was
opened for some other way is saved the next time the launcher or `install-coop.ps1` runs, as long as RPCS3 wasn't
opened again in between. Errors of the launcher itself go to `coop-logs\launcher-errors.txt`.

After a crash, close RPCS3 (it stays open on the error) and send the newest `-crash` zip to whoever maintains the mod,
along with what was happening: planet, what the players were doing, and whether the other player's game crashed too.
If the game was started without the launcher, save its log before opening RPCS3 again:
`powershell -ExecutionPolicy Bypass -File "$RPCS3\coop-launcher.ps1" -SaveOnly`.

## Linux and macOS

Untested. RPCS3 has Linux (AppImage) and macOS builds at https://rpcs3.net/download. Its files are in
`~/.config/rpcs3` on Linux and `~/Library/Application Support/rpcs3` on macOS instead of next to the program, and
`--installfw` and `--installpkg` work the same. The server is a .NET 8 program and may build with `-r linux-x64` or
`-r osx-arm64`, but it needs a matching native Lua 5.4 library in place of `lua54.dll`.

## About this repository

It started as a fork of [bordplate/rac1-multiplayer](https://github.com/bordplate/rac1-multiplayer), the multiplayer
mod built on [PS3-CKit](https://github.com/tge-was-taken/ps3-ckit) by TGE, and is now its own co-op project. The
original as it was when forked is on the `upstream-main` branch. To change or build the mod, read
[AGENTS.md](AGENTS.md). License: AGPL-3.0 ([LICENSE](LICENSE)).
