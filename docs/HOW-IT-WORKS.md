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
Hooks are 5–10 byte inline jumps to a relay allocated within ±2 GB of the target, plus a
trampoline holding the stolen instructions.

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

## Replace

Placement normally never touches existing building voxels. Materials 0x80–0xff are
building materials, 1–0x7f terrain, 0 air. The mod patches the existing-material checks in
the voxel blit callback (a81350) with `src/cave.S`. In Replace mode an existing building
voxel of another material is sent to the building write path and its old material is
counted. Materials the game would not let you remove at that spot are left alone: they
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

## Free placement

The place cursor transform is computed in 28bc80 from the aim ray. With free placement
on and a building shape selected, a post-hook replaces the position with
`camera + forward × distance` (camera position and rotation from the cursor system),
snapped to the 0.5 m grid, with the last valid rotation, and marks the transform valid.
The game then shows its normal ghost there and places at it.

## Sounds and info bar

UI sounds come from the game's UI bundle (`FbUiBundle.sounds`). They are played through
the game's UI sound function (a429f0) from a hook on the HUD building update (b25350),
because that function is not thread safe.

The info bar is a transparent, click-through, top-most window over the game's client
area. It scales with the window height and hides after a few seconds without changes.

## Files written

`BuildingTools.ini` holds settings, written with comments on first start. `BuildingTools.log`
records every action, with the previous session kept as `BuildingTools.prev.log`. A crash
handler logs fatal exceptions as `game+offset` / `mod+offset` with a stack trace, which
is how the bugs during development were found.
