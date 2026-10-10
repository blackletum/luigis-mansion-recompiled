# Unused content in the retail code (GLME01)

Everything here also works on the European game (GLMP01): addresses go through `VA()` and
the hooks are tagged `us pal` (docs/SETUP.md, *Version-aware code*). European differences:
the GBH sub-view clear is `fn_8000B7CC` there (no hook needed); the end-of-frame hook is the European
`LMChangeFrameBuffer` (`fn_80007940`).

Two pre-release features are still in the US `main.dol`. Both are switched on from
**F1 > Enhancements** and are implemented as hooks that call the original function and
only change which of the game's own code paths it takes (`src/recomp/unused_content.cpp`,
`tools/recomp/hooks.txt`). No game code was rewritten. Addresses are GLME01 rev 0, and the
names are the dtk/symbols.txt ones.

## Game Boy Horror live camera ("Game Boy Horror camera")

Pre-release builds (Nintendo Power 2001 screenshots, the E3 2001 demo) showed a live
first-person view on the Game Boy Horror screen in the bottom-right HUD, so the player saw
the room from two angles at once. The retail game still has the whole pipeline behind a
screen-mode variable that it never sets to the camera value.

### How it works in the retail code

GBH HUD state lives in `lbl_803CE2D8` (sprites at +0x48.. frame `cgbh.bti`, +0x60 shadow,
+0x78 `back.bti`, +0x90/+0xA8/+0xC0 LED pieces; +0x48/+0x4C hold the frame's x/y).
The screen-content mode is `lbl_804D846C`:

| mode | set by | effect |
|---|---|---|
| 0 | never | renderer viewport hook only (`fn_80010EBC`), no extra 2D |
| 1 | never | The cut E3 clock + radar border (`fn_80044C4C`: 16-segment minutes/seconds, border segments animated by the ghost tracker). The mode switch at 0x8003D53C sends modes 0/1 to the same viewport code as the camera, so the 3D view covers it unless instruction patch 3 (0x8003D544) skips that; exposed as "Game Boy Horror E3 clock" (D-pad Left) |
| **2** | **never** | **live camera**: see below |
| 3 | `fn_8003EA78` (E. Gadd's call starts) | call portrait at the screen rect (`fn_80115474`, 110x84) |
| 4 | init `fn_8003CEB4`, `fn_8003EA84` (call ends) | the retail screen (`fn_800571B8`) |

Mode 2, in `fn_8003CFC0` (HUD update, every frame):
- the GBH is raised to its normal on-screen height;
- `fn_80010EBC(x+0x18, y+0x16)` stores the screen rectangle (110x82 at 640x480) in
  `lbl_804D0008/804D000A/804D8162/804D8164` and sets `lbl_804CFFF0 = 2`. It returns 0
  (screen off-screen, or map 0/1), and then `lbl_804D8440` turns on the static-noise overlay
  (`gbnoise01..03.bti`, drawn by `fn_8003E3D0`/`fn_8003E7B0`).

Renderer side (`fn_80010F20`, called each frame from `fn_800112F8`):
- `lbl_804CFFF0 == 2` sets the view count to 2 (`fn_8000AE04`). View 1
  (`lbl_803A4C80[1]`) gets the GBH screen rectangle as its viewport, and the flag is reset
  to 1 every frame, so the HUD has to request it again each frame.
- The frame loop `fn_8000BCE8` draws each view (`fn_800115F8(i)` sets the viewport; for
  view 1 it also calls `fn_8000B228`, the sub-viewport scissor/clear). View 1 draws the map
  and actors (`fn_8000BB98(1)`: `fn_80011824` + `fn_80060E70(2)`), then `fn_8003E7B0(1)` puts
  the noise on top when needed.
- View 1's camera was set up for single player in `fn_800204B4`: update callback
  `fn_80021DDC` → `fn_80021B04`, a first-person camera. The target is Luigi's position
  + 180 up, the eye is 100 units along his facing vector (flipped when he is upside-down,
  `unk28 == 1`), the FOV is 50 and near is 10. The same camera code is reused by the shipped
  first-person GBH look mode (`fn_80021C68` when `lbl_804D8160`).

So every piece is present: HUD mode, viewport handshake, second view, camera, noise.
Only the write of 2 to `lbl_804D846C` is missing.

### What the port does

`HLE(fn_8003CFC0)`: when the option is on, D-pad Up (edge on port 1, read from the PADRead
HLE) toggles the camera. While it is on, mode 4 becomes 2 before the original runs, and 2
goes back to 4 when it is switched off. Mode 3 (E. Gadd's call) is never overridden, so a
call still shows the portrait and the camera returns when the call ends. All rendering is
the game's own code. Test aids: `LM_GBH_CAM=1` starts with the camera on, without the
setting. `LM_GBH_CAM=2` also forces the GBH HUD up (player +0x55) before Luigi has the
Game Boy Horror, so the opening foyer can be used.

Depth reset: the view-1 clear `fn_8000B228` resets depth with
`GXSetZTexture(GX_ZT_REPLACE)` from an all-0xFFFFFF Z24X8 texture. Aurora now implements Z
textures (patch 0008, docs/RENDERING.md), so the game's own clear works; the earlier
workaround hook (moving the clear quad onto the far plane) is gone.

Verified: in the foyer (`LM_GBH_CAM=2`, foyer at ~2 min under load), the GBH screen shows
Luigi's view of the staircase and dresser, and D-pad Up from the scripted input toggles it
(`[unused] Game Boy Horror camera on` in the log).

Limitations: ghosts are drawn in view 1 only by the direct pass
(`fn_80060E70(2)`; the ghost glow layer is view-0 only), so on the GBH screen they appear
with the early direct shading described below. The camera image is not mirrored in the
PAL mirror mode.

## Early ghost shading ("Early (opaque) ghost shading")

Retail ghosts get the glowy see-through look from a separate **ghost layer**:

1. `fn_8000BB98` draws the map and normal actors (`fn_80060E70(0x10)`: pass 0x10 skips actor
   types with type flag 8, which is every ghost; flags are in the type table `lbl_8033B590`,
   0x1C bytes per type, read by `fn_8006A3BC`).
2. `fn_8000C2D0`: `fn_8005F518` copies the scene to `lbl_804D86BC` and clears the EFB.
   `fn_80060E70(0x30/0x130)` draws only the ghosts, writing each ghost's alpha
   (MoveObj +0xE3) as destination alpha. `fn_8005F498` copies that layer at half resolution
   (320x240 RGBA8) to `lbl_804D86B8`.
3. `fn_8005F5B0` puts the scene back, and `fn_8005F280` adds the half-res ghost layer
   **additively**, weighted by its alpha (blend SRCALPHA, ONE).

The result is a soft, blurred, light-emitting ghost that never fully covers what is behind
it.

The older path is still in `fn_80060E70`. With pass bits `r3 & 0x30 == 0`, every actor,
ghosts included, is drawn directly into the scene: ghost lighting (`fn_800581E8`: ambient
from the ghost's colour +0xE9, its own glow light), full-resolution S3D material
(`fn_80058C30`, alpha = material alpha × ghost alpha), normal alpha blending. Retail only
uses this pass for the never-shipped GBH camera view (`fn_80060E70(2)`) and the event
renderer (`fn_8000F1F8`). This is the solid, more opaque ghost look of the E3 2001
material.

### What the port does

`HLE(fn_80060E70)`: when the option is on, the 0x10 pass runs as pass 0 (normal actors plus
ghosts, drawn directly). The 0x30/0x130 ghost-layer passes run with pass bits 0x20, which no
case handles, so the layer stays empty and the additive composite adds nothing. Everything
else (per-ghost alpha fades, vacuum flashes, colours) is the game's own data and code. Test
aid: `LM_EARLY_GHOSTS=1`.

Verified with `LM_START_MAP=11` (Boolossus balcony). Retail: a glowing, see-through Boo with
Luigi visible through it. Early shading: a solid grey Boo that hides what is behind it.

Not reconstructed: the shipped GBH first-person "ghost scan" pulse (dst-alpha pulsing in the
0x30 pass, `fn_800E9E48`) has no effect while the option is on.

## Unused assets (`src/recomp/unused_assets.cpp`)

All of these are under F1 > Enhancements > Unused content and are off by default. The hooks
are US-only (`tools/recomp/hooks.txt`) and gated with `port::usCodeFeature()` in the menu.
Data patches only overwrite a value that matches the expected retail original (or the patched
value when you switch an option off). Anything else is left alone and logged once.

### Beta skybox ("Beta skybox")

- Retail: `fn_8002F924` picks the sky descriptor from `lbl_80300C7C[sCurMapNo]`. For map 2
  that is `lbl_80300B60`: archive `/Iwamoto/vrball_M.szp`, model `/vrball01.bmd`, one texture
  animation `/vrball01.btk`. `fn_800067D0` mounts the archive, and the callback
  `fn_8002F4DC` loads the resources through the archive's `getResource`.
- Unused: the always-mounted game archive (`/Game/game_usa.szp`, returned by
  `fn_80066FB4`) contains an older `iwamoto/vrbox/vrball01.bmd` (38752 bytes; the retail one
  is 41888) with its own `vrball01.btk`. No code references that path.
- Port: `HLE(fn_8002F4DC)` marks the load when the descriptor is `lbl_80300B60`. During that
  load, `HLE(getResource__10JKRArchiveFPCc)` sends `/vrball01.bmd` and `/vrball01.btk` to the
  game archive at `/iwamoto/vrbox/...`. The sky then loads, animates and draws through the
  game's own code. Applies to map 2: the opening, the grounds and every window/balcony. The
  boss skies (`vrball_B`, `vrKoopa`) are untouched. Takes effect on the next map load. Test
  aid: `LM_BETA_SKY=1`.
- Verified: the redirect fires (`[unused] beta skybox: ...` in the log). On screen the older
  sky is very close to retail (same cloudy green palette), so the visible difference in the
  opening shots is small.

Verified in game (`LM_TELEPORT`, empty card): the Nursery shows the heart door and the Twins'
room the star door; retail shows the plain brown door in both. The GBH map (Y, then A to
zoom) shows a small full-body Luigi instead of the big head.

### Heart door / Star door

- Map 2's doors are a static table in the DOL: `lbl_802FF218[2]` → +0x18 = `0x80302F80`,
  0x1C bytes per door. Byte +6 is the door type. It selects the model, via
  `lbl_802FF95C[type]` → `lbl_802FF868` = `/iwamoto/Door/door_NN.bin` in the game archive.
  It also selects the lock/unlock cutscenes: `fn_8011496C` plays scene
  `3 + (type-1)*4 + 2*a + b` in `lbl_80373AA4`, which maps to
  `oodemoNN/codemoNN/hodemoNN/nodemoNN.szp`.
- Types 07 and 08 have a model (`door_07.bin`, `door_08.bin`) and all four cutscenes
  (`/Ajioka/ADemo/{oo,co,ho,no}demo07/08.szp`), but no door in any map uses them.
- Door 46 (`0x80303488`, position -3005/700/-710) is the door from the 2F hallway into the
  Nursery (Chauncey's room). Door 45 (`0x8030346C`, -2190/700/-710) is the Twins' room door.
  This was checked against furniture positions: `bed24`/teddy/rocking horse at x ≈ -3700,
  `bed25`/toy car at x ≈ -1300, both south of the hallway wall at z = -710. Both doors ship
  as type 01.
- Port: `HLE(fn_800122BC)` (map setup, before the doors are built) sets the type byte to 07
  (heart, `0x8030348E`) and/or 08 (star, `0x80303472`). Because the type also picks the
  cutscenes, the game plays the unused locked/unlock scenes for that door by itself. Takes
  effect on the next map load. Test aids: `LM_HEART_DOOR=1`, `LM_STAR_DOOR=1`, and
  `LM_TELEPORT=x,y,z,frames`, which moves Luigi once, for example
  `-3005,560,-400,60` = inside the Nursery facing the door.

### Game Boy Horror map: full Luigi model

- Retail: the GBH map (`fn_8002967C` for map 2, `fn_8002B310` for the training maps) loads
  nine files from the guide-map archive (`guidemap.szp` in the DOL's static data, in ARAM)
  through `fn_8001F8D8`. File 1, `/gide_lui.bin` (43968 bytes), is the big Luigi head drawn at
  Luigi's position (model `lbl_804D830C + 0x318`, placed by `fn_8002B140`).
- Unused: the disc still has an older guide-map archive, `/Iwamoto/map2/gidemap.szp`
  (gidemap/gide_d1/gide_d2/gide_lui/g_d1x/g_d2x/g_key1). Its `gide_lui.bin` (24288 bytes) is
  a full-body Luigi figure. Nothing loads that archive.
- Port: on first use, the host reads `/Iwamoto/map2/gidemap.szp` from the disc (Yay0/Yaz0 +
  RARC, original code), copies `gide_lui.bin` into guest memory (`hleAlloc`) and returns it
  from `getResource("/gide_lui.bin")`. `HLE(getResSize__10JKRArchiveCFPCv)` reports its size.
  The game parses and draws it like the retail marker. Test aid: `LM_BETA_MAP_LUIGI=1`.

### Portrificationizer: each ghost's own model

- Table `lbl_8036E52C`, 0x18 bytes per portrait ghost, read by `fn_80121E14`/`fn_80124890`.
  Field +4 of the 21 entries at `0x8036E548 + 0x18*i` is `0x60` (the generic blue blob)
  everywhere. The values the known Gecko code writes (`0x64`, then `0x121`, `0x125`, …,
  `0x16D`) are each ghost's own unused portrait-scene model id.
- Port: both readers are hooked and write the table first (only over `0x60` or the patched
  value), so it is correct whenever the scene reads it and goes back to `0x60` when the option
  is off. Test aid: `LM_UNUSED_PORTRAITS=1`.

### Air and water nozzles ("Air and water nozzles")

`/model/wplight.szp`, `/model/wpwater.szp` and `/model/wpair.szp` are character types 7/8/9
(`lbl_8033B590`, right after `vbody` 5 / `vhead` 6). Each has a model-registry entry and its
own attach table for Luigi in `fn_80078008` (`lbl_80354290`, `lbl_803543B4`,
`lbl_803544D8`; Luigi's other character types 1/0xB3 have a parallel 0xB5..0xB9 set).

How the retail code uses them:
- Luigi's equipment is created in `fn_80072554` (types 6, 5, 0x0A `gameboy`) and attached by
  `fn_800AE1A4`: nozzle at Luigi +0x7A4, tank +0x7A8, GBH +0x7AC.
- The nozzle is one actor whose character type is changed on the fly: `fn_800B9064(luigi,
  type)` → `fn_800B90D0` → `fn_80067B04` (MoveObj re-init with the new type's model).
- Luigi's Poltergust mode selector `fn_800B36B8` sets +0x1180: 1 vacuum (R, input object
  +0x7D4 +0x20 > 0), L (+0x2C > 0) → the element +0x1184 (2 fire, 3 water, 4 ice; 1 = no
  element → 6 air), 5 flashlight. On a change it re-types the nozzle: **5 → type 7
  `wplight`** (so wplight *is* used in retail, as the idle/flashlight nozzle), every other mode
  → type 6 `vhead`. Types 8/9 are never selected.
- Vacuum, air and element spray all work in ordinary rooms with `LM_TEST_STATE` (checked in the
  Nursery and in the foyer after the opening). The earlier "L/R do nothing" runs pressed the
  buttons before gameplay started (still in the opening/dialogue).

Port (`src/recomp/nozzles.cpp`, hook on `fn_800B36B8`, US code only): the selector runs
unchanged; afterwards, with the option on, the nozzle is re-typed through the game's own
`fn_800B9064`: mode 3 (water) → 8 `wpwater`, mode 6 (air) → 9 `wpair`; when the mode leaves
them (or the option is turned off) it goes back to 6/7 as in retail. Vacuum, fire and ice
keep `vhead`, the flashlight `wplight`. The option is a plain on/off switch (no button: the
D-pad is kept for the early Game Boy Horror modes). With the option off the hook only calls
the original (types stay 6/7).

The models have to be loaded first. Character models live in the registry `lbl_8034E46C`
(0x34 per model: +0 file name, +8 relocated `.mdl` once loaded, +0x30 load state; the type
table `lbl_8033B590` +0 gives a type's model index: `vhead` 1, `wplight` 2, `wpwater` 18,
`wpair` 19). Map setup (`fn_8006227C`) clears the registry and loads only the models the map
needs (resident archives directly, others through `fn_800620F4`); event code loads extra
models the same way (`JmpMessage`, `0x800EB5A0`). `wpwater`/`wpair` are never requested, so
re-typing the nozzle to them without loading left the S3DModel with no model data: the first
version of this option drew only a thin grey stub in Luigi's hand. The hook now requests both
with `fn_800620F4__Fi` while the option is on (again after every map change, since the
registry is cleared) and only re-types once the registry entry has its model. Drawn standalone
in the model viewer (`LM_DEV_VIEWER=/model/wpwater.szp`), the models are a long grey tube,
dark ridged collar, bulging blue (`wpwater`) or red (`wpair`) section and white tip, 88 units
long like `vhead`, with the same three joints and key names.

Verified (`build/nozzles_side.png`, native guest frames from `LM_FRAME_DUMP`, Nursery,
`LM_START_MAP=2 LM_START_APPEAR=1`, Luigi turned sideways with the stick): retail water
(`vhead`), water with `wpwater` (blue), air with `wpair` (red). The nozzles sit in Luigi's
hands like `vhead` and the water
stream and air puffs come out of their tips. Test aids: `LM_NOZZLES=1` (force on),
`LM_NOZZLE_DUMP=1` (logs mode/element/meter/nozzle type on change, plus the Poltergust
controller `[Luigi+0x7E4]` +0x310/+0x314/+0x334 and its 0x50-byte input block).

What the game itself never finished:
- No code path selects types 8/9 or loads their models; the mapping water → `wpwater`, air →
  `wpair` is inferred from the type names, their models and their attach tables. There is no
  `wpfire`/`wpice`, so fire and ice keep `vhead`.
- The three attach tables (`lbl_8035416C` `vhead`, `lbl_803543B4` `wpwater`, `lbl_803544D8`
  `wpair`) are identical apart from their animation lists (same joint 0x0E, same values), so
  keeping the `vhead` attachment loses nothing.
- Air and water behave exactly as with `vhead` (same effects, same element meter); nothing
  in the code gives the dedicated nozzles different gameplay.
- Luigi's other character types (1/0xB3) have 0xB8/0xB9 entries too; the port leaves them alone.

### Unused model swaps (`src/recomp/unused_models.cpp`)

Character models are loaded per map into the registry `lbl_8034E46C` (0x34 per model: +0 file
name, +4 archive, +8 relocated `.mdl`, +0xC key count, +0x10 key list, +0x30 load state) from
the resident game archive when it has `model/<name>.arc`, otherwise from the loose disc file
`/model/<name>.szp`. `fn_80061C94(archive, entry)` sets each one up: the `/key` files into the
key list in archive order (relocated by `fn_80061A24`), then the `.mdl` into +8 (relocated by
`fn_80061B40`). It runs for resident archives directly from map setup (`fn_8006227C`) and for
disc archives from the DVD callback (`fn_800620CC`). The port hooks it (US code only): after it
finishes, for an enabled swap the entry gets a relocated host-loaded copy of the unused `.mdl`
(read from the disc with the port's RARC/Yay0 reader, copied into `hleAlloc` memory, relocated
by the game's own `fn_80061B40`), and each key slot the unused archive's key for the same
action. Actors built afterwards use it like any other model. Takes effect at the next map load.

#### Early Poltergust 3000 ("Early Poltergust 3000")

Two `vbody` models exist. Retail uses `model/vbody.arc` from the resident `game_usa.szp`
(34808-byte `.mdl`, the rounded red tank with the big silver wheel); `/model/vbody.szp`
(25764-byte `.mdl`) is the E3 2001 Poltergust, a red canister with dark end caps and silver
knobs on a base with four black wheels (yellow hubs). The loose file is never read because the
registry's `/model/vbody.szp` name resolves to the resident `.arc` first. Both have 4 joints.
The E3 archive's keys have the same names as the retail ones except that it has no
`lght_vby.key` (flashlight pose), so that slot keeps the retail key; the other six are the
E3 ones (its `h_vbody` idle is a one-frame pose instead of retail's 30-frame wobble).

Verified (`build/early_poltergust.png`, Nursery, guest frames): retail tank from the side, the
E3 canister from the back (four wheels) and from both sides. It sits upright on Luigi's back on
the retail attachment; the hose (drawn by the player code between the tank and the nozzle)
starts at the top of the canister and still lines up. Vacuuming works as before. Test aid:
`LM_EARLY_POLTERGUST=1`.

Not finished in the game: nothing references the E3 model any more; how it was meant to sit
on Luigi's back (upright, as the retail keys and its own keys place it) can't be checked against
E3 footage here.

#### Early Bogmire ("Early Bogmire model")

`/model/shadow.szp` (registry model 129) is the early Bogmire; no character type uses it.
Retail Bogmire is type 213 `bfire` (model 130, `/model/bfire.szp`, a disc file loaded for
map 13, the cemetery boss arena, and listed in map 2's `characterinfo`). The two skeletons
differ (shadow 42 joints, bfire 41), so bfire's keys can't drive the early model; every bfire key
slot gets the shadow key for the same action: `appear` -> `syutugen` (appear), `dappear` ->
`kieru` (vanish), `wait`/`dwait` -> `wait`, `nigeru`/`nigeru2` (pulling against the vacuum) ->
`kinko` (the 12-frame tug of war, same length as `nigeru`), `biribiri` and `suikomi` by name.
The early archive's `kogeki` (attack), `kuyasi` (frustrated) and `tyakasu` keys have no retail
slot and are not used. The retail `geb/bfire.geb` and `.bas` (sound timing) files stay in use.

Verified: model viewer (`build/bogmire_viewer.png`: `bfire` above, `shadow` below with the
longer wings and thin hanging flaps, the defined chin and the wavy skirt) and the map 13 intro
(`build/bogmire_intro.png`, `LM_START_MAP=13`, same guest frames retail/early). The fight runs
with the early model (60 s with `LM_SKIP_CUTSCENES`, shadow clones and Bogmire's appearances,
no errors). Not checked: whether any Bogmire effect is attached to a joint by index (with one
more joint, such an effect could sit on a different body part); the Portrificationizer and the
gallery never use `bfire` (portrait scenes use their own models, the gallery a painting).
Test aid: `LM_EARLY_BOGMIRE=1`.

### Green Toad ("Green Toad (kinopio2)")

Types 42 `kinopio` and 43 `kinopio2` both use model 0x49 (`/model/kinopio.szp`); kinopio2 is
never placed. The archive has two texture patterns (`txp/kinopio1.txp`, `kinopio2.txp`, loaded
per model by `fn_80187B24` into the list at `lbl_80389B68 + model*0x40`); a type's +0x10 picks
one when an actor is set up (`fn_80067B04`/`fn_80067F84` -> `fn_80187974` -> S3DModel +0xA4):
kinopio 0, kinopio2 1 (green spots). TCRF's NTSC-U code `0433BA28 00000049`,
`0033BA31 00000002`, `0433BA38 00000001` writes kinopio's model index, its +9 byte and +0x10;
on GLME01 rev 0 the first two already hold those values, so the effect is kinopio +0x10 = 1.

Port: that value is written at every map setup (`fn_8000C188` hook), and because Toad's archive
is a disc file loaded asynchronously (the Toads placed at map load are set up before their
`.txp` files exist, so the code alone leaves them red), every live Toad's S3DModel +0xA4 is set
to the green pattern once it is loaded (GBH HUD hook, every gameplay frame). Dialogue icon:
`fn_80041DB8` loads 19 speaker icons named by `lbl_80305148 + 0x360` into 0x18-byte sprites at
`lbl_803CE7D0 + 0xA90` (+0x10 = texture object). Despite the names, every Toad uses entry 16,
`kino_ri.bti` (red), and entry 15, `kino_icon.bti`, is a green-capped Toad that is loaded but
never shown; the option points sprite 16 at sprite 15's texture. Everything is put back when
the option is turned off. The addresses go through `VA()`, so it also works on the European
game (tables checked in the PAL DOL: same layout, kinopio/kinopio2 at types 42/43, icons 15/16).

Verified (`build/green_toad.png`): the Washroom Toad (`LM_START_MAP=2`,
`LM_TELEPORT=-2210,0,-4150,100`, A presses) talking, retail vs green: model and dialogue icon
both green. Test aid: `LM_GREEN_TOAD=1`.

### Early Luigi model ("Early Luigi model")

The disc root's `/model/luige.szp` (TCRF: the early Luigi) is never loaded. The model
registry `lbl_8034E46C` (0x34 per model) has `/model/luige.szp` as entry 0, but map setup
(`fn_8006227C`) first asks the resident game archive for the same name with `.arc`
(`game_usa.szp` → `model/luige.arc`) and reads the disc file only when that is missing.
`fn_80061C94` then fills the entry from the archive: every `/key`, `/sls`, `/slk` and `/txp`
file (keys are stored by archive order, 0x8D slots), the per-model tables of `fn_80187B24`,
and last the `/model` `.mdl`, relocated by `fn_80061B40` into entry +8.

Comparison (archives extracted from the user's disc):
- Skeleton: identical. 65 joints, 55 envelopes, the same node graph, inverse bind matrices
  and skin weights, so every retail animation drives the early mesh unchanged.
- Animations: early 121 keys, retail 141, 87 names in common (63 byte-identical). Early-only:
  squat set (`squ_*`, `k_squ_*`), `l_tdm01-07`/`l_sdm01-04` (the lab cutscene keys that
  retail moved to `dluige01`/`dluige03`), `turi09_1-3`/`turi11`/`turi12` (hose-pull
  variants), `wait2`, `walk1`, `walk2`, `b_kam_wt`, `bibi_kam`, `bibi_set`, `ldr_wt_e`.
  Retail-only: the `ac_*` actions (knock, call, punch, ...), burn/ice/poison/bite reactions,
  elevator, `get_*`, `lookheat`, `dmydr_*` and others. The game indexes keys by archive
  order, so the early key set cannot be swapped in; the retail set is kept.
- Mesh: same vertex counts per type except texture coordinates (0x5FA vs 0x5F6); 19
  materials vs 16, 20 textures vs 21, 26 samplers vs 30. Visible differences: the face
  (bigger, rounder nose; fuller lower face and chin), ears, the hair at the back and its
  texture (texture 4), cap shape (`build/early_luigi_viewer_heads.png`).
- Eyes: the `.txp` texture patterns (blinks, expressions) store *sampler* indices. Retail
  eye textures 10, 14-18 exist pixel-identical in the early model under other indices; retail
  19 (looking eye, used by `lookheat`/`ac_call`/`doku_lp`) and 20 (the dark "poisoned" eye of
  `doku_*`) do not exist (`build/early_luigi_eye_textures.png`). The early model's own extra
  eye (its texture 19) is pixel-identical to retail 18.

Port (`src/recomp/early_luigi.cpp`, hook on `fn_80061C94`, US code only): the original runs
unchanged; then, if entry +8 is a relocated `.mdl` with exactly the early model's skeleton
(counts, node graph, inverse binds), +8 is pointed at an early copy built for it. The copy
is the early `.mdl` (read once from the disc with the host RARC/Yay0 reader, `rarc.h`) with
a new sampler table laid out like the target model's: each target sampler points at the
early texture with identical pixels, or at a copy of the target's texture appended to the
early file (retail 19 and 20, and the unused retail 4); the early materials are re-pointed
at matching entries, so the retail `.txp` files show the same eyes. The game's own
`fn_80061B40` relocates it. Copies live in host-backed memory above MEM1
(0x81F00000-0x81FF0000, taken from the end of the model viewer's heap), one per target
layout. Everything else (keys, `.txp`, `.slk`/`.sls`, the per-model tables) is retail.
Takes effect on the next map load (the registry is rebuilt at every map setup); with the
option off the hook only calls the original. Test aid: `LM_EARLY_LUIGI=1`.

Which Luigis change:
- Gameplay Luigi (registry entry 0) in every map, including the in-engine event scenes in
  the mansion and the lab that use him.
- `dluige03` (the lab cutscene Luigi with the Poltergust, map 4/1 events) and `dluige01`:
  same skeleton and bind pose, loaded through the same registry; each gets its own copy
  (`dluige03` has 22 textures, so texture 21 is appended too). Verified for `dluige03`;
  `dluige01` was not reached in testing but takes the same path.
- Not changed: `dluige02` (ending; same graph but different inverse binds) and the demo
  scenes under `/Ajioka/ADemo` (opening forest/gate: `opwf_luigi.mdl`/`entergate.mdl`, a
  122-joint rig; ending: `db_luigi.mdl`, other bind pose). Those are separate models built
  for their own animations, not variants of `luige.mdl`, so the early mesh cannot drive them.

Verified (native guest frames from `LM_FRAME_DUMP`, Nursery with
`LM_TEST_STATE=poltergust LM_START_MAP=2 LM_START_APPEAR=1`, same input script with the option
on and off; lab with `LM_START_MAP=4`):
- `build/early_luigi_nursery.png`: front (eyes, blink, scared mouth animation), side and
  three-quarter views while walking with the flashlight; backpack, hose, nozzle, flashlight
  and hands line up as in retail.
- `build/early_luigi_vacuum.png`: vacuuming (mode 1, `LM_NOZZLE_DUMP=1`), side view.
- `build/early_luigi_lab_cutscene.png`: E. Gadd's lab event scenes (`dluige03` and the
  gameplay model).
- `build/early_luigi_viewer_body.png`, `..._heads.png`: model viewer, early
  (`LM_DEV_VIEWER=/model/luige.szp|wait0`) vs retail (`/Game/game_usa.szp#luige|wait0`),
  front and side, with the new test aid `LM_DEV_VIEWER_CAM=<yaw>,<pitch>,<distance>` (fixed
  camera, no spin).
- Game Boy Horror map/screen opens normally. The self-shadow uses whatever model Luigi draws,
  so it follows the early mesh (no artefacts seen; not inspected closely).

Not checked in game: the retail-only expressions that use the appended eye textures
(poisoned, `lookheat`) and the burn/ice reactions.

## 2-player mode ("2-player mode (unfinished)", "Co-op improvements")

`src/recomp/coop.cpp` (the mode as the game left it), `src/recomp/coop_fixes.cpp` (the
improvements and test aids). US code only (`port::usCodeFeature()`), both options off by
default, and both take effect at the next map load.

### How it works in the retail code

Everything hangs off `Koga::EnManager::sAppearPointSlotIndex` (`0x804D80B0`), the index of the
last player slot. Nothing writes it (only reads), so it stays 0. With 1:
- Map setup creates two players: the loop in `fn_80072554` runs `slot + 1` times (at most 4),
  each with its own nozzle/tank/Game Boy Horror (`fn_800E977C`), its own persistent record
  (`fn_800739A4(i)` = `lbl_803D5CD0 + 0x16C*i`: health, max health, Poltergust/GBH bits) and
  its own flashlight (+0x7E0) and Poltergust controller (+0x7E4). Only player 0 gets a pad
  (`sGamePad`, flag bit 1); later players get none.
- The camera setup `fn_800204B4` gives view 1 (`lbl_803A4C80[1]`) the same third-person
  camera as view 0 (`fn_80021844`), following player record 2 (`lbl_803A3CA0 + 0x28`).
- The per-frame view update `fn_800112F8` fills both player records (position, room) and calls
  `fn_80011150`: if both players are in the same room and closer than sqrt(600000) ~ 775 units
  there is one 640x240 view letterboxed at y 100..340; otherwise view 0 is the top half and
  view 1 the bottom half (two views drawn by the frame loop `fn_8000BCE8`).
- `fn_8003E7B0` (HUD after each view) skips the whole HUD when the slot index is 1.
- The flashlight (`fn_800850B4` -> `fn_800859EC`) and the vacuum (`fn_8008487C`) test ghosts in
  rooms visible in either view (`fn_8001CE1C(room, 0) || (room, 1)`), so their code was written
  with two players in mind.

The known Action Replay code (US) that turns it on:

| AR line | Effect | Port |
|---------|--------|------|
| `044D80B0 00000001` | slot index = 1 | written at every map setup (`fn_8000C188` hook, before the original), so it only changes while no player exists |
| `04005894 483829CC` + cave at `0x80388260` | replaces the `blr` of the boot pad setup `fn_80005830` with: `new JUTGamePad(port 2)` (0x98 bytes), stored at r13+0x11C = `sGamePad[1]` (`0x804D807C`) | host code at the first map setup with the mode on: the game's own `JUTGamePad` constructor for port 2 run on 0x98 bytes of HLE memory (no game heap is touched, so the retail heap layout is unchanged); the constructor links it into `JUTGamePad::mPadList`, so the game's pad read updates it |
| `040726D8 800D011C` | player creation: players after the first get `sGamePad[1]` instead of no pad | `patch 40` |
| `04079EB4 60000000`, `04079E6C 48000010` | door open `fn_80079E20`: skip the room-blocked check, every locked door counts as unlockable | `patch 41`, `patch 42` |
| `040B943C 3003FFFE` | `fn_800B9430` (Luigi +0x792 event-bit test, e.g. the `<WEAPON>` bits) always true, so player 2 has the Poltergust and the GBH | `patch 43` |
| `0400BBC4 60000000`, `0400BC24 60000000` | `fn_8000BB98` (draw one view): view 1 draws the whole scene like view 0 (normal actors, ghost layer, effects) instead of only map + direct actors | `patch 44`, `patch 45` |
| `0403E7D8 60000000` | `fn_8003E7B0`: the HUD is not skipped in 2-player mode | `patch 46` |
| `04079574 60000000` | door touch `fn_80079440`: a door also opens when `fn_80079780` reports no free side | `patch 47` |

All eight are `tools/recomp/hooks.txt` instruction patches switched with the mode
(`LM_COOP_PATCHES=<hex mask>` keeps only some of them, bit 0 = patch 40, for testing).
Patch 43 is the code's own blanket cheat: it also gives player 1 the Poltergust and the Game
Boy Horror at all times.

### What the unfinished mode looks like (option 1)

Verified in the Nursery and the Mirror Room (frame dumps, `LM_COOP=1`, scripted input on
both ports):
- A second Luigi stands next to the first and is driven by controller port 2 (F1 > Controls,
  assign a device to port 2). Both have their own flashlight beam, Poltergust and nozzle.
- Close together in one room: one shared view, 640x240 letterboxed in the middle of the
  screen, HUD below it.
- Apart (more than ~775 units, or different rooms): top/bottom split screen, top = player 1,
  bottom = player 2. Each view is 640x240 with the normal camera, so it shows more to the
  sides than the room geometry covers: at the room's edges the cut wall ends and the black void
  beyond them are visible (no camera clamp for that aspect). In split screen the HUD
  disappears (drawn after view 0, then covered by view 1).
- Player 2's flashlight lights ghosts (message 2 to the ghost AI, attributed per flashlight)
  and the vacuum's capture test reaches them (messages 1/26); the ghosts mostly attack player 1.
- Player 2 is not taken along when player 1 goes through a door; player 1's health is the only
  one on screen, and the game is over when player 1's runs out.

### Co-op improvements (option 2)

- **Player 2 follows player 1 between rooms.** When player 1's room (view record +0xC, top byte)
  changes and player 2 is somewhere else, player 2 is put 120 units behind player 1 (Luigi
  +0xF48 = facing). Also covers warps and teleports. Verified: `LM_TELEPORT` into the Mirror Room
  brings player 2 along (`[coop] player 1 entered room 4: player 2 (room 26) follows`), the
  room's ghosts appear and fight both.
- **HUD.** `fn_8003E7B0` is hooked: in split screen the HUD is drawn once after the last view
  (player 1's health meter moved up by 240 to the bottom of the top half), and player 2 gets
  the game's own health meter drawn a second time with player 2's health (shared view: next to
  player 1's; split: bottom left of player 2's half). The meter's globals (`0x804D8490/92`
  health shown, `0x804D84B0` heartbeat, `0x804D8496/98` digit position, heart sprites
  `0x803CE698 + 0x30/0x48/0x60` x/y) are swapped in for that call and restored.
- **Health / game over.** A player whose health would drop to 0 while the partner has more than
  1 takes half of the partner's health instead (hook on Luigi's health setter `fn_800B9488`);
  the game's own game over only happens once both are out. Verified in the Mirror Room with
  `damage_scale=20` (one hit is lethal): player 1 was revived six times from player 2's health
  (100 -> 50/50 -> ... -> 2/2), both meters following.
- Doors: with the follow rule player 2 never has to open doors on their own; the AR door
  patches (41/42/47) stay on.

Not done / not verified:
- A full capture by player 2 was not reproduced with scripted input (getting a ghost stunned in
  front of a scripted player is timing-dependent). The capture path is per player (the vacuum
  check iterates ghosts in rooms visible in either view and starts the capture through the
  shared capture manager `lbl_804D8BF0 +0x24` with the vacuuming Luigi), and the capture count
  is global (`fn_800E5784`), so captures by either player count.
- Player 2's stick is relative to view 0's camera (the creation params pass camera 0 to every
  player). Both cameras look the same way, so this only matters at room-specific angles.
- Event scenes, boss arenas and the lab with two players are untested; scripted events move only
  player 1.

Interplay with the enhancements: the widescreen hook that anchors view 1 as the GBH screen
(`fn_800115F8`, widescreen.cpp) leaves it full width in 2-player mode; Hor+ widening and the
widened culling apply to both views (each spans the full width); checked at 21:9 with "Keep HUD
at 4:3" off (both meters anchored left, the GBH right, both views Hor+). Interpolation was checked with
`LM_INTERP_TEST` in split screen (both views blend without artefacts). The self-shadow and
the ghost layer are drawn per view by the game's own code (patches 44/45).

Test aids: `LM_COOP=1`, `LM_COOP_FIXES=1` (force the options), `LM_COOP_DUMP=1` (both players'
position, health, flags and equipment every 300 frames), `LM_COOP_MSGLOG=1` (messages to ghost
AI objects, with the player whose flashlight sent message 2), `LM_COOP_GHOST_TEST=<from>,<to>`
(guest frames: keeps the first visible ghost 220 units in front of player 2, or player 1 with
`LM_COOP_GHOST_TARGET=1`; `LM_COOP_GHOST_HP0=1` also zeroes its health), and `P2:` items in
`--input` (docs/CONTROLS.md). Typical run: `LM_COOP=1 LM_COOP_FIXES=1 LM_TEST_STATE=poltergust
LM_START_MAP=2 LM_START_APPEAR=1 LM_TELEPORT=3380,0,-220,200 LM_SKIP_CUTSCENES=1
LM_SKIP_AUTO=30 --input "600:START*10,1000:A*6,1500:A,1800:A,2100:A,2400:A,P2:2600:STICKLEFT*500"`
(Nursery, then the Mirror Room with its ghosts).

## Other leftovers noticed

- GBH screen mode 1 is the E3 clock (exposed as a toggle); there is no separate gauge screen.
- `fn_80011150` / `fn_800204B4`'s second branch: the two-player split screen, restored as the
  2-player mode above.
- Ghost archives (`model/obakeNN.szp`) carry `bti/obamN.bti` (64x64 CMPR ghost portrait) and
  `obamN_kg.bti` (I4 silhouette) plus a `geb/` file. No code references them by name.

## Debug features

Retail LM was built from an older JSystem that has no JUTProcBar / JUTConsole, and no game
code calls `OSReport`. What is left are a few switches in `.sbss` that code reads but nothing
ever writes. They were found by listing every small-data/bss variable that game code
(0x80003000-0x80190000) loads but never stores to or takes the address of (`pal_diff.py`'s
`walk()` over every function), then reading what each one gates. Most hits are guarded static
locals; these two are real debug switches. Both are in **F1 > Enhancements**, off by default
(`src/recomp/dev_tools.cpp`, applied each frame in the `LMChangeFrameBuffer__Fv` hook):

| Switch | Gates | Port |
|--------|-------|------|
| `lbl_804D8E04` (byte) | `fn_80160138` (particle manager per-frame update) prints `"Emits %d Ptcls %d Chldn %d"` and five `"%02d %02d ..."` history rows with `fn_801D71B0` (JUTReport: vsnprintf, then `fn_801D7114` inserts into the JUTDbPrint queue at `lbl_804D9550`) | Debug tab "Particle manager report". Retail never creates the print object (`lbl_804D9550` is never written), so `fn_801D7114` is hooked: each line is collected and drawn by an ImGui overlay at the game's 640x480 position. |
| `lbl_804D86B0` (word) | `fn_8005E9EC` (called from the game-scene draw `fn_8000BCE8`): copies the EFB at full size and at 160x120, then blends the small (blurred) copy back by depth with a TEV setup; its buffers `lbl_804D86B8`/`BC` are allocated by `fn_8005F098` on every scene setup | Unused content tab "Unused depth blur pass". Works as is: the far parts of the picture go soft. |

Also checked and not usable: `print__14JUTDirectPrint` is only the crash screen
(JUTException); `test.blog` and `/scrn/testtitl.blo` are unreferenced layout names;
`ATestData.cpp` (Ajioka) is a water-effect test loader whose strings are OSPanic messages.

### Debug modes known from Action Replay codes

Community AR codes (TCRF / GC-Forever) reach more leftover debug code. The recompiled game has
no instruction memory to patch, so each code is reproduced by a hook with the same effect
(`src/recomp/dev_tools.cpp`, Debug tab "Debug modes", all off by default; the original codes
activated them with button combinations on the pad word `0x804CAFD0`, here they are toggles).

| Feature | AR code (US) | What it changes in the game | Port |
|---------|--------------|-----------------------------|------|
| Debug mode | `403A2B84 001325CB` (writes 0xCB to byte +0x1325 of the actor at `[0x803A2B84]`, Luigi) | Luigi's debug-flag halfword +0x1324 (initialised to 0x14 by `fn_800AD5AC`). Bits: 0x08 debug text, 0x40 scan boxes (both read by Luigi's draw callback `fn_800B172C`), 0x10 (`fn_800AE770`), 0x20 (`fn_800B1BD4`, toggled by `fn_800B2F98`), 0x04 (`fn_800B80C4`); 0xCB clears 0x10/0x04, which changes how Luigi is drawn | Low byte forced to 0xCB in Luigi's update hook (`fn_800B783C`); the game's value is put back when turned off |
| Debug info display | NOP the `beq` at `0x800B17E4` | `fn_800B172C` pass 6: Japanese text at the top left, ホコリ (dust) mg, れいき (aura) cc, みず (water) ml, ハート (hearts), via `fn_8003EF40` | Bit 0x08 presented to the original for the duration of the pass-6 call |
| Game Boy Horror scan boxes | NOP the `beq` at `0x800B17CC` | `fn_800B172C` pass 3: `fn_8007BDA4` draws orange boxes around every scannable object | Bit 0x40 presented during the pass-3 call |
| XYZ vectors | NOP the `ble` at `0x800B1794` | `fn_800B172C` pass 2 draws axes (`fn_80094170`, size 150) on joint `+0x1328` only when it is > 0 | After the original, the skipped draw is done: `PSMTXConcat(actor +8, joint 0)` then `fn_80094170` |
| HUD test | byte `0x804D854A` = 1, plus `0x80043910` = `addi r16, r16, 0x0A90 / 0xFD00 / 0x0FA0` | `fn_800433B4` draws four coloured message panels and a row of 19 sprites; the patched instruction picks the sprite set: HUD icons (shipped value), digits (-0x300), element meter (0xFA0) | Flag written each frame; the sprite pointer of the calls from that loop (`fn_8003BEEC`, return address `0x80043930`) is moved by the same offset. Combo Off / Icons / Numbers / Element meter |
| Font test | PAL only: `0A4B8590 0201 / 024C5B58 0001`, `02309B26 9B28` | Flag `lbl_804D8488` (PAL `0x804C5B58`, mapped with `pal_diff.py`): `fn_8003EBB0` prints 12 lines of kanji, kana and symbols. The second PAL write repoints table entry 11 (US `0x80304C18`, katakana) at "alphabet 123" | Flag written each frame; "Latin last line" repoints entry 11 (restored when off). US prints under the HUD's projection, so nothing shows; PAL added `LMDefaultOrthoView` at the start of the function, and the hook does the same while the test is on |

Verified in the foyer (`LM_START_MAP=13; LM_DEV_WARPS=2:6@3000`): debug text and scan boxes
(`build/ar_info_scan_axes.png`, axes are the short red/green lines at Luigi's feet), debug mode
(`ar_debugmode.png`), HUD test modes (`ar_hud1..3.png`), font test (`ar_font.png`). Test aids:
`LM_DEV_LUIGI_FLAGS`, `LM_DEV_INFO_DISPLAY`, `LM_DEV_SCAN_BOXES`, `LM_DEV_AXES`,
`LM_DEV_HUD_TEST=0..3`, `LM_DEV_FONT_TEST=0|1|2` (2 = with the Latin line).

Port-side debug aids in the Debug tab (not game code): an **info overlay** (F3: map, room,
Luigi's position, taken from Luigi's update `fn_800B783C`), the **Visit area** warp list and
the **model viewer** (F4). F3/F4 are keyboard-only and not mapped to the pad, so they cannot
interfere with play; the viewer neutralises the pad while it is open, like F1.

Test aids: `LM_DEV_PARTICLE_REPORT=0|1`, `LM_DEV_DEPTH_BLUR=0|1`, `LM_DEV_INFO=1`,
`LM_MENU_TAB=<Enhancements sub-tab>` (starts with the menu open on that tab).

## Unused areas

Map table: `lbl_802FF218` (14 pointers, indexed by `sCurMapNo` `0x804D80C8`) to per-map
descriptors (room count, room files `/Iwamoto/mapN/...`, room bounds, adjacency). Maps are
changed only by the event command `<WARP>(n)` (in `fn_8006385C`: next map `lbl_804D80A4` = n
and `sCurSceneId` = 0, so the scene manager `fn_8000B4A0` tears the game scene down,
`fn_8000BF38` sets `sCurMapNo` = next map and `fn_8000C188` sets the new map up), by new game
(map 2) and by game over (`fn_8000F140`: map 1 or 12). `<SETLUIGI>(p)` picks the Luigi appear
point (`sGlobalLuigiAppearPoint`, consumed by `fn_800E52BC`). WARP targets in the event
scripts: 1, 2, 3, 4, 6, 9, 10, 11, 12, 13. Never loaded: **0, 5, 7, 8**.

| Map | Rooms (`/Iwamoto`) | Content | State |
|-----|-------------------|---------|-------|
| 0 | `map0/test_00..03.bin` | Test map. JMP: Luigi, `dancer`, `situji` (butler), `candle`, two `dltelesa` Boos, `marioe`, `mphand`; observers `kinopio` (Toad) and `rion1`; events 74 and 95; rat, skull, "waiter" and `test1-4` paths | Loads; a mansion-like room with test actors |
| 5 | `map5/h_03_00..03.bin` | "Statue hall": winged-unicorn statues, pillars, two doors. Only Luigi in its tables; `telesa`, `dance`, `mg_test` paths | Loads, playable |
| 7 | `map7/h_05_00..03.bin` | Map 5's statue hall with both corridors (rooms 1 and 2) 2000 units longer and the end hall (room 3) moved back to match; own `col.mp`. Only Luigi | Loads with the rebuilt archive (below), walkable end to end |
| 8 | `map8/h_06_00..03.bin` | The same four-room plan in a wooden, lantern-lit style (two stone chests in the entrance hall, side alcoves, corridors 5500 units long, end hall). E. Gadd (`lohakase`) stands in the entrance hall with event 14 attached. JMP tables in an older layout | Loads with the rebuilt archive (below), walkable |

Used maps for reference: 1 E. Gadd's lab, 2 mansion, 3 training room, 4 Portrificationizer,
6 portrait gallery, 9 roof (King Boo / Bowser), 10 Chauncey's room, 11 balcony (Boolossus),
12 the Portrificationizer room with its own setup (after Bowser / game over; same `h_02.bin`
as map 4), 13 graveyard (Bogmire).

**Port:** the Unused content tab lists maps 0/5/7/8; the Debug tab "Visit area" lists all 14
plus the ten mansion appear points (map2 `jmp/characterinfo` `luige` entries, named as the game
shows them on arrival: 0 Foyer, 6 Foyer by the front door, 2 Graveyard, 9 Secret Altar,
1 Nursery, 7 Sealed Room, 4 Clockwork Room, 5 Balcony, 8 3F west hallway, 3 Roof). A warp is
queued and applied in the `fn_8000B4A0` hook exactly like `<WARP>`, and only while the game
scene is settled: `sCurSceneId` is the game scene, no scene change is pending
(`lbl_804D8088 == 0`, also set while an event hides the HUD), no event script runs (event
interpreter `0x803D2D40` +0x0C), at least 60 frames since the map setup and Luigi updated for
30 frames (or 300 frames on a map without him). Tearing a map down while it is still being set
up crashed (a warp queued during the lab's intro fired right after the event's own WARP to the
training room: call through a garbage pointer in `fn_800BA9E0`).

**Entry cutscenes are held back** (Debug tab "Skip entry cutscenes", on by default, setting
`debug.warp_skip_events`). Map events come from `jmp/eventinfo`: `fn_8002DA34` turns each row
into a 0x58-byte trigger (array `lbl_804D8370`, count `lbl_804D8374`; +0x10 condition callback
chosen by `EventIf`, +0x38 event number), and `fn_8002B7BC` loads `eventNN.txt` when
`fn_8002B6D4` (trigger flags, story-flag gates, then the condition) returns 1. EventIf 0 is
"always" (`fn_8002C9E0`), 2..6 are "Luigi within EventArea in the trigger's room", 1
(`fn_8002C364`) is the same plus A pressed facing it (talking). The `fn_8002B6D4` hook (US and
European game): after a warp's map setup, every trigger except A-to-talk ones whose condition
holds within 150 frames is recorded and answers 0 for the rest of that visit (until the next
map setup). Events reached later by walking somewhere, talking or story flags play normally;
normal play (no warp) is unchanged. Held back on arrival: map 1 event 8 (E. Gadd's lab talk),
3 event 9 (training explanation), 4 event 2 (E. Gadd at the Portrificationizer), 9 event 75
(Bowser intro), 10 event 64 (Chauncey), 11 event 72 (Boolossus), 12 event 52, 13 event 66
(Bogmire), mansion appear point 9 events 6 and 74 (King Boo at the Secret Altar), point 7
event 83 (Sealed Room), point 5 event 71 (Balcony Boos). Bosses stand idle (Bowser on the
roof, the Boolossus Boos on the balcony); Luigi has control and the HUD is on.

### Warp list status (US, verified 2026-10-09)

Every entry was warped to in sequence from the foyer of a new game and from each other (maps
0-13, then 2, 9, 1, 4, 2 again; the ten appear points; the six leftover rooms, then the lab and
room01A again), with the stick pushed up/left/down after each arrival (`LM_DEV_STATUS_LOG`
positions and frame dumps). No crash, no event on arrival, no letterbox or hidden HUD left
over, Luigi moved and the camera followed everywhere. Contact sheets (not committed):
`build/warps/warp_contact_sheet.png` (one frame per entry), `maps_arrive_walk.png`,
`mansion_appear_points.png`, `leftover_rooms.png`, `breaker_old_vs_retail.png`,
`leftover_retail_reference.png`.

| Entry | Luigi placed at | Result |
|-------|-----------------|--------|
| Map 0 test map | (-86, 0, 496), the map's spawn | Works. The test `candle` fire actor drifts around the spawn and burns Luigi if he stays in it; Toad, Boos and the other test actors are there |
| Map 1 E. Gadd's lab | (-3229, 50, -3) | Works, event 8 held back. No HUD (the lab never shows one); E. Gadd idle. The lab is normally left through its event menu, so the way out is another warp |
| Map 2 mansion (appear point 0) | Foyer (-8, 0, 145) | Works |
| Map 3 training room | (0, 0, -18) | Works, event 9 held back; no training session starts |
| Map 4 Portrificationizer | (-960, 0, -70) | Works, event 2 held back. Luigi's `luige` entry is flagged invisible in the JMP table, but he is visible and controllable |
| Map 5 statue hall (unused) | (2547, 0, -138) | Works |
| Map 6 portrait gallery | (1952, 0, -57) | Works; E. Gadd's talk (A) and the portrait plaques are A-to-talk triggers and still work |
| Map 7 long statue hall (unused) | (2547, 0, -138) | Works (rebuilt archive) |
| Map 8 lantern hallway (unused) | (2547, 0, -138) | Works (rebuilt archive) |
| Map 9 roof | (-247, 1780, -961) | Works, Bowser intro held back; Bowser stands idle |
| Map 10 Chauncey's room | (0, 0, 250) | Works, intro held back; no fight |
| Map 11 balcony | (0, 0, 0) | Works, intro held back; the Boolossus Boos float around and can hurt Luigi |
| Map 12 Portrificationizer (after Bowser) | (0, 0, 0) | Works, event 52 held back |
| Map 13 graveyard | (0, 0, 1000) | Works, Bogmire intro held back. The arena's own camera sits outside the fence, so Luigi is seen through the railings |
| Appear 6 foyer door, 2 Graveyard, 1 Nursery, 4 Clockwork Room, 8 3F hallway | the appear point | Work |
| Appear 9 Secret Altar | (2311, -550, -6724) | Works, King Boo's events 6/74 held back |
| Appear 7 Sealed Room, 5 Balcony | the appear point | Work, events 83 / 71 held back (the Balcony Boos keep circling) |
| Appear 3 Roof | (-11, 1800, -2087) | Works on arrival; walking north starts the roof's area event (not an entry event), which ends with Luigi in the Clockwork Room |
| Leftover: lab E3 2001, Portrificationizer pre-E3 | the map's spawn | Work (old models shown) |
| Leftover: breaker room, 1F / 2F hallway duplicates | see below | Work; Luigi starts in the room |
| Leftover: Hidden Room alternate (room01A) | (-1800, 0, 100) | Works (see below) |

Test aids: `LM_START_MAP=<n>` (new game starts in map n; moved here from the Hidden Mansion
hooks), `LM_START_APPEAR=<p>`, `LM_DEV_WARPS=<map>[:<appear>]@<frame>,...`,
`LM_DEV_STATUS_LOG=<n>` (every n frames: scene, map, room id, Luigi's position, event cursor,
HUD-off flag, Luigi's flags; bit 0x10 = Luigi stopped).

### Maps 7 and 8: why they crashed and how they load now

Both crashed for the same reason, and it is in the data, not the port: the game would fail
the same way on a console.

- **Missing model.** Every map setup loads `k03-elem1_eyes.bmd` from the map archive (the
  lookup at `0x8017310C`, type `K03-`) and builds a J3D model from it without checking the
  result. Every retail map archive has `j3d_effect/k03-elem1_eyes/`; `map7.szp` and
  `map8.szp` have no `j3d_effect` directory at all. The model is built from a null resource.
  On the console that read of address 0 (not mapped by the OS's BATs) is a DSI exception.
  In the port low addresses alias main RAM, so the loader wrote through garbage pointers
  instead (J3D packets, the audio message queue, JAudio's track tree). The audio thread then
  recursed through the corrupted track tree (`SeqUpdate` -> `Jam_UpdateTrack` ->
  `SeqUpdate`, and `Jam_SeqmainNote`) until the host thread's stack ran out. That is the
  "unbounded recursion" seen before: a cycle in corrupted data, not deep-but-finite guest
  recursion, so a bigger host stack would not have helped. Found with `LM_TRACE=ring`, a
  resource-lookup log and `LM_WATCH` on a track's child pointers.
- **Map 8's tables.** Its JMP tables predate the retail field layout: characterinfo has 16
  fields instead of 23 (no `create_name`, `room_no`, `item_table`, `appear_point`, `msg_no`,
  `cond_type`, `move_type`, `appear_type`; `dir_x/y/z` are 16-bit ints), groupinfo 20
  instead of 9 (with link and transform fields since dropped), eventinfo 21 instead of 24,
  polygoninfo 8-byte entries with 7 fields instead of 4-byte entries with 3. furnitureinfo,
  observerinfo, soundgroupinfo, soundpolygoninfo and roominfo are missing.

**Fix** (`src/recomp/unused_maps.cpp`, `src/recomp/rarc.cpp`): when the game opens
`/Map/map7.szp` or `/Map/map8.szp`, the port serves a copy rebuilt from the user's disc
(`hle_dvd.cpp` overlay chain, virtual entries `0x4C000000+`). The archive is unpacked into a
directory tree and `j3d_effect/k03-elem1_eyes/k03-elem1_eyes.bmd` is added from
`/Map/map5.szp` (map 7's twin). For map 8, every table map 5 has is rebuilt in map 5's
layout. Fields are matched by name hash (that is how `ToolData` finds them), ints and floats
are converted where the layouts disagree, and fields with no source get the retail
"nothing" values (`create_name` `----`, other strings `(null)`, numbers 0). The sound
tables get one zeroed entry per collision polygon or group, and the other missing tables
stay empty. The result is repacked as a RARC and wrapped in a literal-only Yay0 stream.
Retail never loads these two files, so nothing else changes. With tracing on, the runtime
now reports host-stack exhaustion with the call ring instead of dying silently.

**What is there** (verified with `LM_START_MAP=7/8` and `LM_DEV_WARPS=7@3000,8@4500` from
the mansion, with screenshots, F3 overlay for positions):

- Map 7: Luigi starts in the statue hall. Both corridors lead about 4500 units north (map 5's
  stop at about 2500) and end at a door.
- Map 8: Luigi starts at (2547, 0, -138) facing a lantern-lit wooden hallway. E. Gadd
  (`demo_hakase`) stands at (2549, 0, -932) between the corridors. Luigi bumps into him, and
  the walls and the chests block him. The left corridor runs to a door at about z -6500.
  The wall between the entrance hall and the corridors (z -1050) is fan-triangulated from
  one corner in `col.mp`, so thin collision slivers cover the right part of each corridor
  entrance. Enter the left corridor at x of about 2200, and the right one near its left
  edge. Maps 5 and 7 have a milder version of the same thing.
- Event 14 on map 8 is a talk-style trigger (`EventIf` 1, `EventLock` 1, area 120) at
  E. Gadd's position. Its script is the retail `event14.txt`: Lydia's portrait line "Oh
  dear... Such a draft..." (map 2 uses it for her room), then `<FLAGOFF>(169)`. With the
  shipped area it cannot start, because E. Gadd's body keeps Luigi about 165 units away. In
  a test with the area raised to 300 (not shipped), pressing A next to him played the line
  over this hallway. So in this early build the event was a test hookup on E. Gadd, not a
  scene of his own.

## Unused rooms

Leftover room models on the disc: `/Iwamoto/map1/h_01.bin` (E3 2001 lab, extra robotic-arm
stand), `/Iwamoto/map1/h_02.bin` (pre-E3 Portrificationizer room; retail is `map4/h_02.bin`),
`/Iwamoto/map2/room_67.arc` member `b1_c_67.bin` (older breaker room), `room_07.arc` /
`room_30.arc` member `room..bin` (duplicate 1F / foyer 2F hallways; room_07's differs from the
retail `room.bin` by 42 bytes), `/Iwamoto/map2/room01A.bin`.

`src/recomp/room_swap.cpp` (Unused content tab, "Unused rooms"): Go warps to the retail room's
map with Luigi standing in the room, and shows the leftover model for that visit only;
collision, doors and objects stay retail. The swap ends when Luigi leaves the room he was
placed in or the map, or another warp is made. Entry events are held back as for any warp.

- **File swap** (lab, Portrificationizer, breaker room, hallways): `hle_dvd.cpp` serves the
  retail file rebuilt with the leftover model (the whole `.bin`, or the RARC member replaced).
  The member is written into the retail member's own space when it fits, so the archive keeps
  its size: map 2 streams room archives through fixed-size buffers, and the breaker room's
  archive with the old model appended (485 KB instead of 371 KB) overran them; the room's node
  tree was overwritten and the room draw `fn_80015844` recursed forever (host stack exhausted).
  All leftovers but room_30's `room..bin` (128 bytes larger, appended: 377 KB) fit in place.
- **Spawn**: the map setup picks Luigi's `luige` entry for the appear point
  (`findLuigiAppearIndex`) and reads `pos_x/y/z` with `ToolData::getValue`; while a visit's
  spawn is armed (from the warp until Luigi is placed) those reads return the room's floor
  point: breaker room (3350, -550, -1810), 1F hallway (-1250, 0, -5000), 2F hallway
  (250, 550, -1000), Hidden Room (-1800, 0, 100). The game derives the starting room from the
  position. The visit used to clear the spawn as soon as the current map was the visit's map,
  which is already true when visiting a mansion room from the mansion, so the override was gone
  before the warp happened (Luigi started in the foyer); arrival is now the warp's map setup.
  The game's current-room id (`0x803A3CA0` +0xC) is not the archive index (breaker room
  archive 67 reads 69, room 7 reads 20, room 30 reads 31), so "left the room" is a change from
  the id Luigi started in.
- **room01A** is not loose junk: map 2's descriptor (`lbl_8030377C` +0xC, table
  `lbl_80301360`) lists alternate room models, room 28 -> `room_28A.arc` (switched by game code,
  `fn_80037550` / `fn_800376A4`) and room 1 -> `room01A.bin`, which nothing selects. The room
  loader (`fn_80011FDC`) uses a room's alternate when its slot +0x45 picks one; the visit calls
  the game's own `fn_800129FC(1, 1)` once Luigi is placed (and `(1, 0)` when he leaves). Room 1
  is the **Hidden Room**: room01A is a bare version of it (plain cracked walls and floor, no
  curtains, rug or portraits). Its furniture models live in `room_01.arc`, which a bare `.bin`
  alternate does not load, so the furniture is not drawn (its collision is still there).

Comparison with the retail rooms (`LM_DEV_ROOM_RETAIL=1`, same input): the old breaker room has
a pipe/door shape on the left wall where retail has stone, and lacks the pipes right of the
breaker (`build/warps/breaker_old_vs_retail.png`); the hallway duplicates are near-identical,
as the file sizes suggest. Test aids: `LM_DEV_ROOM=<index>@<frame>,...`,
`LM_DEV_ROOM_RETAIL=1` (place Luigi without swapping the model).

## Model viewer

Debug tab "Model viewer" (F4), `src/recomp/model_viewer.cpp`. Everything is drawn by the game's
own code at the end of every game frame (`LMChangeFrameBuffer__Fv` hook), so it works on any
screen (title, menus, in game) and in every frame-rate mode.

### Catalogue: every model on the disc

On first open the viewer scans the whole disc (a few files per frame, progress bar):
every `.szp`/`.arc`/`.bin`/`.mdl`/`.bmd`/`.bdl` file is read with Aurora's disc reader,
Yay0/Yaz0 are decompressed and RARC archives are expanded at any depth (members that are
archives themselves, e.g. `game_usa.szp`'s `model/*.arc` and `Nakamura/gallery.bin`, are
opened too). Each file is classified by its contents, not its name:

| Kind | Detection | Where | Count (US) |
|------|-----------|-------|-----------|
| `mdl` S3DModel | magic `04B40000` | `/model` characters, ghosts, items; `/Ajioka/ADemo` cutscenes; `/Kawano` menus; `/Nakamura` gallery portraits; `/Effect/door.mdl` | 718 |
| `bin` room renderer | `.bin`, version byte 2, node table | rooms and furniture (`/Iwamoto/mapN/*`), doors (`iwamoto/door`) | 630 |
| `bmd` J3D | `J3D2bmd3`/`J3D2bdl4` | effects and items (`/Map/mapN.szp` `j3d_effect`), `kt_static`, sky boxes | 110 |

1458 models in 579 of the disc's 694 candidate files. Archives of ghost actors are marked
"ghost" in the list; that set comes from the game's tables (actor types `lbl_8033B590`, 0x1C
each: model id, flags; flag 8 = drawn in the ghost layer by `fn_80060E70`; model archives
`lbl_8034E46C`, 0x34 each), minus archives that non-ghost types also use (Luigi's).

### How each kind is set up and drawn

Files are copied into a private `JKRExpHeap` (`__ct__10JKRExpHeapFPvUlP7JKRHeapb`) at guest
0x82000000-0x83FF0000, host-backed memory above the 24 MiB the game uses (0x81F00000-0x81FF0000
below it holds early Luigi's model copies, `early_luigi.cpp`). The host window for
guest addresses is 64 MiB for this (`ctx.h` `kHostMemSize`, module ABI 3): a whole mansion
with its furniture needs about 22 MiB. Game code that allocates (`operator new`) runs with
the viewer heap as `JKRHeap::sCurrentHeap`.

- **mdl**: `fn_80061B40` relocates the model, `fn_80061A24` the `.key`, `__ct__8S3DModelFll`
  builds the S3DModel, `fn_8005AA3C` sets the animation; drawn like the US file-select Luigi
  (`fn_800494E4`): lighting mode 0 of `fn_80057F90`, `fn_8005AA34` frame, `fn_800589B0`
  joints, `fn_80059710` view, `fn_80059C54` opaque / `fn_80059F6C` translucent.
  "(no animation)" is the **bind pose**: joint = inverse of the model's inverse-bind matrix
  (`PSMTXInverse`), envelopes from those (`fn_80058850`, the second half of calcJoints), and
  a placeholder key because `fn_80059C54` draws nothing without one. Parts bound at a
  vanishing scale (the portrait ghosts' `oba??g?` effect meshes are bound at 1/100000 and only
  grown by their animation) get unit scale.
- **bin** (rooms, furniture, doors): a 0x7C graph object (`fn_8001CF64`), `fn_8001D3F4`
  (relocation + node matrices), drawn by `fn_80015CF4` passes 0/1 (opaque) and 2
  (translucent) with the camera in `lbl_803A4168`, as rooms (`fn_8001798C`) and furniture
  (`fn_8001F098`/`fn_8001F138`) are drawn, but without the portal list `lbl_803A4470` and its
  scissor rectangles. Room materials light channel 0 with the light mask `lbl_804D0044`
  (GX_LIGHT0) and the ambient register, both set by the room light code in play; the viewer
  loads its own ambient level and a light at the camera (Maps tab sliders).
- **bmd** (J3D): `load__22J3DModelLoaderDataBaseFPCvUl` (flags 0x54020000, as the map effects
  in `fn_801730C4`), `__ct__8J3DModelFP12J3DModelDataUlUl`, two `J3DDrawBuffer`s
  (`fn_801BFF44`); per frame calc `fn_801C7814` (its joint calc also enters the packets; do
  not call the entry `fn_801C79A0` as well: materials that share a display list then link
  their shape packets into a loop), `viewCalc`, `J3DSys::drawInit`, the game's J3D light
  `fn_800AAF5C`, draw buffers `fn_801C043C` in modes 3/4 and `fn_801C0050`. `j3dSys`+0x50 (the
  draw mode mask `J3DMatPacket::draw` tests) is forced to all bits during the draw: the game
  changes it per pass, which made models blink in and out. Joints whose rest scale in `JNT1`
  is 0 (effects grown only by their `.bck`: `doki_b`, `lg_icestate`, `ftc`) get unit scale.

### No culling

Nothing in these paths culls by distance, frustum or level of detail, and the viewer skips
the room portal list. What made models "partly vanish when the camera is moderately far" was
the viewer itself: the full-screen backdrop that clears the picture was drawn at 99.9% of the
depth range, so every part of a model further away than about 1000 times the near plane was
behind it. The backdrop now sits exactly on the far plane, and the near/far planes are fitted
around the model's bounds (vertices and joints at frame 0, or the room shells for maps) with
wide margins. "Back faces" turns off back-face culling as well (Aurora patch 0012, below), to
see sky domes and rooms from outside.

### Ghost effects

"Ghost effects" (default on for ghost archives) draws the ghost through the game's own ghost
layer instead of as a plain opaque model, exactly as `fn_8000C2D0` does in the game scene:
`fn_8005F518` copies the scene and clears the EFB, the ghost is drawn with ghost lighting
(`fn_800581E8`, ambient from the actor's colour +0xE9) and its alpha (+0xE3) as destination
alpha, `fn_8005F498` copies the layer at half resolution, `fn_8005F5B0` restores the scene and
`fn_8005F280` adds the layer weighted by its alpha (blend SRCALPHA, ONE): soft, glowing,
see-through. The layer buffers (`lbl_804D86B8`/`BC`) are swapped for viewer-heap ones during
the draw (the game only has them in the game scene). Defaults are the values measured in play
(colour 404040FF, alpha FF for a fully visible ghost); both are adjustable.
The S3D lights (`fn_80057CFC`/`fn_80057E40`, including the ghost glow lights) are placed
relative to the game camera's view matrix (`lbl_803A3820`+0x18C); the viewer swaps its own
camera in while it draws, otherwise ghosts were lit from wherever the game's camera was.
Because the layer is added to what is behind the ghost, the look depends on the background:
on the default grey backdrop the ghost turns lighter and see-through (the mouth shows the
backdrop through it), on a dark backdrop ("Backdrop" colour, test aid
`LM_DEV_VIEWER_BG=RRGGBB`) it glows as in the dark mansion. Compared with an in-game frame of
Boolossus on the balcony (`LM_START_MAP=11`), the in-game ghost is flatter and more uniformly
bright: its glow lights sit relative to a camera much further away, and there is a lit scene
behind it.

### Wireframe (Aurora patch 0012)

`patches/aurora/0012-wireframe-override.patch` adds two debug draw commands:
`GXSetWireframeAurora(enable, rgba)` (`GX_AURORA_SET_WIREFRAME`): every triangle-type draw,
including the display-list optimiser's pre-indexed draws, gets an index buffer of its
triangle edges and a line-list pipeline (WebGPU has no polygon fill mode) whose fragment
shader outputs the line colour; no culling and no depth bias (not allowed on lines).
`GXSetCullOverrideAurora(twoSided)` (`GX_AURORA_SET_CULL_OVERRIDE`) forces GX_CULL_NONE.
The viewer's "Wireframe" combo: Off / Over model (the filled pass is pushed back with
`GX2SetPolygonOffset`, then the edges) / Only.

### Maps

The Maps tab lists the 14 maps and their rooms from the game's map descriptors
(`lbl_802FF218[map]`: u8 room count, +4 table of room file paths, indexed like
`room_no`). A room is its file's `room.bin` (or the `.bin` itself) plus its furniture from
`/Map/mapN.szp` `jmp/furnitureinfo` (`dmd_name` + `.bin` from the room's archive, placed with
`pos`, `dir` in degrees and scale as T * Rz * Ry * Rx * S through the node matrix functions
`fn_8001D96C`/`fn_8001CFB4` under a root matrix). "(whole map)" loads every room: the whole
mansion is 706 models (one furniture model, named in the table, is missing from its room
archive). Fly camera: drag to look, WASD to move, Q/E down/up, Shift faster, wheel changes
the speed.

### Verification: every model loads and draws

`LM_DEV_VIEWER_SWEEP=<path filter>[@<frames>]` loads every catalogued model in turn and logs
one line per model with how much of the picture differs from the backdrop; an empty picture
is retried a few frames later (Aurora compiles new pipelines in the background and skips
their draws until ready), then in the bind pose (some animations start with parts scaled to
nothing), then with back faces. `LM_DEV_VIEWER_SHOTS=<dir>` writes a 160x120 thumbnail per
model (`efbcopy::requestNamedDump`), `LM_DEV_VIEWER_SWEEP_START=<n>` resumes,
`LM_DEV_VIEWER_SWEEP_EXIT=1` quits at the end.

Last full run (US): 1432 drawn, 26 with an empty picture, 0 failures; all 1458 load without
errors. What does not draw is empty by design:

- placeholders with no visible geometry: `dummy.mdl`/`dummy2.mdl` (all vertices at the origin;
  `/model` and both `game*.szp`), `Nakamura` `oba23ge0`/`oba23gp0` (portrait 23's effect
  mesh, all 170 vertices within 0.3 units), and six 416/544-byte room-renderer stubs
  (`room_00` `ot.bin`/`ozou.bin`, `room_21` `o_e1.bin`, `room_33` `bednul.bin`,
  `room_41` `room.bin`, `room_47` `onuno.bin`: one node and a single batch that draws nothing visible);
- J3D effects whose colour or alpha only comes from their animation (`ef_ash_bomb`,
  `sentaku`, `oryap`, `snet_b`, `doki_a`, `babyp`, `kek_a`, `warp`): drawn, but invisible
  at rest. With "Wireframe: Only" every one of them shows its geometry (a wireframe sweep of
  all 76 `j3d_effect` models and `warp.bmd` finds nothing empty).

108 animated models were empty in their first animation frames (parts grown by the
animation) and are drawn in their bind pose instead.

Test aids: `LM_DEV_VIEWER=<archive>[#<model name part>][|<key name part>]` (`|-` = bind pose),
e.g. `/model/baby.szp|akubi`, `/Game/game_usa.szp#luige|wait`, `/Map/map2.szp#gold`;
`LM_DEV_VIEWER_MAP=<map>[:<room>]` (e.g. `2` = whole mansion, `2:61` = cold storage);
`LM_DEV_VIEWER_WIRE=1|2` (over model / only); `LM_DEV_VIEWER_GHOSTFX=0`;
`LM_DEV_VIEWER_NOTEX=1`; `LM_DEV_VIEWER_CAM=<yaw>,<pitch>,<distance>` fixes the orbit camera
(no spin), e.g. `0,0.1,0.8` front, `1.5708,0.1,0.8` side. From Git Bash, set
`MSYS2_ENV_CONV_EXCL=LM_DEV_VIEWER` (or `MSYS_NO_PATHCONV=1`, or launch from PowerShell),
otherwise the leading `/` is rewritten into a Windows path.

Known limits: J3D models are static (their `.bck`/`.btk`/`.brk` animations are not played),
so effects whose colour or alpha only comes from an animation stay invisible or faint;
furniture animations (`anm/`) are not played either. Framing uses the file's vertex positions
and the joints at frame 0, so a few cutscene models are framed loosely. Some characters get
their materials from game code at run time (Luigi's textures are bound by the player code),
so they are drawn untextured. Not checked on the European game (the addresses are mapped
with `VA()`, the functions by name).
