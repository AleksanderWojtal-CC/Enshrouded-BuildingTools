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

## v0.15.0: compatibility and creative building
- Creative Mode (Phoenix 1355) scans `building.payCosts` (48eeb0) and `ecs.queryBegin`
  (9bc1a0) by their first bytes; no other of its 168 patterns overlaps bytes the mod
  patches. Both are now reached through redirected E8 calls (PayCost: 12 calls, 9bc1a0:
  the dismantle call only).
- PayCost callers: 2d39ab blueprints (place), 2d46b0 dismantle shape (-> RemoveCommit),
  2d4b5a dismantle prefab (-> 488670), 2d4cc4 dismantle prop pickup, 2d5934/2d5991
  place prop, 2d6040 undo, 2c8e6f/2c8eca, 3d3b89, 3e1ef7, 3e700e other systems.
- Missing-materials check 2865e0 (one caller 2975f2; true -> cursor flag 0x1000).
- `g38_items::updateInventoryStocks` 4076b0(stocks, ctx, onlyShown): 7 stock lists of
  0x648 bytes at stocks + type*0x648 ({data, count, cap, grow}, entries {itemId,
  slot u64 @+4, count u64 @+0x10} of 0x18), sort order byte +0x638 (0 spatial/backpack,
  1 last used, 2 amount, 3 curated/item sets, 4 favourites), selected index +0x628 and
  valid +0x630, shown type stocks+0x2bf8. Add 405140(list, &{id,count,0}, container,
  index, keep). Item groups: e823f0(ctx[0x68]), iterated with 3f070/3f270, group type
  byte 0x11..0x14 -> stock types 2..5 (terrain, default, roof, overgrowth), ids at
  group+8+off32, count +0xc. Finalise 4070c0 (call at +0xa76) sets the selected index
  from the chosen ids (405b90; types 2..5 read [ctx+8]+0xc8..0xd4).
- Removal refunds: building materials go through a per-material remainder
  (49fd70: acc += voxels; items = acc / rate), so a few 0.5 m blocks refund nothing.
- client_cycle_inventory_stock (2af5f0) -> 406020 (call 2afa47): chosen id not in the
  list -> selected/first entry, result 6 if its count is 0; result 0 + id -> 405270
  writes ClientPlayerInputData.buildingStockCycleAction (+0xa0 version, +0xa4..+0xb0).

## v0.16.0: off switch, connected replace, terrain as the new material
- Blit callback a81080, after the existing-material checks (a81373): [params+0xa0] != 0
  (terrain material placed) -> density path a81381..a81400 (density from the shape mask,
  existing terrain density from byte 1, building/air count as 0; writes material + density,
  volume += delta / xmm2); else block path a81402 (material, density byte 0, volume += 1).
  The cave used to jump straight to a81402, so a terrain material got density 0 = air;
  it now jumps to a81373.
- Connected replace probes cells with the mod's removal dry run (RemoveShape, refunds
  suppressed; per-material removed volume from the 48ae10 hook). In game: 500 cells with
  791 probes plus pricing took about 50 ms on the server thread.

## v0.17.0: magic chests of all bases
- ECS systems by name: system descriptors in .rdata are {name, len, fn, ..., dependency
  arrays}. `cache_building_stock` (3fc450, server, deps ItemRegistry, PlayerBasesSystemServer,
  Inventory) and `cache_crafting_stock` (2af3b0, client, PlayerBasesSystemClient).
- PlayerBases system: base count +0x24, records of 0x130 from +0x30 (max 16): +0x00 chest
  ids (u32*), +0x08 count, +0x28/+0x30 a second id list, +0xf0 area box (min xyz, max at
  +0x100), +0x110 mask of sub-boxes (array of 0x20-byte boxes at [sys+0x1330]).
  Server: per-base stock maps at sys+0x1610+i*0x58 (staging +0x1b90). Client: one map at
  +0x1610 (staging +0x1668, chunk counters +0x16c8/+0x16cc, done flag +0x16c0).
  Map: {+8 bit set, +0x10 capacity, +0x28 u32 keys, +0x40 values {u16 count, ...}, +0x54 used}.
- 469060 BaseAt(out, sys, float pos[3]): out[0] = 0 found (index out[8]) or 0x1f.
- 468660 ChestList(out, list{ids,count,ids2,count2}, sysServer, sysClient, pos, player):
  status 0 / 0x1f no base / 0x16 permission (3b1cc0) / 0x12. Callers: 3f1ae0 (charge),
  3f1d70 (check), 2c1a80 (player_crafting), 2b3690 (player_ui, ~4000 calls/s with a
  bench open), 40bf70 (area_deposit_trigger).
- 3f1ae0 charge(C, list, ...): C+0xb1 chest order (0 backpack only, 1 backpack then
  chests, 2 chests first); per inventory 40a3d0(out, C, entity, item, count) -> 40a400
  (missing entity -> status 0x16, skipped). PayCost mode 0 -> 3f1ae0; modes 1/2 (client
  prediction, chosen by 9c5ee0 "is server world" in the blueprint system) -> 4664a0, which
  adjusts the client map at +0x1610 if the position is in a base. 3f20a0: C+0xb0 bit 0x40
  selects the map check (3f2800 via 4695f0) instead of the chest list.
- 4680b0 client iterator(out, sys, pos, player): throttled (+0x16d4 - +0x16d0), returns
  slices of <= 40 ids of the current base's two lists; at the end copies staging into
  +0x1610 and resets (46b150). Readers of the client map: 4695f0 (lea +0x1610) from
  updateInventoryStocks, player_ui, 3f2800; 469610 (missing check 2865e0, map at ctx+0xa0).
- Blueprint system (2d3270): handles PlaceVoxelObjectEvent (0xde627227), which the cursor
  systems only send when the missing-materials check passes. Then dry run, PayCost, and
  PlaceCommit (always after a successful payment), then 48be20(ctx, C, id, ok): applies
  the transaction (408ed0) only if the commit returned true, and sends event 0xbbaebaf7.
- 2865e0 runs in `client_cursor` and in the server's `server_cursor_no_prediction`
  (both through 297510). Its StockCount 469610(map, sys, item, pos) calls (+0x137, +0x2d7)
  read the server's per-base maps, which stayed 0 in testing, also for local materials:
  the server refused materials that were only in another base before paying.
- Tests (3 bases, 15 chests): picker counts and crafting from other bases worked first;
  building needed the server check (v0.17.0 answers 0xffff for the server's system).
  Widening player_ui's ChestList calls dropped the game to 2 FPS with a bench open;
  client worlds are excluded.
- v0.17.0 redirects all calls to 468660 (5), 4680b0 (1) and the two StockCount calls in
  2865e0. ChestList is widened for every caller except the deposit (40c379) and client
  worlds; the client iterator swaps the chosen base's list {ptr, count} for every base's
  chests for the length of the call.

## v0.17.1: review
- Unused lookups removed: WriteTable (48f1c0, its hook was never installed but the mod
  refused to start without it), TearDownEvent (490540), CursorQuery (287b10), BeginUndo.
- The E8 byte scan used to redirect every call of a function matches exactly the real
  calls on this build (checked against the capstone index: PayCost 12, Stocks 2,
  ChestList 5, ClientStock 1).
- Connected replace visits up to 6 * FillLimit + 1 cells; the visited table (2^16) could
  fill up for large limits and every lookup then walked the whole table. Now 2^18.

## v0.17.2: crafting in a base without chests
- Building in a base with no chests worked (server ChestList status 0, 0 local chests,
  widened to every base), crafting at a bench there did not: the menu (player_ui,
  2b3690) sets the transaction's chest order at 2b8c5d: 0 (backpack only) unless the
  ChestList result (2b89e6, client world) has chests ([rbp+0x4e8] or [rbp+0x4f8] != 0),
  then 2. The menu check 3f2030 -> 3f20a0 uses flag 0x40 (stock map path 3f2800), which
  with order 0 only counts the backpack (40a3d0) and never reaches the client map.
- 471290 (handle_crafting_events / factory_station) runs 3f2030 every frame for station
  queues with order 0 and flags 0x8; unrelated to the bench craft.
- v0.17.2: a client-world ChestList result with no chests gets one chest of another base
  (from the client stock's all-bases list), so the menu picks order 2.

## v0.17.3: player report (FillLimit 5000, aim errors with Airborne Architect)
- A failed PayCost of the server's building charge skips the commit silently
  (blueprint system 2d39b2 -> 2d3afd). With FillLimit 5000 the charge for thousands of
  shapes is easily more than the inventory holds; the "FillLimit" banner was the last
  thing shown. The mod now reports refused charges of its own multi-shape actions.
- The pending action's 3 s window started before pricing; pricing several thousand
  replace shapes (removal probe + dry run each) can take longer. Now set after pricing.
- Connected replace chose the material with the most volume in the first shape-sized
  cell, including the material being placed. Now the largest other building material.
- "could not read the aimed block" = aim_lookup failed: no client cursor component
  recorded yet (the client skips its placement prediction while its ghost is blocked,
  which is common when flying), or the client's live aim is more than 6 m from the
  server's place cursor. The log line after it says which; waiting for the player's log.
- Partial actions: 3f1d70(C, entity, count) is NOT an availability check: it asks whether
  inventory `entity` has room for `count` (the craft output), so it failed for every
  building material (first test: "0 of 500 affordable" with 128 in the inventory).
  The take path (40a400 -> 40f4a0 view -> 40a6e0 per slot) writes into the transaction's
  slot copies (4089b0) and its change list (412710 on C+0xf0), so trial charges cannot be
  rolled back. The mod uses the picker's counts instead (stock lists of types 2..5 after
  finalise 4070c0, entries {id, slot, u64 count @+0x10}), binary searches the largest
  affordable shape prefix (cost list prefix at g_pendCost[k]), truncates the list and the
  pending shapes, and rebuilds the Replace refund from a per-shape log of cave volume changes.

## v0.17.4: aim from the per-frame cursor read
- Player log (0.17.3): first Replace click of the session -> "aim lookup failed: no client
  cursor known yet". g_clientCC was only learned in client PlaceShape, which the client
  skips while its ghost is blocked (flying / Airborne Architect).
- Blueprint system 2d3270 loops over hammer players every frame: events (213b50), item
  check e7d430, then cursor read 218720(out = rbp+0xf8, player = [rbp-0x68], cursor
  component = [rbp-0x58]) at 2d340a (inside SIG_CURSOR_CALL, E8 at +15), before the
  PlaceVoxelObjectEvent loop. rbp+0x50 = undo storage, tested at 2d3687
  (48 83 7D 50 00, ServerCheck - 0x6b): non-null only in the server world.
- The mod redirects that one call and keeps the last 4 client cursor components;
  aim_lookup tries each (exact place match, then within 6 m).

## v0.18 (in progress): native in-game menu
- The game's UI is immediate mode: screens are code run every frame. Creative Mode 3.0
  (private "Flame" SDK) uses about 95 of these functions; used only as a map, every
  address below was located and checked in our exe.
- Play-state UI `c4fc40` ("doPlayStateUi()"), one caller `bc24ba`, no arguments used.
  hud = [[[ui ctx 281e498]+0xd0]]+0x38 (same ui ctx as the sounds); current
  `HudScreenType` byte at hud+0x1cd8 (1 InGame, 51 NoUi skips the UI, 0 Invalid).
  Opens window `PlayStateBaseWindow` (beginWindow a32b50(win, &StringView, 1, 2, -1, 0)),
  named box `doPlayStateUi()`, then dispatches on the screen at `c5028b`:
  52 TechGallery -> `cc6ad0` (call at `c50a9e`), 53 YcPlayground -> `ce77d0` (call at
  `c50a97`, used by Creative Mode), others below.
- `ecs::OpenHudScreenEvent {+8 playerEntityId, +0xc interactionHostId, +0x10 screen}` is the
  game's own way to open a screen (not used yet).
- TechGallery `cc6ad0` is a developer test screen ("Test1"), a complete minimal example:
  beginWindow a32b50(&win, &StringView{"Test1",5}, 0, 0, -1, 0); ctx = windowContext a3eb90(&win);
  beginRoot a301b0(&box, ctx); ... beginText b0e280(&text, &TextString, style, 0) with
  style = [ui ctx]+0xc8 (bundle) + 0x10a0c; endText b15a60; endBox a336a0; endWindow a330b0.
- StringView = {const char*, size_t}. TextString = {const char*, size_t, uint64 hash},
  hash = FNV-1a 64 of the text (named boxes use the same hash of their name).
- Test (dev1, writing the byte directly): screen 52 showed the test screen (red corner
  squares, check mark, "dies ist ein test"), the HUD disappeared, Home closed it, Esc went
  52 -> 2 (SystemMenu). No cursor and the character still walked: input/cursor are set
  on the screen transition, not per frame.
- `d60260` toggleHudScreen(ui ctx, screen): queues message type 1 on [ctx+0xd0]+0x33cc0
  and logs "[GameUi] toggleHudScreen to %k"; used by the game to open/close screens
  (85 callers). dev2 opens/closes screen 52 through it.
- `1205610` isMenuScreen(screen) (jump table): true for every menu-like screen, including
  TechGallery and YcPlayground. `12056b0` is true for screens 29..37 (multi-tool menus).
- Test (dev2, toggleHudScreen): cursor shows on screen 52 (isMenuScreen is true for it),
  but camera and walking still work and Esc goes to the system menu.
- Window input: beginWindow stores layer at window+0x7c, inputMask at +0x74, flags at
  +0x78. Flags bit 0 is modal: the UI system skips input for windows on lower layers
  while a modal window on a higher layer covers the same input mask (`118b8a0`,
  `11be190`). Every real menu opens its window with fullscreen=1 and flags=1 (layer
  9 for YcPlayground, 0x16 or 0xb for others); TechGallery uses fullscreen=0, layer 0,
  flags 0, so the world keeps the input. dev3 redirects the test screen's beginWindow call
  (screen fn + 0x73) to open it fullscreen, layer 9, modal.
- Other screen predicates: `1205720` true for 2, 3, 38, 39, 40, 49, 50; `1205830` true for
  29, 31-33, 36, 37; `1205690` true for 21, 22, 41.
- Test (dev3, modal window): mouse works, camera and walking stop, Esc and Home close it.
- Popup recipe (`c6a820`, reportSharedWorldPopup): beginWindow(fullscreen 1, layer 0x1b,
  mask -1, flags 1); beginRoot; root stretch (1,1) (`a3d000`); root background
  {0x80000000, 1.0} (ARGB, black at half alpha); beginBox panel; padding 90/60/90/60;
  spacing (0,30); selfAlignment 0x202 (centre); layout 0x102 (column); fancyBox(panel,
  params 0xb8: style 8, scale 1.0, rest 0); content; endBox x2; endWindow. Text styles
  in the UI bundle ([ui ctx]+0xc8): +0x11b94 (title), +0x10a0c (body), +0x11bec, +0x10abc.
  Node field writes: layout u16 @+0x6e, selfAlignment @+0x6c, stretch @+0x90,
  padding @+0xa8. dev4 draws the mod's own panel this way.
- Test (dev4): the panel draws with the game's frame and fonts; the root background
  dimming did not show (to look at later).
- settingOption (`ad0d70`, found by its call at `c44c59`; the function start has an
  identical twin): params 0xc48 bytes. Entries of 0x30 {u32 value, TextString label @+8}
  from 0, entry count u8 @0xc00 (max 64), row label TextString @0xc08, description
  TextString @0xc20, dword 0x10000 @0xc38 (arrows byte @0xc3a), dword 1 @0xc3c
  (interactive), byte 0 @0xc40. Returns true when the u8 value changed.
- settingHeader (`c8c6e0`)(TextString* title, float gap). dev5 draws the tool settings
  (mode, replace, style, fill, free placement) with these.
- Full-screen menus (HudMenu, `bd933c` chunk): blurBehind(ui ctx, node rect, {0xffffffff,
  1.0}) then a dark tint fillRect(`cf8a00`)(rect, colour from bundle+0xfa74), side images
  at bundle+0xd300, menuTabBar, drawHints. Node rect = node+0x114 (last layout).
- Layout/selfAlignment u16 values are alignments (low byte x, high byte y; 1 start,
  2 centre, 3 end). Direction comes from spacing: setSpacing (`11b3810`, [node+0xb8]=2)
  stacks vertically, setSpacingX (`11b3480`, [node+0xb8]=1) side by side.
- doButton (`cc8fb0`) params 0xa8 as the Deletion Confirm menu fills them: +4 dword
  0x10202, +8 word 0x100, +0xa warning byte, +0x10 qword 3 (style), +0x4c 10.0,
  +0x50 5.0, +0x54 1.0 (scale), +0x58 enabled, +0x78 TextString. Returns true on click.
- dev6: full-screen menu (blur, dark tint, menu at the left), Keys and Options tabs,
  key rebinding saved to the ini; opens from InGame and multi-tool screens 29-37.
- Test (dev6): rebinding and options work, Home works from the hammer (screen 29).
- menuTabBar (`bdaf40`)(menu, flag): the top tab bar of the full-screen menus (Q/E).
  Menus 3, 25, 27 always take the tab path. It fills a vector {data, count, capacity,
  grow fn} (inline room for 10) with menuTabList (`d11e90`, called at `bdb2a4`) and
  draws each tab with `cd0100` (called at `bdb719`; true = clicked, then `d67dd0`
  switches the game's screen). Tab 0x40 bytes: shortcut @+1 (0x8b none), label
  TextString @+0x18, active @+0x30, hover @+0x31, interactive @+0x32, opacity @+0x34.
  dev7 hooks both calls while it draws its own bar (menu 3) to show its pages.
- Test (dev7): tab bar looks and works like the game's; combos and columns work.
- Hint bar: HudMenu collects hints for the frame in a list at [ui ctx+0xd0]+0x32e18
  (16 x 0x48, count u8 @+0x480) and draws them with drawHints (`c2a9b0`)({list, count},
  0x303, padding {0,10,0,0}, 0, 0). makeHint (`b14b00`)(hint, UiActionId, enabled);
  custom label flag @+0x1b, label TextString @+0x20. UiActionId (types.txt
  keen::input_mapping::runtime::UiActionId): 0 Confirm, 1 Back, 10 NextTab,
  11 PreviousTab, 24 BindingReset, 25 BindingClear. dev8 draws a Back hint ("Close").

### Building HUD box rows (0.18.0-dev9)
- Building HUD box `b5d4e0` (named box at rbp+0x20). Key rows via `bc4e20`(u8 UiActionId, params 0x78)
  at b5dc6a and b5ddf2 ("Undo Build Action"); box finished by E8 at b5de4b (`11b3cf0`, rcx = &box),
  right after Creative Mode's hud.buildingAreaRows site (b5ddf7). bc4e20 looks the binding up
  (`e74570`(out 0xc4, inputMap = [[ui]+0xd0]+0x33cd0, action), 0x118 per action) and calls
  bindingHint `b2f8b0`(binding, params).
- Binding 0xc4: keyboard combos @0 and @0x30, bytes @0x60/0x61 (0x60 == 2: hold, progress), gamepad
  combos @0x62 / @0x92, bytes @0xc2/0xc3. Combo 0x30 = input (12 bytes) + up to 3 modifier inputs.
  Input: device u8 @0 (1 keyboard, 2 mouse, 6..8 gamepad), key u16 @2 = USB HID usage (SDL scancode:
  PageUp 0x4b, LeftControl 0xe0, LeftShift 0xe1, LeftAlt 0xe2; name parser `e83cd0`), mouse button
  u8 @4 (1 L, 2 R, 3 M, 4, 5). bindingHint skips the row if combo device is 0 and params+0x2c == 0.
- Row params 0x78: +0 combo index, +6 custom align flag (+4 u16), +0x28 flag, +0x2f = fill label from
  the action's name (bc4e20), +0x30 flag, +0x3c 1.0, +0x44 500.0, +0x48 hold progress (+0x4c set),
  +0x54 24.0, label TextString @0x58, colour {ARGB, intensity} @0x70.
- On/off switch `cf05d0`(bool* value, u8, u8) -> bool clicked; draws FbUiMenu.toggle textures
  (bundle +0xe860 menu, +0x1044 toggleOn/toggleOff). Not used: Aleksander prefers the bottom-left HUD mark and text On/Off options.
- Small on/off mark of the bottom-left HUD slots (c28190 @c284bb): box 21x21 (setSize 11b32e0), image ctx
  a3ddf0(), resolveImage a3eef0(ctx, &handle, texture), drawImage a3aab0(box, handle*, colour*);
  fancyToggleSmallOn bundle+0xfed4 (white), Off bundle+0xfec4 (colour qword bundle+0xd4d8).
- Building box title (b5d9a6 beginText): loca hud.building.buildTooltipAreaActions (bundle+0x41f0), empty
  in the game; dev11 replaces its string with mod messages and the area size.
