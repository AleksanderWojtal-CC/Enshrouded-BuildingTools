Enshrouded Building Tools 0.13.2 (single player)
==============================================

Files in the game folder
  dinput8.dll           the mod (Windows loads it with the game)
  BuildingTools.ini     settings, created on first start
  BuildingTools.log     what the mod did (previous session: BuildingTools.prev.log)

Keys (with the Construction Hammer)
  F6  switch Prefab mode (vanilla) <-> Area mode
  F7  Replace on/off (works in both modes)
  F8  clear area corners
  F9  Replace style: full shape <-> overlap only
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
  (line / plane / box, shapes along each side, height, total). Corner clicks
  build or remove nothing. Switching between left and right click starts over.
  Build charges per shape as usual; Replace works as in Prefab mode (F9 style);
  Remove refunds like normal removal.
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

Info panel and sounds
  The info panel sits at the bottom edge of the screen, shows the mod's state
  and keys, and hides after 5 seconds without changes (it comes back with the
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
  Keys are Windows key codes: F1..F12 = 112..123, A..Z = 65..90, 0..9 = 48..57,
  Page Up 33, Page Down 34, Insert 45, Delete 46, Home 36, End 35,
  Numpad 0..9 = 96..105. 0 turns a key off. Example: to use F5 for Area mode,
  set ToggleAreaMode=116. The comments in the file explain each option.
  Delete the ini to get all defaults back.

Uninstall
  Close the game and delete dinput8.dll (the ini and log can go too).
