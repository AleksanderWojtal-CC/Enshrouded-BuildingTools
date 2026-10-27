# Changelog

Each `## x.y.z` section is used as the release notes on GitHub and as the file
changelog on Nexus Mods, so keep it short and written for players.

## 0.20.0

- New: **the mod speaks your language.** Menu, building box rows, floating text, info bar
  and messages follow the game's language, in all 15 languages of the game's language
  menu. Translations other than English are drafts and may contain mistakes, sorry!
  Corrections are very welcome on Nexus or GitHub (see TRANSLATING.md).
- The translations are plain text files in the new `BuildingTools-Languages` folder next
  to the mod, so anyone can fix a word and see it in the game.
- New option **Language** (Options tab, or `Language=` in the ini): follow the game, or
  pick one by hand.
- Key names and distances use the language's own form (for example `Strg`, `4,0 m`).

## 0.19.0

- New: **connected delete.** With Replace on and the style set to connected (fill
  bucket), a right click removes every touching block of the clicked material, up to
  FillLimit. Other materials in the same spots stay. Works on blocks and roofs, and on
  terrain while a terrain material is selected (the hammer's own rule). The material is
  refunded with the normal pop-up; undo covers the first 30 or so shapes, as Area remove.
- New: **Replace works on terrain.** With a terrain material selected, Replace (all three
  styles) repaints ground too, for example dirt into stone, keeping its shape. The fill
  bucket spreads over terrain as well. The old terrain is refunded by how much of it was
  there.
- New: **the fill bucket follows roofs diagonally.** For roof materials, connected replace
  and connected delete also spread to diagonal neighbours (side to side, up and down and
  across), so a whole sloped roof is caught in one click.

## 0.18.0

- New: **Building Tools menu in the game's own UI.** Press Home (also while holding the
  hammer) for a full-screen menu that looks and works like the game's menus: blurred
  world, the game's tab bar (Q / E), mouse cursor, and Esc to close. The character and
  camera stay still while it is open.
- **Keys tab:** click a key, then press the new one. Shift, Ctrl and Alt combinations
  work (for example Shift+K). Esc cancels, Delete clears. Saved to BuildingTools.ini at
  once; a key taken from another action is removed there.
- **Options tab:** mode, Replace, replace style, area fill and free placement, as rows
  like the game's settings. The F-keys keep working.
- The menu key itself can be changed in the Keys tab or with `Menu=` in the ini.
- New: **the mod's keys in the game's building box** (bottom right, under "Undo Build
  Action"), with the game's own key icons: Area mode, Replace and Free placement with the
  game's on/off switch, plus fill, replace style, clear corners, wheel distance and the
  menu key while they apply. `HudRows=0` in the ini turns them off.
- New: **info as floating text** below the screen centre, drawn like the game's snapping
  hint (blurred, soft backdrop): area state, size and block count, and the mod's short
  messages. The old info bar at the bottom only shows up when the building box is not
  there (for example with `HudRows=0`).

## 0.17.4

- Fix: Replace often said "could not read the aimed block, try again", above all while
  flying (Airborne Architect) and on the first clicks after loading. The game skips its
  own placement preview while the ghost is blocked, and the mod only learned where you
  aim from that preview. It now reads your aim every frame.

## 0.17.3

- New: **partial area and connected actions.** When an area build, area replace or
  connected replace costs more than your backpack and chests hold, it now does as many
  shapes as you can pay for (bottom layer first; nearest blocks first for connected
  replace) instead of nothing. The banner says "Placed N of M shapes (not enough
  material)". Replace refunds and undo cover exactly the shapes that were done.
- Fix: such an action used to fail without a word (often right after the "FillLimit"
  message). If not even one shape can be paid, the banner now says "Not enough
  material for N shapes (needs X items)".
- Fix: very big area and connected actions (thousands of shapes) could take longer to
  price than the mod waited for the game to write them, and then only one shape or
  nothing was changed. The wait now starts when pricing is done.
- Fix: connected replace with a big shape said "that is already your material" when
  the aimed spot also held blocks of the new material (for example next to blocks
  already repainted). It now picks the clicked material from the other blocks there.
- The FillLimit message now reads "only the first N shapes (FillLimit)": the shapes
  found up to the limit are still replaced.

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
