# player: installing games built on SwARc

What a game's players need, shared by every game repository that uses SwARc: the one-line installer, the build
kit it downloads, the script that builds the game on the player's Mac, and an end-to-end test in a macOS VM. A game
only describes itself, in `recomplauncher.json` at its repository's root.

| File | What it is |
|---|---|
| `kit.py build` | Makes `dist/<id>-kit-macos-arm64.tar.gz` in the game's repo: everything needed to build the game except the game itself (the bundled libraries, the lifter, the game's non-lifted objects, headers, CMake's flags, manifests, `game.env`, `build.sh`) |
| `kit.py sync [--check]` | Writes the game's `install.sh` and `.github/workflows/kit.yml` from `templates/` (`--check`: fails if they're stale; CI runs it) |
| `build.sh` | The player's build, shipped in the kit: checks the Mac, installs Apple's command line tools, finds the game copy (Wine bottles, CrossOver, Whisky, Kegworks, Spotlight) or downloads it from Steam with DepotDownloader and a QR sign-in, checks its hashes, asks for files the player supplies, lifts, compiles with Apple's clang and installs the app |
| `vm-test.sh REPO...` | The whole player install, then the game's smoke cases on the installed app, in throwaway tart VMs (deleted afterwards) |
| `run-installed.sh` | The run script `tools/smoke.py --run` uses to run an installed app |

Players run the game's `install.sh` (`curl -fsSL https://raw.githubusercontent.com/<repo>/main/install.sh | bash`),
which downloads the latest release's kit to `~/Library/Application Support/<id>/kit` and runs its `build.sh`. The
app goes in `/Applications`, or `~/Applications` when this account can't write there (or wherever it was before).
Its executable is a starter script, `Contents/MacOS/launcher`, that passes the game folder the install saved
(`~/Library/Application Support/<id>/game-dir`) as `--game-dir`. Log: `~/Library/Logs/<id>-build.log`.

## What a game needs

- `recomplauncher.json` (below).
- CMake targets `<binary>` (the game), `<binary>_host` (its non-lifted code) and `<binary>_kit_parts` (depends on
  `<binary>_host`, `recomp_lift` and the libraries the game links), and a `<ID>_GENERATED_DIR` cache variable naming
  the lifted code's folder. `<ID>` is the id in upper case.
- `cmake/Info.plist.in` with `CFBundleIdentifier` `local.<id>-recomp`.
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
  "start": { "macos": { "program": ["Soldiers.app"] } },
  "x-swarc": {
    "id": "soldiers",                  // folders, logs, env prefix (SOLDIERS_), kit name
    "app": "Soldiers",                 // Soldiers.app
    "binary": "soldiers",              // CMake target and executable
    "repo": "namuol/soldiers-heroes-of-wwii-recomp",
    "steam": { "appid": 11480, "depot": 11481, "manifest": "<depot manifest ID>", "build": "21022753",
               "title": "<name on Steam>", "folder": "<download folder name>", "size": "1.7 GB" },
    "files": ["Soldiers.exe", "resource.pak"],     // must exist in a game copy ("Dir/" for a folder)
    "hashes": { "Soldiers.exe": "<sha-256>" },     // the Supported build: every one must match
    "other_versions": "Other versions (retail CD, GOG, older patches) don't work.",
    "supplied": [{ "file": "Game.unpacked.exe", "sha256": "<sha-256>", "about": "<what it is, for the player>" }],
    "lift": { "args": ["--sweep-gaps"],
              "images": [{ "file": "Soldiers.exe", "manifest": "soldiers.manifest" },
                         { "file": "some.dll", "base": "61000000" }] },   // first is --exe, the rest --module
    "disk_gb": 5,
    "build_time": "a few minutes",
    "kit": { "brew": ["cmake", "ninja", "..."], "cmake_args": [], "require_libs": [], "runner": "macos-15" }
  }
}
```

`supplied` files (for executables whose Steam copy has encrypted code) are looked for in `--supply` paths,
`$<ID>_EXE` and the game folder, then asked for; they're copied into the game folder, where the game loads them.
Images without a `manifest` get an empty one.

## Testing

```bash
python3 -I external/SwARc/player/kit.py build      # in the game's repo
external/SwARc/player/vm-test.sh .                  # or several repos, one after another
```

`vm-test.sh` needs [tart](https://tart.run) and the game copy on this Mac. The first run makes the base VM
(Cirrus Labs' vanilla macOS, a 25 GB download, plus Apple's command line tools) and deletes it at the end with
tart's cache; `--keep-base` keeps them for the next run.
