# Building

The mod is one Windows DLL, `dinput8.dll`, with no C runtime. It is built with
clang and lld for the `x86_64-w64-mingw32` target. Any OS with clang works; CI builds on
Ubuntu.

## Requirements

| | Linux (Ubuntu/Debian) | Windows |
|---|---|---|
| Compiler | `clang` (16+), `lld` | MSYS2, *CLANG64* shell |
| Windows headers + import libs | `mingw-w64-x86-64-dev` | included in the toolchain |
| Packaging | `zip` | `zip` |

```sh
# Ubuntu / Debian
sudo apt install clang lld mingw-w64-x86-64-dev zip

# Windows: install MSYS2 (https://www.msys2.org), open "MSYS2 CLANG64", then
pacman -S mingw-w64-clang-x86_64-toolchain zip
```

`build.sh` looks for the MinGW-w64 headers in `/usr/x86_64-w64-mingw32`,
`/usr/local/x86_64-w64-mingw32` and `~/winsdk`. Set `WINSDK=/path` to use another
directory containing `include/` and `lib/`. In the MSYS2 CLANG64 shell, nothing needs to be
set.

## Commands

```sh
./build.sh           # build/dinput8.dll
./build.sh test      # build + unit tests
./build.sh package   # build + tests + dist/EnshroudedBuildingTools-<version>.zip
```

The build is reproducible: no timestamps, stripped, so the same sources give the same
DLL.

### Tests

`tests/test_core.cpp` runs on the build machine (host compiler, `HOSTCXX` to override):

- **Planner tests** always run. They cover the area planner, the axis lock, the hollow
  and walls fills, and fixed-point rounding.
- **Game binary checks** run only when the game executable is available:
  `ENSHROUDED_EXE=/path/to/enshrouded.exe ./build.sh test`. They check that every
  signature in `src/main.cpp` matches exactly once at the expected address, and that
  every hook steals position-independent bytes. Run these after a game update. The
  executable is never part of the repository.

## Source layout

| Path | What |
|---|---|
| `src/main.cpp` | The whole mod: proxy exports, config, hooks, building logic, overlay, input |
| `src/core.h` | Platform-free logic (signature scanner, area planner, fills), also used by the tests |
| `src/cave.S` | Replacement code patched into the game's voxel write loop (Replace) |
| `src/chkstk.S` | `___chkstk_ms` (stack probe needed without a C runtime) |
| `src/dinput8.def` | Exports `DirectInput8Create`, forwarded to the real `System32\dinput8.dll` |
| `package/` | Files shipped in the release zip |
| `tests/` | Unit tests |
| `tools/re/` | Python scripts used to analyse the game executable (see its README) |

## Releasing

1. Set the version in `src/main.cpp` (`#define MOD_VERSION "x.y.z"`), add a `## x.y.z`
   section at the top of [CHANGELOG.md](../CHANGELOG.md), and update
   `package/BuildingTools-README.txt` and the docs if features changed.
2. Merge to `main` (pull request). The **Release** workflow sees a `MOD_VERSION` that
   has no GitHub release yet, builds, runs the tests and publishes the release
   `vx.y.z` with `EnshroudedBuildingTools-x.y.z.zip`. The notes are the version's
   changelog section. (Pushing a tag `vx.y.z` or running the workflow by hand does
   the same.)
3. The same workflow then uploads the zip to Nexus Mods as a new version of the
   existing main file, with the changelog section, through
   [Nexus-Mods/upload-action](https://github.com/Nexus-Mods/upload-action). It needs,
   in the repository settings:
   - environment **Release** with the secret `NEXUS_API_KEY` (your Nexus Mods API key),
   - repository variables `NEXUS_MOD_ID` (the number in the mod page URL,
     `nexusmods.com/enshrouded/mods/<id>`) and `NEXUS_FILE_ID` (the main file's id:
     Files tab → the file's download link `...?file_id=<id>`).
   Without the variables the Nexus step is skipped.

Every push and pull request runs the **Build** workflow, which uploads the zip as a
workflow artifact.

## Updating for a new game version

1. Run the binary checks against the new `enshrouded.exe`.
2. For each failing signature, find the function again. Usually the code only moved; the
   context is described in `docs/reverse-engineering-notes.md`.
3. Regenerate the pattern with `tools/re/sigs.py`, update `src/main.cpp` and the expected
   addresses in `tests/test_core.cpp`, then repeat until all checks pass.
