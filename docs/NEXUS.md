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
The old material is refunded with the normal item pop-up, and undo puts everything
back exactly.

[b]Area mode[/b] (F6)
Click two opposite corners: the selected shape is repeated over a line, floor, wall
or box. Left click builds (or replaces with Replace on), right click removes.
Fill (F11): solid, hollow room (walls + floor + ceiling) or walls only.
The info bar shows the live size and block count before you confirm.

[b]Free placement[/b] (F10)
The block ghost hovers in front of you instead of sticking to surfaces.
Hold Q and turn the mouse wheel to move it closer or farther.

[b]Creative building[/b] (optional)
CreativeBuilding=1 in BuildingTools.ini: blocks, roofs, terrain and prefabs cost nothing,
removing or undoing gives nothing back, so your inventory is never touched. The material
picker ("backpack and magic chests" order) lists every material, also ones you have none
of; favourites and the other orders stay normal. Props, chests and crafting are unchanged.

[b]Details[/b]
[list]
[*]Costs, refunds and undo go through the game's own building code.
[*]Game UI sounds, small info bar at the bottom of the screen, auto-hides.
[*]F5 turns the whole mod off and on (vanilla game while off).
[*]All keys and options in BuildingTools.ini (instructions inside).
[*]Big areas: the game's undo memory is small, so undo covers the first ~30 blocks
of a large area; use an area remove for the rest.
[/list]

[b]Install[/b]
Extract the zip into the game folder (next to enshrouded.exe).
Uninstall: delete dinput8.dll.

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

Source code and build instructions: <GitHub URL>
```
