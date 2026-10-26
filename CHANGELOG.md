# Changelog

Each `## x.y.z` section is used as the release notes on GitHub and as the file
changelog on Nexus Mods, so keep it short and written for players.

## 0.17.2

- Fix (magic chests of all bases): crafting at a bench in a base without any chests of
  its own (or outside every base) only used the backpack, although the costs showed
  the materials of the other bases. It now uses the chests of every base there too.

## 0.17.1

- Faster and lighter: the info panel no longer re-creates its font, looks up the game
  window and asks Windows for key names twenty times a second; hooks share one small
  block of memory instead of 64 KB each; a few unused game lookups at start-up are gone.
- Fix: connected replace (fill bucket) with a very large `FillLimit` could freeze the
  game for a long time; its search table is now big enough for the maximum limit.
- Fix: the mod no longer refuses to start when one unused piece of game code is not
  found (after a game update).
- The info panel shows the key you set for the free placement wheel (not always Q).
- Cleaner log: debug lines from earlier versions removed.

## 0.17.0

- New: **magic chests of all bases** (`GlobalMagicChests=1` in the new `[Chests]`
  section of `BuildingTools.ini`, off by default). Building and crafting take
  materials from the magic chests of every flame altar area, not only the one you
  stand in, also outside a base. The material picker, the red "missing materials"
  ghost and the crafting menus count them too. Order: backpack, the chests of the
  base you are in, then the other bases. The loot deposit stays local.
- Chests of a base the game has not loaded (far away) may not count.
- `BuildingTools.ini` gets the new `[Chests]` section on the first start; your
  settings are kept.

## 0.16.0

- New: **F5 turns the whole mod off and on** (`ToggleMod` in the ini). Off is the
  vanilla game: Prefab mode, Replace, free placement and creative building are off
  and the other mod keys do nothing until you press it again.
- New: **connected replace (fill bucket)**. F9 now cycles full shape / overlap only /
  connected. With Replace on in Prefab mode, click a block: every touching block of
  the same material is repainted with your current material, in steps of the
  selected shape (0.5 m cube = block-exact), up to `FillLimit` shapes (default 500).
  Other materials are never touched; costs, refunds and undo work as for Replace.
- Fix: Replace with a terrain material as the new material removed the blocks
  instead of repainting them; they now become that terrain.
- `BuildingTools.ini` gets the new options (`ToggleMod`, `FillLimit`) on the first
  start; your settings are kept. If F5 clashes with another key, change `ToggleMod`.

## 0.15.0

- New: **creative building** (`CreativeBuilding=1` in the `[Creative]` section of
  `BuildingTools.ini`). Blocks, roofs, terrain and prefabs cost nothing, removing
  and undoing give nothing back, so the inventory is never touched. Props, chests
  and crafting stay normal.
- In creative building the material picker lists every block, roof, terrain and
  overgrowth material in its "backpack and magic chests" order, also materials
  you have none of. Favourites, last used, amount and item sets stay as they are.
  A material type you have nothing of can be built with right away.
- Works next to Creative Mode and other mods that hook the same game functions:
  the payment function and the dismantle dependency copy are no longer patched
  at their entry.
- Installing next to other mods that ship a `dinput8.dll` (XHL Native Core
  `dinput8-chain.ini`) is described in the README.

## 0.14.1

- The mouse hook is installed only while free placement is on and the game has
  focus (fixes cursor delay and hover/click issues some players saw).

## 0.14.0

- Area preview: the edges of the area being set up light up with the hammer's
  block highlight (on block surfaces; `Preview=0` turns it off).

## 0.13.4

- Works when another mod has already hooked the same game functions
  (fixes "PayCost not found").

## 0.13.3

- Replace with larger shapes in flight mode no longer reports "no live aim".

## 0.13.2

- First public release: Replace, Area mode (solid, hollow room, walls), free
  placement, game sounds, info bar, refund pop-up and undo of Replace.
