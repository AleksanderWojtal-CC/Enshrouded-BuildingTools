# Enshrouded Building Tools

A client-side mod for **Enshrouded 1.0 (single player)** that extends the Construction Hammer:

- **Replace**: repaint existing blocks with your current material, as a full shape,
  only where the shape overlaps existing blocks, or **connected** (fill bucket: every
  touching block of the clicked material, up to a limit; roofs also diagonally). Old material is refunded, with the game's item pop-up.
  Undo restores everything, including taking the refund back. With a terrain material
  selected it repaints the ground too (for example dirt into stone).
- **Connected delete**: with the fill bucket style, a right click removes every touching
  block (or terrain) of the clicked material and leaves other materials alone. The fill
  bucket uses the small shapes (first row); bigger shapes replace or delete one whole
  shape, and building pieces in the hammer work as in the game.
- **Area mode**: click two opposite corners and the selected shape is repeated over a
  line, floor, wall or box. You can build, replace or remove the area in one action.
  Area remove and connected delete cut building pieces (foundations...) like blocks.
  Fills: solid, hollow room (walls + floor + ceiling) or walls only. A live size readout
  and an in-game edge highlight show what the next click will make.
- **Free placement**: the block ghost hovers in front of you, snapped to the 0.5 m
  building grid, instead of sticking to surfaces. Hold Q and turn the mouse wheel to set
  the distance.
- **Creative building** (optional, `CreativeBuilding=1` in `BuildingTools.ini`): blocks,
  roofs, terrain and prefabs cost nothing and removing or undoing gives nothing back, so
  the inventory is never touched. The material picker's "backpack and magic chests"
  order lists every material, also ones you have none of; favourites and the other
  orders stay normal. Props, crafting and chests work as normal.
- **Magic chests of all bases** (optional, `GlobalMagicChests=1` in `BuildingTools.ini`):
  building and crafting take materials from the magic chests of every flame altar area,
  not only the one you stand in, also outside a base. The material picker, the red
  "missing materials" ghost and the crafting menus count them too. Chests of the base you
  are in are used first; the loot deposit still only fills local chests.
- **Menu in the game's own UI** (Home): change keys (also Shift / Ctrl / Alt combinations)
  and options in a full-screen menu with the game's tabs. While you build, the mod's keys
  sit in the game's building box with the game's key icons and on/off switches, and the
  area size and messages show as floating text below the crosshair.
  Everything is also in `BuildingTools.ini`.
- **In your language**: the mod's texts follow the game's language (all 15 languages of
  the game's language menu). English and Polish are checked; the other translations are drafts that may contain
  mistakes; corrections are very welcome, see [TRANSLATING.md](TRANSLATING.md).

Materials are charged and refunded through the game's own building code, and edits
replicate like vanilla building.

## Install

1. Download `EnshroudedBuildingTools-<version>.zip` from the
   [Releases](../../releases) page.
2. Extract it into the game folder, next to `enshrouded.exe`
   (Steam: right click Enshrouded → Manage → Browse local files).
3. Start the game. The banner "Building Tools <version> ready" confirms it loaded.

The zip also holds the `BuildingTools-Languages` folder with the translations; keep it
next to `dinput8.dll` (without it the mod shows English).

Uninstall by deleting `dinput8.dll` and `BuildingTools-Languages` from the game folder. The full key list and
options are in [`package/BuildingTools-README.txt`](package/BuildingTools-README.txt),
which is also shipped in the zip.

| Key | Action |
|---|---|
| Home | Building Tools menu: change keys and options in the game's own UI |
| F6 | Prefab mode (vanilla) / Area mode |
| F7 | Replace on/off |
| F8 | Clear area corners |
| F9 | Replace style: full shape / overlap only / connected (fill bucket) |
| F10 | Free placement on/off (Q + wheel: distance) |
| F11 | Area fill: solid / hollow room / walls only |
| F5 | Whole mod off / on (off = vanilla game) |

All keys can be changed in the menu (Keys tab), also to Shift, Ctrl or Alt combinations.

What changed in each version: [CHANGELOG.md](CHANGELOG.md).

## Build

```sh
sudo apt install clang lld mingw-w64-x86-64-dev zip   # Ubuntu/Debian
./build.sh            # -> build/dinput8.dll
./build.sh test       # + unit tests
./build.sh package    # + dist/EnshroudedBuildingTools-<version>.zip
```

See [docs/BUILDING.md](docs/BUILDING.md) for Windows (MSYS2), the test options and the
release process. [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) explains the design;
[docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md) is the raw
research log with all game addresses.

## Compatibility

The mod is built against the Enshrouded 1.0 client from 2026-10-20. It finds the game code by
byte signatures. If an update changes that code, the mod turns itself off safely, and
`BuildingTools.log` says which signature was not found. It is single player only.
Online, the server does not run the mod.

**Other mods with a `dinput8.dll`** (Creative Mode, XHL mods): only one file can have
that name. Use the XHL Native Core (the `dinput8.dll` from an XHL mod) and chain the
others with a `dinput8-chain.ini` in the game folder, after renaming this mod's DLL to
`BuildingTools.dll` and Creative Mode's to `CreativeModeLoader.dll`:

```ini
[Chain]
CreativeMode=CreativeModeLoader.dll
BuildingTools=BuildingTools.dll
```

Building Tools redirects the calls to the two functions Creative Mode also hooks
(`PayCost`, `9bc1a0`) at their call sites and leaves their first bytes alone, so the two mods
can start in either order.

## License

MIT, see [LICENSE](LICENSE). This project is not affiliated with Keen Games.
