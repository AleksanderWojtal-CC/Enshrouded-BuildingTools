# Changelog

Each `## x.y.z` section is used as the release notes on GitHub and as the file
changelog on Nexus Mods, so keep it short and written for players.

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
