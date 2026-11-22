# player: installing games built on SwARc

What a game's players need, shared by every game repository that uses SwARc: the one-line installer, the build
kit it downloads, the script that builds the game on the player's Mac, and an end-to-end test in a macOS VM. A game
only describes itself, in `recomplauncher.json` at its repository's root.

| File | What it is |
|---|---|
| `kit.py build` | Makes `dist/<id>-kit-macos-arm64.tar.gz` in the game's repo: everything needed to build the game except the game itself (the bundled libraries, the lifter, the game's non-lifted objects, headers, CMake's flags, manifests, `game.env`, `build.sh`) |
| `recomp.sh` | The game repo's developer commands, run as `./recomp` there (below) |
| `kit.py sync [--check]` | Writes the game's `install.sh`, `recomp` and `.github/workflows/kit.yml` from `templates/` (`--check`: fails if they're stale; CI runs it) |
| `kit.py env` | The game's settings as bash variables, for `recomp.sh` |
| `player.sh` | Shipped in the kit and installed in the app's `Contents/Resources`: the app menu's actions and the update check at launch (below) |
| `build.sh` | The player's build, shipped in the kit: checks the Mac, installs Apple's command line tools, finds the game copy (Wine bottles, CrossOver, Whisky, Kegworks, Spotlight) or downloads it from Steam with DepotDownloader and a QR sign-in, checks its hashes, asks for files the player supplies, lifts, compiles with Apple's clang and installs the app |
| `vm-test.sh REPO...` | The whole player install, then the game's smoke cases on the installed app, then `vm-app-check.sh`, in throwaway tart VMs (deleted afterwards) |
| `vm-app-check.sh` | Run in the VM by `vm-test.sh`: the app's update check, menu actions and uninstall |
| `run-installed.sh` | The run script `tools/smoke.py --run` uses to run an installed app |

Players run the game's `install.sh` (`curl -fsSL https://raw.githubusercontent.com/<repo>/main/install.sh | bash`),
which downloads the latest release's kit to `~/Library/Application Support/<id>/kit` and runs its `build.sh`. The
app goes in `/Applications`, or `~/Applications` when this account can't write there (or wherever it was before).
Its executable is a starter script, `Contents/MacOS/launcher`, that passes the game folder the install saved
(`~/Library/Application Support/<id>/game-dir`) as `--game-dir`. Logs: `~/Library/Logs/<id>-build.log` (the
build), `~/Library/Logs/<id>.log` (the game, when started from Finder; the run before's is `<id>.log.1`).

## A game repo's commands: ./recomp

Every game repo has the same `./recomp` (generated from `templates/recomp`). It gets SwARc into
`external/SwARc` if it's missing, then runs that checkout's `player/recomp.sh`, so the commands come with the
SwARc commit the game pins, and changing them here reaches every game at its next pin.

| Command | What it does |
|---|---|
| `./recomp setup` | Installs the Homebrew packages the game's `kit.brew` lists that are missing, then `sync` |
| `./recomp status` | Whether SwARc is the pinned commit, the generated files are current, the kit in `dist/` is this checkout's, and which build is installed |
| `./recomp sync` | After pulling: checks out the pinned SwARc, and rewrites the generated files if SwARc's templates changed |
| `./recomp kit [--force]` | Builds the kit in `dist/`, only when it isn't this checkout's: its `VERSION` names the game commit and SwARc commit it was built from, and whether either had uncommitted changes |
| `./recomp install [ARGS]` | `kit` if needed, then the game's `install.sh` with that kit, as a player would install (`ARGS` go to `build.sh`: `--no-launch`, `--game-dir DIR`) |
| `./recomp test [ARGS]` | `kit` if needed, then `vm-test.sh` on this repo (`--keep-base`, `--keep`, `--update`, `--steam`) |
| `./recomp uninstall` | `install.sh --uninstall` |
| `./recomp pin [REF]` | Checks out SwARc's `REF` (default `origin/main`; it must be on the fork), pins it in `tools/swarc.sh`, then `sync`. Commit `tools/swarc.sh` and the generated files together |

`kit`, `install` and `test` stop when `external/SwARc` isn't the pinned commit or has uncommitted changes, since
the kit would then be built from something the game doesn't pin; `--allow-unpinned` goes ahead anyway, for trying
a SwARc change before landing it.

## The installed app

Everything the game writes goes in `~/Library/Application Support/<id>/` (the runtime's `DataDir()`): the guest's
writes to its install folder in `install/`, its other files (My Documents, AppData) in `drive_c/`, the registry in
`registry.txt`, crash and hang reports. Neither the app nor the kit holds any of it, so updating and uninstalling
leave the saved games alone. The same folder has `kit/`, `game-dir`, and the game copy when it came from Steam.

The app's icon is the one Windows shows for the game's program: `build.sh` extracts it from the player's copy
with `tools/pe_icon.py` (the first icon group; 1–32-bit and PNG images; sizes the program lacks are scaled from
its largest, by repeating pixels, so pixel art stays sharp). The name players see is `display_name`; the file is
`<app>.app`, with the same name for the game's binary inside it, so its process shows that name too.

In an app with `Contents/Resources/player.sh` (an installed one; not a developer's build), SwARc's runtime adds to
the macOS app menu, after About:

| Item | `player.sh` | What it does |
|---|---|---|
| Check for Updates… | `check PID` | Asks GitHub how this build's game commit (the kit's `VERSION`) compares with the latest release; if the release is newer, offers to update: quits the game and reruns `install.sh` in Terminal |
| Show Saved Games | `saves` | Opens `~/Library/Application Support/<id>/` in Finder |
| Show Logs | `logs` | Shows the game's log (else the build's) in Finder |
| Uninstall <app>… | `uninstall PID` | Quits the game, deletes the app, the kit and the build cache; asks whether to delete a game copy downloaded from Steam; keeps the saved games |

Before the game starts, the app's starter runs `player.sh launch`: once a day it asks GitHub too, and if a newer
release is out offers to update (Later starts the game). Runs with `<ID>_BACKGROUND`, `<ID>_HEADLESS` or
`<ID>_NO_UPDATE_CHECK=1` skip it. The release API needs the repository to be public. From Terminal, the installer
uninstalls as well: `curl -fsSL https://raw.githubusercontent.com/<repo>/main/install.sh | bash -s -- --uninstall`.

## What a game needs

- `recomplauncher.json` (below).
- CMake targets `<binary>` (the game), `<name>_host` (its non-lifted code) and `<name>_kit_parts` (depends on
  `<name>_host`, `recomp_lift` and the libraries the game links), where `<name>` is `cmake_name` (default: `binary`),
  and a `<ID>_GENERATED_DIR` cache variable naming the lifted code's folder. `<ID>` is the id in upper case.
- `cmake/Info.plist.in`, used as it is (no CMake variables in it). Its `CFBundleIdentifier` tells an install
  of this game from another app of the same name, and finds an install under an older name (which the new one
  replaces). `build.sh` sets the names in it from `app` and `display_name`.
- The game binary takes `--game-dir DIR`.
- `tools/swarc.sh setup` (CI uses it to fetch SwARc) and `smoke/cases.json` (for `vm-test.sh`).

## recomplauncher.json

A [RecompLauncher](https://github.com/elripalda/RecompLauncher) profile; SwARc reads `name`, `game.check.file`
(the file the search looks for) and its own `x-swarc` block:

```json
{
  "format": 1,
  "name": "Soldiers: Heroes of World War II",
  "game": { "kind": "folder", "check": { "file": "Soldiers.exe" } },
  "start": { "macos": { "program": ["Soldiers - Heroes of WWII.app"] } },
  "x-swarc": {
    "id": "soldiers",                  // folders, logs, env prefix (SOLDIERS_), kit name
    "app": "Soldiers - Heroes of WWII",        // the app's file name (no colons), and its game binary's
    "display_name": "Soldiers: Heroes of WWII", // optional: the name players see (menu bar, Dock, Finder; default: app)
    "binary": "soldiers",              // CMake target (the developer build's executable)
    "icon": { "file": "Soldiers.exe", "group": 1 },  // optional: whose icon (default: the first .exe in files)
    "cmake_name": "soldiers",          // optional: <cmake_name>_host and _kit_parts (default: binary)
    "repo": "namuol/soldiers-heroes-of-wwii-recomp",
    "steam": { "appid": 11480, "depot": 11481, "manifest": "<depot manifest ID>", "build": "21022753",
               // or "depots": [{ "depot": 2229851, "manifest": "..." }, ...]: all of them, into one folder
               "title": "<name on Steam>", "folder": "<download folder name>", "size": "1.7 GB" },
    "files": ["Soldiers.exe", "resource.pak"],     // must exist in a game copy ("Dir/" for a folder)
    "hashes": { "Soldiers.exe": "<sha-256>" },     // the Supported build: every one must match
    "other_versions": "Other versions (retail CD, GOG, older patches) don't work.",
    "supplied": [{ "file": "Game.unpacked.exe", "sha256": "<sha-256>", "about": "<what it is, for the player>" }],
    "lift": { "args": ["--sweep-gaps"],
              "images": [{ "file": "Soldiers.exe", "manifest": "soldiers.manifest" },
                         { "file": "some.dll", "base": "61000000" },   // first is --exe, the rest --module
                         { "file": "msvcp140.dll", "base": "185000000",
                           "redist": "_CommonRedist/vcredist/2022/VC_redist.x64.exe" }] },
    "disk_gb": 5,
    "build_time": "a few minutes",
    "kit": { "brew": ["cmake", "ninja", "..."], "cmake_args": [], "require_libs": [], "runner": "macos-15" }
  }
}
```

`supplied` files (for executables whose Steam copy has encrypted code) are looked for in `--supply` paths,
`$<ID>_EXE` and the game folder, then asked for; they're copied into the game folder, where the game loads them.
Images without a `manifest` get an empty one. An image with `redist` is a runtime DLL taken from the Visual C++
redistributable at that path in the game copy (which `hashes` must pin), with `tools/vcredist.py`; the app
carries the extracted DLL in `Contents/Resources`, where the game's `LiftedDll::host_path` should find it.

## Testing

```bash
./recomp test                                       # in the game's repo: the kit if needed, then vm-test.sh
external/SwARc/player/vm-test.sh . ../other-game    # several repos, one after another (kits built first)
```

`vm-test.sh` needs [tart](https://tart.run) and the game copy on this Mac. The first run makes the base VM
(Cirrus Labs' vanilla macOS, a 25 GB download, plus Apple's command line tools) and deletes it at the end with
tart's cache; `--keep-base` keeps them for the next run.
