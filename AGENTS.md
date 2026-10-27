# Working on this repository: notes for AI agents

Setting the game up for a user? Follow [README.md](README.md) instead. This file is for changing and building the mod.

## Branches and releases

- `main` is the co-op mod. Every push to it builds the mod with GitHub Actions
  ([build.yml](.github/workflows/build.yml)) and keeps the `.pkg`, `USRDIR` and `mod.map` as the run's artifact.
- Pushing a version tag (`git tag v1.2.3 && git push origin v1.2.3`) also publishes that build as a GitHub release.
  It becomes the download that the README and `setup/install-coop.ps1` use
  (`releases/latest/download/Ratchet-Coop-Mod-sync.pkg`), so players get it the next time they update. Releases are
  public: only with the repository owner's OK.
- `upstream-main` is [bordplate/rac1-multiplayer](https://github.com/bordplate/rac1-multiplayer) as it was when this
  was forked. The other branches (`coop`, `cleanup`, `wip` and so on) are the original's.
- The server is [adamlbunch/Lawrence](https://github.com/adamlbunch/Lawrence), branch `main`. The packets the two
  exchange are defined on both sides (`prx/rc1/multiplayer/network/Packet.h` here, `Lawrence/Core/Packet.cs` there),
  so a change to one needs the same change in the other. Both players must run the same release.

## Layout

- `app/`: the launcher executable. It only starts the modded EBOOT.
- `loader/`: handwritten assembly that loads `mod.sprx` from the game folder. The build decrypts, patches and resigns
  a copy of the game's code so it carries this loader.
- `prx/`: `mod.sprx`, which the loader loads into the game. All the multiplayer code is here, the co-op sync in
  `prx/rc1/multiplayer/`. [prx/README.md](prx/README.md) explains its modules and library.
- `tools/`: generate the build files (assembly, linker scripts, makefiles) for the loader and the function hooks.
- `basepatches/`: the base patch that gives the loader its imports and changes the game's content paths.
- `config_RC1.mk`: the game's addresses for the loader and the module. `userconfig.mk`, made from
  `userconfig.template.mk`: local paths and the target game's ID, version and category.
- `setup/`: the setup scripts the README uses.

## Building

The simplest build is GitHub Actions: push to `main`, or run the workflow on another branch
(`gh workflow run build.yml --ref <branch>`), then download the run's artifact. A local build does what the workflow
does, and needs:

- `make` (`choco install make`), Python 3.11 or newer, and Linux tools such as `sed` and `awk` on the PATH (Git Bash,
  Cygwin, MinGW or WSL).
- The PS3 SDK compilers, with the `PS3_SDK` environment variable pointing at the SDK's root. The workflow shows where
  it gets them.
- `userconfig.mk` with `PYTHON` and `RPCS3_DIR`, and the game (`NPEA00385` by default) installed in that RPCS3 at
  `dev_hdd0/game/NPEA00385`.

`make` writes `build/BDUPS3-BORD00001_00-0000000000000000.pkg` and `build/PS3_GAME/USRDIR`, which RPCS3 can boot
directly. Later builds update `PS3_GAME`. Restart the game in RPCS3 to load them.
