# How the mod works

This is the design overview. Exact addresses and the research history are in
[reverse-engineering-notes.md](reverse-engineering-notes.md). All addresses are RVAs in the
2026-10-20 1.0 client.

## Loading

Windows loads `dinput8.dll` from the game folder before the system one, so the mod is
a *proxy DLL*. It exports `DirectInput8Create`, which loads the real
`System32\dinput8.dll` and forwards the call. On load it starts:

- an **init thread**, which waits for the game to unpack, finds all code by byte signatures
  and installs the hooks. If any required signature is missing, the mod does nothing and
  logs why.
- an **input thread** for the hotkeys, an **overlay thread** for the info bar, and a
  **mouse hook thread** for Q + wheel.

No C runtime is linked. The DLL has its own entry point, `memcpy`/`memset` and a stack probe.
Hooks are 5–10 byte inline jumps to a relay within ±2 GB of the target, plus a
trampoline holding the stolen instructions; relays, trampolines and the replace blit
share one 64 KB block next to the game image. If another mod already put a jump at a
function's entry, the hook chains to that jump. Two functions that Creative Mode also
finds by their first bytes (`PayCost` and 9bc1a0) are not patched at all: the mod
retargets the `call` instructions that reach them instead, so both mods work whichever
starts first.

The DLL also works when loaded by another loader (for example as `BuildingTools.dll`
listed in the XHL Native Core's `dinput8-chain.ini`): everything starts from `DllMain`,
and the ini and log are kept next to `enshrouded.exe`.

## The game's building pipeline

Enshrouded's engine is an ECS. Building runs in the `player_building_blueprints`
(placing) and `player_building_dismantle` (removing) systems. In single player both run
in the in-process **server world** and in one or more **client prediction worlds**.

For one click the server does:

1. **Dry run.** `PlaceShape` (48a660) or `RemoveShape` (48b180) measures the shape through
   the voxel blit with a "count only" flag and appends cost entries to the undo step's
   cost list.
2. **Payment.** `PayCost` (48eeb0) charges (or, for removal, refunds) the list via the
   player's inventory transaction.
3. **Commit.** `PlaceCommit` (487270) or `RemoveCommit` (487af0) writes the voxels and adds
   one undo record to the current undo step.

Clients receive the result as voxel data from the server. That is why the mod acts only
in the server world: clients only lend their live aim cursor and skip their prediction.

## Area mode

`place_hook` (on `PlaceShape`) and `remove_hook` (on `RemoveShape`) turn clicks into
corners. Corner clicks build and remove nothing, and the following commit is suppressed.
On the last corner, the area is planned (`core::plan_fill`):

- Corners are stored as **shape centres** from the shape's bounding box. The cursor
  position depends on which face was clicked, so it can't be used directly.
- Shapes are laid exactly one shape size apart, starting at corner 1, with
  `round(distance / size) + 1` shapes per axis.
- Fill modes keep the boundary cells (hollow) or only the side boundaries (walls).

Every position is dry-run priced in the game's own cost list, so the normal payment step
charges the total. The single commit is then replaced by one commit per position.

**Undo budget.** An undo step holds at most 32 records, and the record pool and snapshot
memory are small (about 30 small blocks in total). When they are full, the game drops
the oldest undo steps, even the one being written, which corrupts the pool. The mod
therefore records undo only while the current step's own records clearly fit. The rest
is committed without undo, and the step's cost list is trimmed so undo refunds exactly
what it takes back. A hook on the "drop undo step" function (492480) detects any loss.

**Preview.** The client cursor's hover function (28e2f0) runs every frame. When the
hammer aims at an existing block, it highlights that block's voxels with one looped VFX
per voxel. The mod starts its own set of the same highlight, using the same start,
stop and time functions on the same thread, along the 12 edges of the area being set
up. Up to 256 voxels are used, thinned evenly. The effect is drawn on block surfaces,
so edges in open air stay invisible. The game's own blue ghost block is a tinted model
render, which the mod does not reuse.

**Partial actions.** Before an area or connected action is paid, the mod checks whether
the whole cost list is affordable, using the material counts of the picker's lists
(`updateInventoryStocks`, noted every frame: backpack plus magic chests, all bases with
GlobalMagicChests, exactly what the charge takes from). If not, it finds
the largest number of shapes, in planned order, whose costs are, cuts the cost list and
the queued shapes to them, and for Replace rebuilds the refund from a per-shape log of
repainted old-material voxels, so refunds and undo match the shapes actually written.

## Replace

Placement normally never touches existing building voxels. Materials 0x80–0xff are
building materials, 1–0x7f terrain, 0 air. The mod patches the existing-material checks in
the voxel blit callback (a81350) with `src/cave.S`. In Replace mode an existing building
voxel of another material is counted and handed back to the point where the game picks
the write path by the new material: a building material takes the block path (material
byte, density byte 0), a terrain material the terrain path (material and density byte).
The terrain path sees the old block as density 0, so it always writes.
An existing terrain voxel (material 1–0x7f, density > 0) that the removal dry run allows
is repainted too: a new building material takes the block path; a new terrain material
keeps the voxel's density and is priced by it (full shape then raises the density to the
shape's, as the game's terrain placement does). The cave tallies terrain in density units
(255 = a full voxel) and the refund turns them into density / 255, the unit the game's
removal uses. Materials the game would not let you remove at that spot are left alone: they
are found by a removal dry run, which feeds the `allow[]` table.

The dry run runs with the patch active, so the game prices exactly what will be written.
The counted old voxels go through the game's refund builder (48ae10), and the resulting
entries are paid back after the charge.

**Refund pop-up.** Items added by an inventory transaction produce `PickupEvent`s (the
"+N item" pop-up) only if the transaction has an event writer and flag bit 0. The
placement system has no such writer. The mod records the dismantle system's writer per
world (from the system dependency block copied by 9bc1a0) and plugs it into the
placement transaction before refunding.

**Undo.** The game's undo refunds the step's cost list (the new material) and restores
the old voxels. The mod remembers, per undo step, the refund it paid for the shapes that
undo covers. When undo refunds that step, the mod first takes the old material back. If
it is no longer in the inventory, the undo is refused.

## Connected replace (fill bucket)

With Replace style "connected" in Prefab mode, the click starts a breadth-first walk
over a grid of cells the size of the selected shape, from the aimed shape along the six
world axes. Each new cell is probed with a removal dry run (`RemoveShape` with refunds
suppressed), which reports the volume of every material it would remove there; a cell
joins when it contains the clicked material (the building material with the most volume
in the first cell). The walk stops at `FillLimit` cells. The found cells are then priced
and committed like an Area replace in overlap style, with the blit's allow table holding
only the clicked material, so other materials in the same cells are left alone.
The clicked material can be terrain too, when the removal dry run reports it (a terrain
material is selected).

## Connected delete

With the same style, a right click starts the same walk from the dismantle system's
dry run (`RemoveShape`). Removal builds a 256-entry "may remove" table per material
(48f1c0) both in the dry run and in `RemoveCommit`, and the removal blit leaves every
voxel whose entry is 0. The mod redirects both calls; while it removes connected cells
on its thread, every material but the clicked one is cleared from the table. Every
found cell is then dry-run in the dismantle's own context (so the game builds the
refund for the clicked material only) and committed with one `RemoveCommit` per cell,
with the same undo budget as Area remove.

## Magic chests of all bases

With `GlobalMagicChests=1`, building and crafting use the magic chests of every base
(flame altar area), not only the one the player stands in.

The game keeps every base in a `PlayerBases` system (one in the server world, one in
the client world): the number of bases at +0x24 and up to 16 records of 0x130 bytes from
+0x30. A record starts with the list of the base's magic chest entity ids and holds the
base's area box. Three places in the game use "the chests of the base the player is in",
and the mod widens each of them. All three are reached through redirected `call`
instructions; the game functions themselves are not patched.

1. **Payments and crafting (server).** An inventory transaction (building costs,
   crafting, station queues) takes from the backpack and then from the chests that
   `ChestList` (468660) returns for the player's position. The mod returns the chests of
   every base instead, the current base first, so local chests are used before distant
   ones. Outside every base, where the game would answer "not in a base", it returns all
   bases' chests too. Two callers keep the normal list:
   - the **loot deposit** (`area_deposit_trigger`), which would otherwise put loot into
     chests of other bases;
   - **client worlds** (the UI's per-recipe checks ask several thousand times per
     second). Their counts come from the client stock map below, so widening them would
     only cost time; in testing it dropped the game to 2 FPS with a crafting bench open.
   If the deposit's call site is not found in a game version, only the building systems'
   payments are widened (a thread flag set in `pay_hook`) and crafting stays local.
   One client-world change is needed: the crafting menu chooses "backpack only" when
   its chest list is empty (a base without chests of its own, or no base), and
   "backpack and chests" otherwise. In that case the mod hands it one chest of another
   base, so the menu takes its counts from the client stock map (all bases) and lets
   the craft go to the server, which then uses every base's chests.
2. **Client stock map (picker, ghost, crafting menu).** The client's
   `cache_crafting_stock` system adds up the current base's chests into one stock map,
   40 chests per frame, through the iterator `ClientStock` (4680b0). The material picker,
   the red "missing materials" ghost and the crafting menu read that map. For the length
   of each iterator call the mod points the base's chest list at a list of every base's
   chests and restores it afterwards. The list starts with the base's own chests, and the
   pointer is swapped in before the count and out after it, so another thread reading the
   record never sees an id past the end. The list is rebuilt only when a new pass starts
   or the player changes base. Outside every base it uses the first base and a point
   inside that base's area, so the map is built for the player anywhere.
3. **Server cursor check.** Before a click turns into a building action, the server
   (`server_cursor_no_prediction`) runs the same missing-materials check as the client
   (2865e0). It counts the material in a per-base stock map (`StockCount`, 469610), and
   those server maps do not hold what the chests contain (they read 0 in testing even
   for materials in the local chests). For a material that is only in another base, the
   server therefore dropped the click: the animation played, nothing was built or paid.
   With the option on, the server's count answers "plenty" and the payment (1) decides:
   it takes from every chest, or refuses the placement when there is not enough
   anywhere. The client's check is unchanged and still turns the ghost red when you are
   short. Server and client worlds are told apart by the client world's system, which the
   mod learns in (2); until then the check is left alone.

Chests whose entity is not loaded are skipped by the game itself (the entity lookup
fails), so a base far away may only count once the game has it in memory.

## Off switch

`ToggleMod` (F5) sets the mod back to vanilla: Prefab mode, Replace, free placement and
creative building off, corners cleared. All hooks stay installed but pass everything
through, and the other mod keys are ignored until the key is pressed again.

## Free placement

The place cursor transform is computed in 28bc80 from the aim ray. With free placement
on and a building shape selected, a post-hook replaces the position with
`camera + forward × distance` (camera position and rotation from the cursor system),
snapped to the 0.5 m grid, with the last valid rotation, and marks the transform valid.
The game then shows its normal ghost there and places at it.

## Creative building

With `CreativeBuilding=1`:

- **Payment.** `PayCost` is reached through redirected `call` instructions, so the mod
  knows which game system is paying (the return address). For the calls of the
  blueprint (place), dismantle (shape and prefab) and undo systems it returns success
  without touching the inventory; every other caller (props, crafting, chests) pays
  normally. Replace's own refund of the old material is skipped too.
- **Preview.** The client's "missing materials" check (red ghost, Build greyed out) is
  answered with "enough" for building shapes and prefabs.
- **Material picker.** The picker's lists are rebuilt every frame by
  `updateInventoryStocks` (4076b0) from the backpack, the magic chests and the
  favourites. While the hammer is out, the mod adds every item of the terrain, block,
  roof and overgrowth item groups (count 0) to the list on screen, in its "backpack and
  magic chests" order only, just before the game finalises it. The ids are collected
  once; each frame only missing entries are appended.
- **Empty types.** When the material chosen last is not in a list, the game picks the
  selected entry and sends it as the new choice, but refuses an entry with count 0
  (406020). The mod accepts it, so a type with nothing in the inventory gets a material.

## Menu and HUD (game UI)

The game's UI is immediate mode: every frame the HUD code calls widget functions
(beginWindow, beginBox, text, buttons...) and the engine lays them out. The mod calls the
same functions, found by signatures, so everything looks like the game.

- **Menu.** The HUD has a screen byte (HudScreenType at hud+0x1cd8); 52 is unused in 1.0. A
  hook on the play UI (the function the game calls for the HUD each frame) opens and closes
  it with the game's toggleHudScreen, and the screen dispatcher's call for 52 is redirected
  to the mod's menu. The window is fullscreen and modal (input stays in the menu), with the
  game's blur, tab bar (menuTabBar with the tab list and tab widget redirected while the
  mod's menu draws it), setting rows (settingOption), buttons and hint bar. Key rebinding is
  captured by the input thread; the ini is rewritten at once.
- **Building box rows.** The building HUD box function draws its key rows with a row
  function (action + binding + label). The mod redirects the two row calls to copy their
  parameters and the call that finishes the box to add its own rows first, drawn with the
  game's bindingHint. Bindings are built from the mod's keys (USB HID codes, modifiers as
  extra inputs), so the game shows its own key icons. On/off rows are built like the
  game's "first person view" row: key row, stretcher, the game's switch.
- **Floating text.** After the play UI, the mod opens a fullscreen window that takes no
  input and draws a centred box below the crosshair like the game's snapping hint: blur
  behind, the soft nine-slice frame, body text.

## Sounds and info bar

UI sounds come from the game's UI bundle (`FbUiBundle.sounds`). They are played through
the game's UI sound function (a429f0) from a hook on the HUD building update (b25350),
because that function is not thread safe.

The info bar is a transparent, click-through, top-most window over the game's client
area. It scales with the window height and hides after a few seconds without changes. It
stays hidden while the floating text is shown (HudRows=1), so it is mainly the fallback
when the game UI hooks are off or not found.

## Files written

`BuildingTools.ini` holds settings, written with comments on first start. `BuildingTools.log`
records every action, with the previous session kept as `BuildingTools.prev.log`. A crash
handler logs fatal exceptions as `game+offset` / `mod+offset` with a stack trace, which
is how the bugs during development were found.
