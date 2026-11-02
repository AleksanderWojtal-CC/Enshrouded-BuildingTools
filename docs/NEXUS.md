# Nexus Mods page

New versions are uploaded automatically by the Release workflow (see
[BUILDING.md](BUILDING.md#releasing)); this page text is for the first upload and for
editing the description. Manual upload: `EnshroudedBuildingTools-<version>.zip` from the
GitHub release as the main file.
The zip root is the game folder, so Vortex and manual installs put `dinput8.dll` next to
`enshrouded.exe`.

Suggested settings: category *Building*, requirement *none*, single player and
dedicated servers,
virus-scan friendly (no installer, no executable).

## Summary (short description)

Replace blocks with another material, build and remove whole lines, floors, walls,
boxes and hollow rooms in one go, and place blocks in midair, all with correct material
costs, refunds and undo.

## Description (BBCode)

```
[size=5][b]Enshrouded Building Tools[/b][/size]
Extends the Construction Hammer. Works in single player and on dedicated servers.

[b]Replace[/b] (F7)
Aim at existing blocks and click: they take your current material, also a terrain
material. Three styles (F9): full shape, overlap only (repaints just what is there,
keeps its exact form), or connected: a fill bucket that repaints every touching block of
the clicked material (up to FillLimit, default 500), leaving other materials alone.
Roof materials also spread diagonally, so a whole sloped roof is caught in one click.
The old material is refunded with the normal item pop-up, and undo puts everything
back exactly. With a terrain material selected it also repaints the ground, for
example dirt into stone. It also works with a free camera far from your character.
The fill bucket works with the small shapes (first row of the shape picker); bigger
shapes replace or delete one whole shape, and building pieces work as in the game.
With the connected style, a right click deletes instead: every touching block of the
clicked material is removed and refunded, everything else stays (blocks, roofs, and
terrain while a terrain material is selected).

[b]Area mode[/b] (F6)
Click two opposite corners: the selected shape is repeated over a line, floor, wall
or box. Left click builds (or replaces with Replace on), right click removes.
Area remove (and connected delete) cuts building pieces such as foundations like blocks.
Fill (F11): solid, hollow room (walls + floor + ceiling), walls only, roof, round or diagonal;
rooms, walls, roofs, round and diagonal walls 1-3 shapes thick.
[b]Roofs:[/b] gable, hip, gambrel, mansard, shed, conical and dome (Shift+R cycles, R turns). Conical
and dome roofs are round: they cover the area's circle or ellipse and sit on round walls. Mark a flat area
and the roof covers it, one shape up for every shape in from the edge; mark a 3D area and the
box height sets the roof height. Overhang up to 3 shapes.
[b]Round walls:[/b] full circle, half or quarter (Shift+R cycles, R turns), solid or hollow. The
two corners bound the shape: a square gives a circle, a rectangle an ellipse. Standing (towers,
arches in a thin wall) or lying (tubes and tunnels along the area).
[b]Diagonal walls:[/b] standing, a wall from corner 1 to corner 2 seen from above; or lying, a ramp
from corner to corner (45 degrees over a flat area). R turns it. No gaps along the steps.
Floating text below the crosshair shows the live size and block count before you confirm.
A see-through 3D box is drawn over the area while you set it up (dimmer where terrain or
blocks hide it, red while you stand inside), with the blocks the action will touch shown
inside it: blue to build, amber to replace, pink-red to remove (only blocks that exist).

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

[b]Tools wheel[/b] (hold Caps Lock)
A blurred wheel in the middle of the screen, drawn with the game's own wheel: point the
mouse at an option and let go (or click to change several). It shows only what applies
right now: Replace style while Replace is on, area fill and clear corners in Area mode.

[b]Menu in the game's own UI[/b] (Home)
A full-screen menu that looks like the game's: Keys tab (click a key, press the new one,
Shift/Ctrl/Alt combinations and mouse side buttons work), Quick keys section on the scrolling Keys tab (keys that set one
option directly) and Options tab. While you build, the mod's keys sit in
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

[b]Install (single player)[/b]
Extract the zip into the game folder (next to enshrouded.exe), with the
BuildingTools-Languages folder.
Uninstall: delete dinput8.dll and BuildingTools-Languages.

[b]Install on a dedicated server[/b]
[list=1]
[*]Install the mod in your game as above (every player who wants the tools does this).
[*]Extract the same zip into the server folder too, next to enshrouded_server.exe
(Steam: Enshrouded Dedicated Server, Manage, Browse local files; or your host's file manager).
[*]Restart the server. BuildingTools.log next to enshrouded_server.exe should say
"dedicated server: every player builds with the settings of their own Building Tools".
[/list]
Each player builds with their own keys and settings (Area, Replace, styles, fill); players
without the mod build as usual. The server's BuildingTools.ini (created on its first start)
decides creative building, chests of all bases, MaxShapes and FillLimit for everyone.
Server and players should run the same version of the mod.

[b]Compatibility[/b]
Built for Patch #1 (1.0.1.0, 2026-11-02); use 0.24.0 or older only with the original 1.0 client. After a game update the mod disables itself
safely if the game code changed; check BuildingTools.log.
Multiplayer: dedicated servers are supported (see Install). In a game hosted by a player
(not a dedicated server) the mod runs in the host's game, and per-player settings are not
supported yet.

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
