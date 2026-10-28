# Nexus Mods page

New versions are uploaded automatically by the Release workflow (see
[BUILDING.md](BUILDING.md#releasing)); this page text is for the first upload and for
editing the description. Manual upload: `EnshroudedBuildingTools-<version>.zip` from the
GitHub release as the main file.
The zip root is the game folder, so Vortex and manual installs put `dinput8.dll` next to
`enshrouded.exe`.

Suggested settings: category *Building*, requirement *none*, single player only,
virus-scan friendly (no installer, no executable).

## Summary (short description)

Replace blocks with another material, build and remove whole lines, floors, walls,
boxes and hollow rooms in one go, and place blocks in midair, all with correct material
costs, refunds and undo.

## Description (BBCode)

```
[size=5][b]Enshrouded Building Tools[/b][/size]
Extends the Construction Hammer. Single player only.

[b]Replace[/b] (F7)
Aim at existing blocks and click: they take your current material, also a terrain
material. Three styles (F9): full shape, overlap only (repaints just what is there,
keeps its exact form), or connected: a fill bucket that repaints every touching block of
the clicked material (up to FillLimit, default 500), leaving other materials alone.
Roof materials also spread diagonally, so a whole sloped roof is caught in one click.
The old material is refunded with the normal item pop-up, and undo puts everything
back exactly. With a terrain material selected it also repaints the ground, for
example dirt into stone.
The fill bucket works with the small shapes (first row of the shape picker); bigger
shapes replace or delete one whole shape, and building pieces work as in the game.
With the connected style, a right click deletes instead: every touching block of the
clicked material is removed and refunded, everything else stays (blocks, roofs, and
terrain while a terrain material is selected).

[b]Area mode[/b] (F6)
Click two opposite corners: the selected shape is repeated over a line, floor, wall
or box. Left click builds (or replaces with Replace on), right click removes.
Area remove (and connected delete) cuts building pieces such as foundations like blocks.
Fill (F11): solid, hollow room (walls + floor + ceiling) or walls only.
Floating text below the crosshair shows the live size and block count before you confirm.

[b]Free placement[/b] (F10)
The block ghost hovers in front of you instead of sticking to surfaces.
Hold Q and turn the mouse wheel to move it closer or farther.

[b]Creative building[/b] (optional)
CreativeBuilding=1 in BuildingTools.ini: blocks, roofs, terrain and prefabs cost nothing,
removing or undoing gives nothing back, so your inventory is never touched. The material
picker ("backpack and magic chests" order) lists every material, also ones you have none
of; favourites and the other orders stay normal. Props, chests and crafting are unchanged.

[b]Magic chests of all bases[/b] (optional)
GlobalMagicChests=1 in BuildingTools.ini: building and crafting take materials from the
magic chests of all your flame altar areas, not only the one you stand in, also outside a
base. The material picker, the missing-materials ghost and the crafting menus count them
too. The loot deposit stays local. Chests of a base the game has not loaded (far away) may
not count.

[b]Menu in the game's own UI[/b] (Home)
A full-screen menu that looks like the game's: Keys tab (click a key, press the new one,
Shift/Ctrl/Alt combinations work) and Options tab. While you build, the mod's keys sit in
the game's building box with the game's key icons and on/off switches.

[b]In your language[/b]
The mod's texts follow the game's language: English, Deutsch, Français, Italiano, 日本語,
한국어, Polski, Português (Brasil), Русский, Español, ไทย, Türkçe, Українська, 简体中文 and 繁體中文.
English and Polish are checked; the other translations are drafts and may contain
mistakes, sorry! Please
help: corrections are very welcome in the comments or on GitHub
([url=https://github.com/AleksanderWojtal-CC/Enshrouded-BuildingTools/blob/main/TRANSLATING.md]how to translate[/url]).

[b]Details[/b]
[list]
[*]Costs, refunds and undo go through the game's own building code.
[*]Game UI sounds, keys in the game's building box, info as floating text like the game's hints.
[*]F5 turns the whole mod off and on (vanilla game while off).
[*]All keys and options in the menu (Home) and in BuildingTools.ini.
[*]Big areas: the game's undo memory is small, so undo covers the first ~30 blocks
of a large area; use an area remove for the rest.
[/list]

[b]Install[/b]
Extract the zip into the game folder (next to enshrouded.exe), with the
BuildingTools-Languages folder.
Uninstall: delete dinput8.dll and BuildingTools-Languages.

[b]Compatibility[/b]
Built for the 1.0 client (2026-10-20). After a game update the mod disables itself
safely if the game code changed; check BuildingTools.log.
Not for dedicated/online servers (the server does not run the mod).

[b]With Creative Mode / XHL mods[/b]
They also ship a dinput8.dll. Keep the XHL dinput8.dll, rename ours to BuildingTools.dll
and Creative Mode's to CreativeModeLoader.dll, and create dinput8-chain.ini:
[code][Chain]
CreativeMode=CreativeModeLoader.dll
BuildingTools=BuildingTools.dll[/code]
Works next to XHL mods such as XHL Vein Mining (0.20.2 and later).

[b]Found a bug?[/b]
Please report it here, with your BuildingTools.log attached:
[url=https://github.com/AleksanderWojtal-CC/Enshrouded-BuildingTools/issues/new?template=bug_report.yml]Bug report form[/url]
Ideas are welcome too: [url=https://github.com/AleksanderWojtal-CC/Enshrouded-BuildingTools/issues/new?template=feature_request.yml]Idea form[/url]

Source code and build instructions: https://github.com/AleksanderWojtal-CC/Enshrouded-BuildingTools
```
