# Enshrouded building mod: reverse-engineering notes

Target: `enshrouded.exe` 1.0 client, PE timestamp 2026-10-20 14:47:40 UTC,
SizeOfImage `0x2A1E000`, image base `0x140000000`. All addresses below are RVAs.
Single player only (the client runs its own internal server).

## Tooling (cloud workspace)
- Capstone 5.0.6 built from source, Python bindings in `pylib/`.
- `pe.py` PE loader + `.pdata` function table + chained-unwind `real_start()`.
- `index.py` full linear sweep of `.text` -> `xidx.pkl` (rip-relative refs, call/jmp targets).
- `refl.py` dumps the engine's reflection data -> `types.txt` (12,646 types with field
  offsets; enums decoded).
- `systems.txt` 861 ECS systems: `name fn=<rva> desc=<rva>`.

## Engine facts
- Positions are int64 fixed point, 1 m = 2^32 (scale constant 2.3283064e-10).
- Cursor/aim transform is 0x38 bytes: int64 x,y,z; float4 rotation (+0x18); float (+0x28).
  Copied from the player's cursor component at +0x300 (or +0x338 when the player is
  in a special state 5/8, flag at +0x1b1).
- Reflection descriptor: +00 name, +20 full name, +40 u32 size, +48 u32 count,
  +4c kind, +50 u64 hash, +58 members (structs, stride 0x30) / +60 (enums, stride 0x28).

## Building pipeline (server side, runs in single player)
| What | RVA | Notes |
|---|---|---|
| ECS system `player_building_blueprints` | `2d3270` | Handles shape placement/removal |
| ECS system `player_building_dismantle` | `2d3c80` | |
| ECS system `player_building_place_prop` | `2d4fd0` | |
| ECS system `player_building_undo` | `2d5b20` | |
| Request event `keen::actor::PlaceVoxelObjectEvent` | type id `0xde627227` | +0x60 `subtract` (remove mode), +0x61 `consume` |
| Get cursor transform | `218720` | `(out, playerA, playerB)` |
| Get player undo storage | `4926d0` | used by every building action |
| Begin undo step | `4919a0` | |
| **Place one shape** | **`48a660`** | single caller (`2d3762`). Args: rcx=undo list, rdx=ctx(0x130 struct), r8=player building state (blueprint id at +0), r9=player data (material id byte at +0x454), [rsp+20]=&transform |
| Blueprint lookup by id | `e72bc0` | |
| **Voxel write (blit)** | **`a85300`** | shared by placement paths; returns changed volume |
| Material id -> item | `e75240` | |
| Record cost/undo entry | `490060` | |
| Pay/refund materials | `48eeb0` | mode byte: 0 = check/consume, 1/2 = refund at position |
| Send BuildingPlaceEvent | `487270` | |
| No-build-zone overlap test | `e8cc70` | `(zones, &aabb)` |
| Script `terraforming` node | `3e5ef0` | switch on `TerraformingType`: Add `3d38d0`, Flatten `3dbbb0` |

Enums: `TerraformingType` Remove=0 Add=1 Flatten=2 FlattenHorizontal=3 OvergrowthAdd=4
OvergrowthRemove=5 DecayAdd=6 DecayRemove=7.

Shapes: `keen::VoxelBlueprintItem` { itemId, uint3 size @+4, BlobArray<uint8> data @+0x10,
bool isDataCompressed @+0x18 }.

## Mod design (proposed)
Hook `48a660`. On a real click, call the original once per target position:
- line / plane / box: positions between first and second click, stepped by the shape size.
- brush size N: N x N x N grid of the current shape.
- remove variants: same, with the event's `subtract` flag.
- replace: subtract then add with the current material at the same position.
Cost and undo then go through the game's own code (one undo step per click).

## Open
- Voxel material read (needed for replace-connected / flood fill and "replace only
  this material"). Candidates: the dismantle refund path (`48b4c0`), or runtime tracing
  of `BlueprintMaterialSelectFromCrosshair`.
- Confirm in game that calling `48a660` repeatedly within one event is safe (cost
  accumulation, undo grouping, client prediction/visual sync).
- Shroudtopia loader on this build; pattern signatures for every RVA above.

## Findings from in-game test 1 (v0.1.0)
- The blueprint system runs once per world per click: server (has
  `g38_building::UndoStorage`, cost list = undo step [rbp+0x220] + 0x110) and 1–2
  client prediction worlds (cost list = stack local rbp+0x430, aimed cursor stale or 0).
- Cursor component: +0x300 = place cursor (ghost; next to surface for 0.5/1/2 m
  blocks), +0x338 = aimed cursor (the block you look at). Large prefabs: both equal.
- Placement context +0x40 must be a TerraformingLootMemory (0x808 bytes) for the
  removal function; the placement system leaves it null -> crash when removal hits blocks.
- Cost entries are 0x18 bytes, item id at +0x10, count at +0x14. `48eeb0(mode, list,
  consume)` charges (1) or refunds (0) a whole list; mode byte 0 = inventory.

## v0.2.0 changes
Server-only actions; prediction suppressed while a mod mode is on; loot-memory stand-in
for removals; refunds via a hook on 48eeb0; corner clicks place nothing.

## Findings from in-game test 2 (v0.2.0)
- Server ServerCursor +0x338 is NOT live aim: it holds the previous action's position.
  Live aim only exists in client worlds' cursor component (+0x338).
- Server-only edits are not shown on the player's screen (only the last click's
  region updates); the client world must perform the same edit itself, as vanilla does.

## v0.3.0 changes
Mod action runs in every world (server + clients) with per-world corner state keyed by
ctx[0]; clients record place->aim, server looks aim up by identical place cursor
(1.5 s window). Third corner axis-locked (core::lock_third).

## Findings from test 3 (v0.3.0) + static analysis
- Crash: client cost list is an 8-entry inline array with no allocator
  ("No allocator set in BaseDynamicArray") -> never append >8 entries in client worlds.
- Client worlds run the system several times per click (input replay), so per-world
  click counting is wrong.
- KEY: 48a660 (PlaceShape) and 48b180 (RemoveShape) are DRY RUNS: blit callback
  a81080 checks params+0x30 ("count only") which they set to 1. Real writes happen
  after payment: 487270 = PlaceCommit(ctx, Commit*, shapeId), 487af0 =
  RemoveCommit(ctx, Commit*, shapeId). Commit = {float pos[3] (cursor pos, m),
  float rot[4] @0xc, u32 player entity @0x1c, u8 flag @0x20}.
- 48a050 replays building action records by type (+0x4c): 2=place(487270),
  4=remove(487af0), shape id @+0x48 -> this is how server writes reach clients.
- Cost entry from PlaceShape = {pdata+4 (16 B), u32 pdata+0, u32 e75240(ctx+0xf8, pdata[0x454])}.

## v0.5.0 design
Server only. place_hook prices every shape (removal dry runs -> refund buffer;
placement dry runs, synthesised entry when replace hits full voxels) and queues
positions; place_commit_hook (487270) turns the single commit into RemoveCommit +
PlaceCommit per queued position. Clients only lend live aim (g_clientCC) and return 0.

## Test 4 (v0.5.0): crash in RemoveCommit with the placement context
- 487af0 -> 48bff0 / 490100 read ctx+0x120 unconditionally. In the dismantle ctx it is a
  system dependency (likely ecs::ColliderSystem, which only dismantle/undo declare);
  in the placement ctx +0x120 is a non-pointer value.
- Blit add path (a81080, params+0xa0 != 0) only raises density: full voxels are never
  overwritten, so Replace needs a real RemoveCommit.
- 48f1c0 builds the 256-entry per-material "may write" table via 48f070.

## v0.6.0
Hook RemoveShape (48b180) to capture dismantle ctx fields +0x40/+0x48/+0x120 from the
game's own server-side dismantle (keyed by ctx+0x30 voxel world). RemoveCommit runs on a
copy of the placement ctx with those patched in. Replace refuses until captured.
Vectored exception handler logs crash address + stack return addresses.

## Test 5 (v0.6.0)
Crash logged by the handler: game+0x9b7e30 (movzx eax,[rcx+0xb8], rcx=0) <- 490540
(reads ctx+0xc8) <- 487af0+0x6ba, after the removal itself. 48bff0 returns false when
ctx+0x120 == 0 (aborts the removal), so both are real dismantle-only dependencies.
v0.6.1: capture the full dismantle ctx (0x158 bytes); RemoveCommit uses it with
stack-pointing fields and per-click fields (+0x30,+0xe8..+0x108,+0x130..+0x140,+0x148..+0x157)
from the placement ctx.

## Test 6 (v0.6.1)
Same crash: 490540 = send BuildingTearDownEvent (type 0x474baeed, 0x48 bytes) via event
writer at ctx+0xc8; null in both captured dismantle ctx and placement ctx at that point.
It is the last step of RemoveCommit (effects only). v0.6.2 hooks 490540 and skips it when
ctx+0xc8 is null.

## Test 7 (v0.6.2): prefab Replace WORKS
Removal + placement + refund work repeatedly. New material was not charged: e75240 is
items(registry, material, float volume in xmm2) = ceil(volume / (blocksPerLootItemExchangeRate*8));
the synthesised entry passed no volume. v0.6.3 hooks 48ae10 (refund builder, gets the
per-material removed volume table {float*, count}) to sum removed volume during the mod's
own removal dry runs and prices the new material from it.
Open: the refund shows no inventory pop-up (vanilla dismantle sends a notify/UI event).

## Test 8 (v0.6.3): prefab Replace complete for full-shape style; costs correct.
Requests: overlap-only style; drop the "Replace ready" warm-up.

## v0.7.0: replace through a blit patch (src/cave.S)
- 48a050 is only called by player_building_undo, so clients receive building edits as
  voxel data from the server; a server-side blit change replicates.
- Removal blit (a84960) sets fully removed voxels to (material 0, density 0) = air.
- Patch: blit add path density check at a813ba (sig a813a7, +0x13, 11 bytes) jumps to a
  copied cave. Mode 0 = original code; 1 = overlap (solid voxels: new material, keep
  density; empty: skip); 2 = full (solid: max density; empty: fill). Skips voxels already
  of the new material; tallies old density per material in cave vols[256].
- Pricing = placement dry run with the cave mode set (game prices what is written);
  refund = 48ae10 called with vols/255 (same units as blit/removal: density/255).
- No RemoveCommit / dismantle capture / teardown hook in the replace path anymore.

## Tests 9-10 (v0.7.0/0.7.1): blit patch never reached
Diagnostics: cave hit counters 0; removal dry run showed removable volume but no
materials in 1..0x7f. => Voxel materials: 0 air, 1..0x7f terrain, 0x80..0xff BUILDING.
In a81080 the add loop skips existing materials >= 0x80 at a81350 (so placement never
touches building voxels, but overwrites terrain), and building materials (params+0xa0 == 0)
are written by the path at a81402 (material byte, 2nd byte 0, volume += 1.0 per voxel),
not the density path patched in 0.7.0.

## v0.7.2
Patch moved to a81350 (sig a81327, +0x29, 35 bytes = the three existing-material checks).
Mod mode: existing building voxel -> skip if same material or not in allow[] (materials
the removal dry run reports removable), else vols[m]++ and jump to the building write
path a81402; air/terrain -> overlap skips, full runs the original checks. Refund volumes
are voxel counts (1.0 each). 48f1c0 table hook dropped.

## Test 11 (v0.7.2): prefab Replace (both styles) WORKS.

## v0.8.0: Area mode (renamed from Structure)
- Build/replace: unchanged pipeline (dry run per shape, PlaceCommit per shape).
- Remove (RMB): hook RemoveShape 48b180 (server ctx = ctx+0xf0 undo step != 0) and
  RemoveCommit 487af0. Corner clicks return 0 and suppress the following commit (the
  dismantle system commits even when the dry run found nothing). Third corner: dry runs
  at every position in the dismantle's own ctx (refund entries go to the game's list,
  refunded by its own 48eeb0 call), then one RemoveCommit per position; commit position
  = click commit pos + (centre_i - click centre). Step = dry-run aabb extents.
- Corner state has an op (build/remove); switching op restarts corners.

## Test 12 (v0.8.0): Area build of 220 shapes crashed
Fatal "StaticQueue is full" in 491af0 (undo record add): an undo step holds 32 records,
one per PlaceCommit/RemoveCommit (only when ctx+0x151 == 0 and ctx+0xf0 != 0). Undo
storage (4926d0) keeps 5 steps (ring at +0xa00/+0xa08, stride 0x200); 4919a0(alloc =
ctx+0x30, storage = ctx+0xe8) begins a step (cost list at +0x110, inline cap 8; flag
+0x1f8 = consume flag used by undo's 48eeb0). Undo (2d5b20) refunds the step's cost list
then replays its records.
v0.8.1: commits in chunks of 30; chunk k>0 gets its own step (up to 5 total) with its
cost entries merged per item and moved there; later chunks commit with ctx+0xf0 = null
(no undo, entries dropped). ctx+0xf0 restored afterwards.

## Test 13 (v0.8.1): 3x3x3 build/remove fine; 392-shape build crashed in the mod
undo_end read a cleared cost list: the first step had been dropped. 491af0 also drops
oldest steps (492480) whenever the record pool (+0xa10, used +0xa28, cap +0xa38) or the
snapshot memory (+0xa50) is full; it can drop the current action's steps.
v0.8.2: hook 492480; if it drops one of the action's steps, the rest of the action
commits without undo records. Chunk costs are moved when the chunk's step begins.
Banner shows the live area size (client cursor +0x300 build, +0x338 replace/remove).

## Test 14 (v0.8.2): crash in record pool alloc (87b6d0 <- 491be9) after many area removes
Logs: the game dropped the action's first step after ~30-33 records every time, i.e. the
undo record pool/memory holds only ~32 small records in total. When the dropped step is
the one being written, 491af0 still writes its record into it -> orphaned records ->
corrupted pool free list. Chunking into several steps is useless with that budget.
Pool +0xa10: used u32 +0x18 (a28), cap u32 +0x28 (a38). Snapshot memory +0xa50 (ring
allocator 87bbf0): cap +0x10 (a60), head +0x18 (a68, monotonic).
v0.9.0: one undo step per action; records only while own records < 30, < pool cap-2 and
own bytes + 2*max record < mem cap/2; rest committed with ctx+0xf0 = null; step costs
trimmed to the recorded shapes. 2-corner Area mode (ini [Area] Corners=2 default).
Planner lays shapes exactly one shape size apart from corner 1 (no stretching, no
forced extra shape), so different shapes sit side by side.

## v0.10.0 (polish)
- UI sounds: FbUiBundle (ctx = [rva 281e498], bundle = ctx+0xc8) .sounds @0x12060;
  play = a429f0(ctx, &SoundContainerReference copy) (not thread safe: hash-set insert).
  Played from a hook on the HUD building update b25350 (no stack args). Offsets:
  menu.highlightChange 12320, entryTrigger 12330, tabChange 12310, hud.buildActionFailed 12b70.
- Cursor query 287b10(q, sys, equippedItem, buildItem, a5, primary): built from the
  equipped item's CursorConfig (ItemInfo+0x104 equipment, +0x68 cursorConfig); q[0..2] =
  primaryActionConfig {hideWhenUnused, snapPositionToAddableVoxel, snapPositionToRemovableVoxel};
  q[3] mode dispatched in 28bc80 (1..5). Experiment: F10 clears q[1] for the primary query.
- Key-hint widget: FbUiLocaGameplayActionLabels (labels per input_mapping GameplayAction);
  adding custom entries needs new actions + loca text hooks (not done).

## Test 15 (v0.10.0): query byte q[1] already 0 for the hammer (dump 00 00 00 04 02 ...);
F10 had no effect. q[3]=4 = blueprint mode. 28bc80(out, sys, q, ray): ray[0]==0 -> hit path
28d9e0; no hit -> out valid only via forceClamp/restrictDistance path. out = {int64 pos[3],
float rot[4] @0x18, bool valid @0x28}. sys+0x640 camera pos, +0x658 camera quat (forward =
row 2 of 42880 matrix), +0x678 float distance.
v0.10.1: post-hook 28bc80: free placement writes pos = cam + fwd*dist snapped to 0.5 m,
rot = last valid rot, valid = 1. Panel bottom centre, font 24 px * height/1080 * PanelScale.

## Test 16 (v0.11.0): replace area missed the first layer when the two corner clicks hit
faces with different normals: the cursor position is not the shape centre and where it
sits depends on the cursor rotation (aim follows the surface normal). v0.12.0 stores
corners as shape box centres (shape_bounds) and converts planned centres back with the
template's (pos - centre) offset.
Ghost outline: camera pos/quat captured in 28bc80 hook (sys+0x640/+0x658); fovY/aspect from
ecs::ClientCamera {pos, quat, distance, fovY @2c, aspect @30, near, far}, found by a
memory search (ReadProcessMemory) for the current camera transform, verified over 750 ms.
Drawn as GDI lines on a colour-keyed topmost window. Camera basis = rows of 42880 matrix.

## Test 17 (v0.12.0): replace corner fix confirmed. Outline overlay never drew; user prefers
no overlay unless real in-game ghost blocks -> removed in v0.12.1.

## v0.13.0: refund pop-up
48eeb0 mode 0: mode+8 = inventory transaction C (init 21dea0, set up by 412170 from params).
Adds go through 40b4f0 into C's change list (+0xf0, count +0xf8, stride 0x34, type @+0x26:
0/5 = add). Commit 48be20 -> 408ed0 -> 4116f0 sends PickupEvent (0x7c31fc55, 0x30 bytes:
pos, itemId, count, by, from, slot, hideInUi) per added entry only if C+0xb8 (writer) != 0
and C+0xb0 bit 0. Mod sets bit 0 before its refund. Logs flags of vanilla refunds for
comparison. Ini rewritten with all options when older.

## Test 18 (v0.13.0): no pop-up. Placement transaction flags 0, writer 0; vanilla dismantle
transaction flags 0x25, writer 0x13bedbef0. Writer = dismantle system dependency (deps block
rbp+0x1f0 filled by 9bc1a0, +0x180 -> params+0xa0 -> C+0xb8). v0.13.1 hooks 9bc1a0, keeps the
writer per world (*sysctx) when called from the dismantle (ret 2d3cae), plugs it into the
placement transaction (C+0 = system ctx) and sets flag bit 0.

## Test 19 (v0.13.1): pop-up works. Undo of Replace duplicated: undo refunds the step's cost
list (new material) and restores the old voxels, the mod's refund of the old material stayed.
v0.13.2: per undo step (list ptr+data+count) remember the paid refund (prefab/full area) or
rebuild it from per-shape voxel snapshots for the recorded shapes (partial area undo); in
pay_hook, an undo refund of that list first charges those entries (refuse undo if missing).

## Test 20 (v0.13.2): "no live aim" with 2 m shapes in flight mode
The client world skips its placement prediction (so PlaceShape never runs there) when its
ghost is blocked, so neither the exact client-cursor match nor a click record existed.
v0.13.3 falls back to the client cursor component's live aim (+0x338) when it lies within
6 m of the server place cursor, and logs both client transforms when the lookup fails.

## v0.14.0: area preview
- The blue ghost block is a tinted model render (not VFX).
- Client cursor hover function 28e2f0 (called every frame from client_cursor) highlights the
  voxels of an aimed existing block: voxel list built by a8a290 only while hovering a block;
  per voxel a looped VFX guid {b0718612,4e1327b1,ca143cbf,9a608f41} started with
  29d3d0(fxSys = ctx+0x168, time = *9b9490(ctx[0]), &guid, &pos centre, &rot identity,
  &defaults 150ba30, &params) and stopped with 29d420(fxSys, &handle, time, &extra16).
  Voxel coords = floor(world m * 2); centre = v*0.5 + 0.25. ClientCursor keeps 256 slots.
- The mod runs its own 256 slots from a hook on 28e2f0 along the area's edges. The
  highlight is surface-projected (invisible in air). Hammer cursor VFX references tried
  (ItemInfo+0x1e4/+0x29c/+0x354/+0x38c): only equipment cursorVfx showed, flickery and near the
  player, with debris effects -> not used.
