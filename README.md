# Ratchet & Clank Co-op

![Ratchet Multiplayer logo](https://raw.githubusercontent.com/bordplate/rac1-multiplayer/main/app/LOGO.png)  
Logo courtesy of Discord user \_triippy\_

Play the whole campaign of the original 2002 Ratchet & Clank (the PS3 HD release) together with a friend online, in
the RPCS3 emulator. Weapons, gadgets, bolts, planets, gold bolts and skill points are shared, and each of you sees the
other player, their weapons, the enemies and the world exactly as they are in the other's game. Progress saves on the
host's PC.

**[⬇ Download the latest version](https://github.com/adamlbunch/rac1-multiplayer/releases/latest/download/Ratchet-Coop-Mod-sync.pkg)**
· [All releases](https://github.com/adamlbunch/rac1-multiplayer/releases)
· [Setup guide for AI agents](AGENTS.md)

## Quick start

**You each need:** a Windows PC with [RPCS3](https://rpcs3.net) and the PS3 system software (RPCS3: File → Install
Firmware, with `PS3UPDAT.PUP` from [Sony](https://www.playstation.com/support/hardware/ps3/system-software/)), and
your own copy of the PS3 Ratchet & Clank: the European PSN version (NPEA00385), the US PSN version (NPUA80643) or the
European Trilogy disc (BCES01503). The PS2 version doesn't work.

1. **Both players** paste this into PowerShell:
   ```powershell
   irm https://raw.githubusercontent.com/adamlbunch/rac1-multiplayer/main/setup/install-coop.ps1 -OutFile $env:TEMP\install-coop.ps1; powershell -ExecutionPolicy Bypass -File $env:TEMP\install-coop.ps1
   ```
   It finds RPCS3 (or downloads it), checks the system software and the game, and installs the latest version of the
   mod (click **Install** when RPCS3 asks). It also turns RPCS3's networking on and saves your name and the server
   address. To do it by hand instead: download the `.pkg` above, install it with **File → Install Packages/Raps/Edats**,
   and set **Configuration → Network → Network Status** to **Connected**.
2. **The host** also pastes this:
   ```powershell
   irm https://raw.githubusercontent.com/adamlbunch/rac1-multiplayer/main/setup/setup-server.ps1 -OutFile $env:TEMP\setup-server.ps1; powershell -ExecutionPolicy Bypass -File $env:TEMP\setup-server.ps1
   ```
   It builds the co-op server ([adamlbunch/Lawrence](https://github.com/adamlbunch/Lawrence/tree/coop-fixes)) and puts
   a **Ratchet Co-op** shortcut on the Desktop that starts the server and the game.
3. **Connect the two PCs** with [Tailscale](https://tailscale.com) (free): both install it and sign in, and the host
   shares their PC with the guest from the [admin console](https://login.tailscale.com/admin/machines). The guest uses
   the host's Tailscale IP (`tailscale ip -4` on the host, starts with `100.`).
4. **Play.** The host double-clicks **Ratchet Co-op**, presses Start, connects to `127.0.0.1` (**Circle → Direct IP**)
   and presses **Circle** to make a lobby. The guest starts "Ratchet Multiplayer" in RPCS3, connects to the host's
   Tailscale IP, picks the lobby with **Cross** and presses **Circle**. The host presses **Circle** to start.

Run the same commands again to update. You both need the same version.

**Setting up with an AI agent?** Give it this repository's [AGENTS.md](AGENTS.md): every step, with checks, so it can
set everything up for you end to end.

## About this project

It started as a fork of [bordplate/rac1-multiplayer](https://github.com/bordplate/rac1-multiplayer), a multiplayer mod
that runs on console and emulator thanks to [PS3-CKit](https://github.com/tge-was-taken/ps3-ckit) by TGE, and its
server [bordplate/Lawrence](https://github.com/bordplate/Lawrence). The official mod connects to this project's server
but doesn't sync the game the same way, so both players need this version. The original project as it was when we
forked it is on the `upstream-main` branch. The rest of this README describes how the mod is built, from the original.

Every push to `main` builds the mod with GitHub Actions (the `.pkg` is kept with the run). Pushing a version tag
(`git tag v1.2.3 && git push origin v1.2.3`) also publishes it as a release, which becomes the download above.

# Structure
## Launcher `/app/`
This is the main executable for the game launcher, it only spawns our modded EBOOT. 

## Loader `/loader/`
The loader consists out of handwritten assembly to load the mod.sprx from the game directory. When building, we also 
decrypt, patch and resign a copy of the original game code such that we have a static binary with our loader code. 

## PRX module `/prx/`
The PRX module is loaded by the injected loader into the game process. From that point on the PRX loader will communicate
with the ELF loader to share addresses with each other, allowing both to interoperate. The sharing of addresses is how the
bulk of data, including code, can be moved to the SPRX where code space is less of a concern than it is in the ELF. The 
PRX module loads its own configuration and runs its submodules accordingly. Submodules have the ability to hook functions
and insert custom functionality however they see fit.  

This module contains all the multiplayer logic in the game.

## The build scripts
The build scripts are activated through Makefiles in the respective directories, but the bulk of the work is done in the tools placed in the tools directory. Those tools are used to generate the build files (assembly, linkerscripts and makefiles) used to generate the code needer for the ELF loader, PRX loader and function hooks.

The build is configured through 2 files:
- The userconfig.mk for user settings such as paths
- The game config (config_RC1.mk) for game target specific settings.

In userconfig.mk are defined:
- The paths to locations used for building, such as the RPCS3 installation directory
- The target game ID, version and serial.

The config_*.mk file defines the game specific addresses used to build the loader and PRX module.

# Building
I strongly suggest you have a look at how the building works in practice by looking at the 
[Github Action workflow](.github/workflows/build.yml) in this repository.  

To build this project on Windows 10, you need:
- chocolatey
- make, installed through ``chocolatey install make``
- Python >3.11, available through ``py -3``
- PS3 SDK compilers (check GitHub Action workflow mentioned above)
- PS3_SDK environment variable that points to the root of the PS3 SDK
- Cygwin/MinGW/WSL to access Linux tools such as ``sed`` and ``awk``. Must be available in the environment path.
- RPCS3
- text editor of choice, such as vscode
- a copy of Ratchet & Clank (NPEA00385) installed in RPCS3 in `{RPCS3_INSTALL_DIR}/dev_hdd0/game/NPEA00385`
- bit of luck
- coffee


### Base patches
This base patch provides the loader with the needed import functions to load the SPRX module at game boot. And including
some patches to change behavior of some library calls that determine the game content paths for the game. Only the rc1.txt
patch file is actually applied, but the "legacy" yaml file contains comments. 
The base patches can be found in the 'basepatches' folder.

## Setup
To perform the build, execute:
```
make
```

If all goes well there shouldn't be any errors, and a .pkg installer file should be created in the `/build/` folder, including
a `/build/PS3_GAME/USRDIR` folder with the app launcher that you can start directly from RPCS3. Subsequent builds update
the `PS3_GAME` folder and when you restart the game in RPCS3, the updates should be applied.