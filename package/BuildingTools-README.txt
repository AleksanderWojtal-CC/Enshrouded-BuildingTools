Enshrouded Building Tools 0.18.0 (single player)
==============================================

Files in the game folder
  dinput8.dll           the mod (Windows loads it with the game)
  BuildingTools.ini     settings, created on first start
  BuildingTools.log     what the mod did (previous session: BuildingTools.prev.log)
  BuildingTools-Languages\   the translations, one text file per language

Keys (with the Construction Hammer)
  Home  Building Tools menu: change keys and options in the game's own UI
  F5  whole mod off / on (off = vanilla game, other mod keys do nothing)
  F6  switch Prefab mode (vanilla) <-> Area mode
  F7  Replace on/off (works in both modes)
  F8  clear area corners
  F9  Replace style: full shape -> overlap only -> connected (fill bucket)
  F10 Free placement on/off: the ghost hovers in front of you (snapped to the
      0.5 m building grid) instead of sticking to the surface you aim at
      Hold Q + mouse wheel (or Page Up / Page Down): farther / closer, 0.5 m steps
  F11 Area fill: solid -> hollow room -> walls only

Prefab mode
  Vanilla placement. With Replace on, the shape replaces the blocks you aim at
  with your current material (aim at the block itself, not the ghost).
  Replace styles (F9):
    full shape   - blocks inside the shape get the new material and the rest of
                   the shape is built as well
    overlap only - only what already exists inside the shape is repainted; its
                   exact form is kept and nothing new is built
    connected    - fill bucket: click a block and every touching block of the
                   same material gets your material, nothing else changes. It
                   spreads in steps of the selected shape, so a 0.5 m cube
                   follows the blocks exactly and a bigger cube is faster but
                   coarser. Stops at FillLimit shapes ([Limits] in the ini,
                   default 500). Prefab mode only (in Area mode it works like
                   overlap only). The clicked block's material is the one that
                   spreads. Roof materials also spread diagonally (side to
                   side, up and down and across), so sloped roofs are
                   followed.
                   Right click deletes instead: every touching block of the
                   clicked material is removed and refunded, other materials
                   in the same spots stay.
                   The fill bucket works with the small shapes (first row of
                   the shape picker); with a bigger shape it replaces or
                   deletes one whole shape.
  Any material can be the new one, also a terrain material (the blocks then
  become that terrain). The ground is repainted (and with connected, deleted)
  only while a terrain material is selected, the same rule the hammer uses
  for removing ground; it keeps its shape and is refunded by how much of it
  was there.

Area mode (2 corners)
  Click two opposite corners; the current shape is repeated over the area
  between them, shapes side by side (exactly one shape size apart), starting at
  corner 1.
  Left click  = build corners (with Replace on: replace corners)
  Right click = remove corners
    line : two ends of the line
    floor: two opposite corners at the same height
    wall : two opposite corners in the same vertical plane
    box  : bottom corner and the opposite top corner
  After corner 1 the banner shows live what you would get if you clicked now
  (line / plane / box, shapes along each side, height, total), and the edges
  of the area light up with the hammer's block highlight. The highlight shows
  on block surfaces (and inside blocks when removing), not in open air.
  Turn it off with [Area] Preview=0. Corner clicks
  build or remove nothing. Switching between left and right click starts over.
  Build charges per shape as usual; Replace works as in Prefab mode (F9 style);
  Remove refunds like normal removal.
  Building pieces (foundations, stairs and other placed pieces) are removed
  as blocks too, in Area mode and with connected delete. In Prefab mode a
  right click removes the whole piece, as in the game. With a building piece
  in the hammer, Replace and the fill bucket are off (vanilla game).
  Fill (F11): solid fills the whole box; hollow room builds only the outer
  shell (walls, floor and ceiling, one shape thick); walls only leaves out the
  floor and ceiling. Floors, walls and lines come out complete in every mode.
  The fill applies to build, replace and remove.
  3-corner mode is still there: in BuildingTools.ini set [Area] Corners=3
  (two base corners, then a third click for the height).
  MaxShapes in the ini limits the size (default 2000 shapes).

Undo after Replace
  Undo puts the old blocks back, returns the new material and takes back the
  old material Replace gave you. If you no longer have that old material, the
  undo is refused (banner message) so nothing is duplicated.

Undo after area actions
  The game's undo memory is small (about 30 small blocks). An area action is one
  undo press; for big areas it covers only the first shapes ("undo covers N" in
  the banner) and gives back exactly their materials. Remove the rest with an
  area remove.

Menu (Home)
  A full-screen menu like the game's own, also while holding the hammer. Switch
  tabs with Q / E or a click; Esc or Home closes it.
  Keys tab: click a key, then press the new one. Shift, Ctrl and Alt
  combinations work (Shift+K). Esc cancels, Delete clears. Saved at once.
  Options tab: mode, Replace, replace style, area fill, free placement, language.

Building box and floating text
  While you build, the game's building box (bottom right) lists the mod's keys
  with the game's key icons: Area mode, Replace and Free placement with an
  on/off switch, then fill, replace style, clear corners, the wheel distance and
  the menu key while they apply. The area state, size and block count and the
  mod's short messages show as floating text below the crosshair.
  HudRows=0 under [Interface] turns both off and brings back the info panel.

Info panel and sounds
  The info panel at the bottom edge of the screen is used when the building box
  rows are off (HudRows=0). It shows the mod's state and keys, and hides after
  5 seconds without changes (it comes back with the
  next change). Under [Interface] in BuildingTools.ini (close the game first):
  PanelScale (size in percent), PanelBottom (lift it up, pixels at 1080p),
  PanelTimeout (seconds, 0 = always visible), PanelKeys (0 = no key line).
  Sounds are the game's own UI sounds (Sound=1); Sound=2 uses Windows sounds,
  Sound=0 turns them off. When Replace gives old material back, the game's
  item pop-up shows it (RefundPopup=0 turns that off).

Changing keys and settings
  All keys and options are in BuildingTools.ini in the game folder (this
  version rewrites an older ini once so it lists every option; your values are
  kept). Close the game, open the file with Notepad, change a number, save.
  Easier: change keys and options in the menu (Home), which saves the ini.
  Keys are Windows key codes: F1..F12 = 112..123, A..Z = 65..90, 0..9 = 48..57,
  Page Up 33, Page Down 34, Insert 45, Delete 46, Home 36, End 35,
  Numpad 0..9 = 96..105. 0 turns a key off. Add 4096 for Shift, 8192 for
  Ctrl, 16384 for Alt (Shift+K = 4096 + 75 = 4171). Example: to use F4 for Area mode,
  set ToggleAreaMode=115. The comments in the file explain each option.
  Delete the ini to get all defaults back.

Language
  The mod's texts follow the game's language (Settings > Gameplay > Language, or
  Steam's language when the game uses the system language). Options tab in the
  menu > Language picks one by hand; Language= under [Interface] in the ini does
  the same (auto, en, de, fr, it, ja, ko, pl, pt-BR, ru, es, th, tr, uk, zh-CN,
  zh-TW). The translations are text files in the BuildingTools-Languages folder.
  English and Polish are checked; the others are drafts and may contain
  mistakes, sorry! Corrections are very welcome: edit the file, test it in the
  game (it is read when the language is set and when the menu opens), and send
  it in a Nexus comment or on GitHub:
  https://github.com/AleksanderWojtal-CC/Enshrouded-BuildingTools
  Language=test shows every text longer and in [brackets ~~], to check that
  long translations fit.

Turning the mod off
  F5 (ToggleMod in the ini) switches the whole mod off: Prefab mode, Replace,
  free placement and creative building turn off, the info panel stays hidden
  and the other mod keys do nothing. The game then plays exactly as without the
  mod. Press F5 again to turn it back on (the banner says which).

Creative building
  Set CreativeBuilding=1 in the [Creative] section of BuildingTools.ini (close
  the game first). Blocks, roofs, terrain and prefabs then cost nothing, and
  removing or undoing gives nothing back, so your inventory is never touched.
  Props, chests and crafting stay normal. The info bar shows "creative".
  Material picker: in its "backpack and magic chests" order it lists every
  block, roof, terrain and overgrowth material, also ones you have none of
  (count 0). Favourites, last used, amount and item sets show the normal lists.
  A type you have nothing of (e.g. no roof at all) can be built right away.
  Blocks built in creative are refunded like any other block if you remove
  them later with creative off.

Magic chests of all bases
  Set GlobalMagicChests=1 in the [Chests] section of BuildingTools.ini (close
  the game first). Building and crafting then take materials from the magic
  chests of all your bases (flame altar areas): first your backpack, then the
  chests of the base you stand in, then the other bases. This also works
  outside a base. The material picker, the red "missing materials" ghost and
  the crafting menus count the chests of all bases. The loot deposit still
  only puts loot into the chests of the base you are in.
  If there is not enough of a material anywhere, the block is simply not
  placed, as without the mod. F5 (mod off) switches this off too.
  The game only sees chests in parts of the world it has loaded; chests of a
  base far away may not count until you have been near it. Off by default.

Uninstall
  Close the game and delete dinput8.dll and the BuildingTools-Languages folder
  (the ini and log can go too).

With other mods that ship a dinput8.dll (Creative Mode, XHL mods)
  Only one file can be called dinput8.dll. Use the XHL Native Core (the
  dinput8.dll from any XHL mod, e.g. XHL Creative Mode Fix) as dinput8.dll and
  let it load the others:
  1. Rename this mod's dinput8.dll to BuildingTools.dll.
  2. If you use Creative Mode: rename its dinput8.dll to CreativeModeLoader.dll
     (do this before the XHL dinput8.dll overwrites it).
  3. Put the XHL dinput8.dll in the game folder.
  4. Create dinput8-chain.ini in the game folder:
       [Chain]
       CreativeMode=CreativeModeLoader.dll
       BuildingTools=BuildingTools.dll
     (leave out the CreativeMode line if you don't use it).
  XHL-Native-Plugin-Host.log shows what it loaded; BuildingTools.log should end
  with "ready".
  XHL mods with their own folder under mods\ (e.g. XHL Vein Mining) are loaded
  by the XHL dinput8.dll as before; Building Tools runs next to them.
