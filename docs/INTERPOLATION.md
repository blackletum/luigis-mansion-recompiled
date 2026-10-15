# Frame interpolation

Code: `src/recomp/interp.cpp` (tapes, tag keys, pairing, blending, test mode, judder detector),
`src/recomp/tags.cpp` (the game's draw entry points that open tag scopes), `src/recomp/interp_vtx.cpp`
(draw identities, vertex-array copies, CPU-built vertices), `src/recomp/interp_tex.cpp` (texture and
TLUT copies), `src/recomp/shadows.cpp` (LM: camera source, shadow light), `src/recomp/frame.cpp` (decoupled
guest, target frame rate, pacing), hooks in `src/recomp/hle_gx.cpp`, Aurora patches
`0005-capture-and-replay`, `0021-replay-state-slots`, `0028-replay-copy-generations`, `0030-interp-fades`. Background and the original design: docs/ENHANCEMENTS.md section 3.

## Setting

F1 > Visuals > **Frame rate**: a slider from 30 to 320 fps, plus **Match display refresh
rate** (greys the slider out and uses the refresh rate of the display the window is on, queried
through SDL3 every 0.5 s, so moving the window to another monitor follows it).

- **30** is the game's own rate, drawn directly in lockstep (exactly the old "Original 30").
- **Above 30** the guest is decoupled and every game frame is captured into a tape; the host
  presents at the target and replays the newest tape with its moving values blended toward the
  previous tape's. The game still runs at 30 Hz; the picture is one game frame (33 ms) behind.
- settings.ini: `[video] target_fps=<30..320>`, `match_display_refresh=true|false`. The old
  `frame_rate` key is migrated on load: 0 -> 30, 1 -> 60, 2 (unlimited) -> match display.
- Pacing (`frame::pace`): the host loop sleeps (SDL_DelayPrecise) to the target. With VSync on
  and a target at or above the refresh rate the swap chain paces instead, so the frame rate is
  the refresh rate; the menu says "(VSync limits this to N fps)" and the tooltip explains it.

## Tagged interpolation (interp-tagged, October 2026)

The heuristic pairing of the earlier passes (record two game frames' GX streams, then guess which
draw of frame a is which draw of frame b by mesh, vertex layout, colour, order or nearest position)
produced a new class of bug with every fix: identical instances swapping (doors, vases), a reused
light slot tinting the whole picture yellow, HUD quads mixing up, particles swapping partners. It is
replaced by **source-tagged** interpolation, as in Zelda 64: Recompiled / RT64: the game's own draw
code labels what it draws, and the interpolator pairs strictly by that label. Nothing is guessed.

Kept from before (unrelated to pairing): tapes (capture and replay of a game frame's GX stream with
substituted values), vertex-array / texture copies, the replay state slots and copy generations,
camera factoring and the rigid (polar) blend, shadow-receiver rebuilding, frame pacing and
presentation (pacing.cpp, VRR cap, governor, present timing 0197/0216, keep-warm 0215), the shader
precache, vertex snapshots at draw time, Original 30 (no capture). Removed: nearest/mutual/range-scan
pairing, layout groups, swarm handling, colour pairing, fade-ins/outs (Aurora 0030 is no longer used),
blink detection, second-chance pairing by source address or display-list content.

### Identity

`interp::pushTag(category, object, type, sub)` / `popTag` (interp.h; `TagScope` for C++ scopes) open
a scope around something the game draws. Scopes nest; a scope's identity is

  hash(parent scope, category, object address, type, sub, generation(object), occurrence)

- *object*: the guest address of the thing (actor, model instance, J3D packet, furniture object,
  particle, J2D pane);
- *type*: what it is (model data, a constant per hook);
- *sub*: a part index (node, mesh, joint, pass);
- *generation*: bumped when the object's memory is freed (`free__10JKRExpHeapFPv`: every tagged
  address in the freed block) or a pool slot is handed out again (particle link/unlink, particle
  birth stamps below), so a new object at a reused address is never its predecessor;
- *occurrence*: the n-th scope with that identity under the same parent in this game frame (an
  object drawn twice in one view).

Every value the GX HLE notes (position/normal/texture matrix, projection, GX light, CPU-built vertex
block) gets a **key** = hash(innermost object scope, kind, GX call site (the guest return address of
the GX call), its occurrence for that scope/kind/site); GX lights add the light object's address and
slot. Values noted outside any object scope are **untagged** (key 0), except GX lights (the game's
light object is its own tag) and projections (keyed by the camera's structural scope).

Cameras: the view matrix is sampled at every noted value (`viewMatrixSource`, the game camera's GX
view matrix); a view's key is its structural (View) scope path + pass + the order of distinct
cameras in that path. Views pair by key.

### Pairing and policies (`pairTapes`, `replayPair`)

A value of game frame b pairs with the value of frame a that has the same key and the same shape
(kind, size). Ambiguous keys (twice in one frame) are never paired. Then:

| category | what is blended |
|---|---|
| View (structural) | the camera: rotation slerped, eye moved in a straight line |
| World (map rooms) | only the camera: b's world matrices under the in-between camera |
| Object (characters, furniture, props, J3D) | model and normal matrices, rigidly (rotation slerp, scale/shear, translation) in world space under the blended camera; texture matrices; GX lights (position linear, direction slerped, by light object) |
| Particle | the particle's matrices / CPU-built quad positions, by emitter + particle identity (+ birth stamp) |
| Effect (CPU-built geometry of an object) | vertex positions, only with the same vertex count (topology) |
| Hud (2D) | 2D matrices (a jump > 96 px switches) |
| untagged | nothing: b's values, placed under the in-between camera when in the 3D view (else as captured) |

- Colours (channel ambient/material colours 0x100A-0x100D, light colours, TEV colour and konst
  registers 0xE0-0xE7, fog colour 0xF2, also from display lists) and the direct vertex colours of
  CPU-built blocks are blended between values with the identical tag (noteColors: Kind::Color, one
  tape word each) when the draw they belong to pairs with its partner's draw by its own matrices or
  vertices (a colour set in a helper the item loop calls has no item tag and would pair with another
  item's). 2D/HUD colours blend under the same draw check since f45cd1e (static HUD colours blend
  to themselves; the HUD fading in with a room stepped at 30 Hz). 2D immediate-mode quads with direct
  positions and direct RGB(A)8 vertex colours (HUD fades, full-screen fade quads) are noted as 2D
  blocks (role 2, `posBytes2D`): their colours blend while every vertex position is byte-identical
  in both game frames, or while the picture's layout animates (positions blended too: "Text boxes"
  below; text boxes themselves are shown at the game frame); untagged ones are keyed by structural scope + call site + occurrence (87f2ea6), plus
  the texture they show. XF colours written inside display lists (0x100A-0x100D, light colours) are
  noted like the lists' BP colour writes, and untagged 2D colour registers are keyed by structural
  scope + call site + register + occurrence and blend with a draw of the same kind (8d11727).
  Still open: the scene fade-in on arriving in the foyer / returning from the save screen steps at 30
  Hz (sweep C: ~90% of the ramp at the tape change). It is none of these colour paths, nor a light's
  attenuation (tested: no light changes its attenuation then); candidates left are an EFB copy drawn
  back over the scene or vertex colours of 3D CPU-built blocks. Exceptions, shown as the newest
  game frame: a channel that jumps by more than 96 of 255 in one game frame (blinks, on/off,
  the hit flash), a register of another type (colour vs konst). Texture swaps (another texture
  object) switch. A GX light's colour (XF 0x600 + 16 n + 3, from GXLoadLightObjImm) takes the
  identity of the Light value of the same call (light object + slot, tagged even outside object
  scopes) and blends while that light pairs: Bogmire's arena lightning fades the scene light from
  white to black over ~1 s, loaded untagged per view, and stepped at 30 Hz (sweep C: 75-91% of each
  fade's change at the tape change, 33-34% after, the linear value). (Until b28a8fd colours were never blended: the water's bubbles and the shadow
  ghosts' shimmer stepped at 30 Hz.) Enhanced lighting's extra lights
  (the port's own lights) blend position, radius, colour and intensity by id; flags and shadow
  weight are b's (ghost glows, dramatic flares and flicker change every game frame: at b's values big
  lit areas stepped at 30 Hz).
- A pair that cannot be blended (rotation over ~150 degrees in one game frame, scale 16x, a jump over
  half its distance from the camera) is shown at b's world place under the in-between camera (Luigi's
  instant turns). The test is made in WORLD space (camera motion taken out; in view space a fast
  camera alone made nearby things "jump"). A turn over ~150 degrees is first tried against b turned
  180 degrees about each of its own axes: within 45 degrees of a, that equivalent is blended (matrices
  built from direction vectors, e.g. the Poltergust hose's segments, flip their roll between frames).
- Fast twists: a pair that turns by more than 75 degrees in one game frame mostly about one of its own
  axes (that axis turning by less than 35 degrees, the part scaled alike across it) takes the twist
  from b at once and blends only the rest (`untwist`). The Poltergust hose's envelope matrices are
  rebuilt from the hose direction every frame and their roll changes by up to ~150 degrees between
  game frames: blending spun the ribs around the tube.
- Model instances are handled as one (	agGroup: an S3D model, fn_80059C54 / fn_80059F6C /
  fn_8005A280's instance; a J3D model, its shape packets' matrix buffer at packet +0x14). Skinned
  parts are drawn with weighted sums of the model's joints (sheared, nearly flat where two joints
  differ a lot), so the skin only stays together if every matrix of the instance is blended alike:
  when one matrix jumps (its place moved farther than blending allows) the whole model is shown as b
  for that interval (counted as switches); when one turns by more than 90 degrees, cannot be blended
  by rotation or is squashed flat, every matrix of the model is blended entrywise in world space, so
  each skinned vertex moves in a straight line from its place in a to its place in b. The Poltergust
  hose is part of an S3D model (matrix pool around 0x814FF43C, near Luigi's nozzle): on the vacuum's
  on/off its matrices turn by 150-178 degrees in one game frame. Blended one by one (some switched,
  flip and twist resolution chosen per matrix) neighbours took different paths and the hose's skin
  stretched into long straight sticks (user clip with LM_FRAME_TAG: tape 160 alpha 0.05 and 0.34,
  tape 165 alpha 0.67); switching the whole model detached it from Luigi's hand (2f69188).
  An erratic member has no say in the instance's policy and is blended or switched on its own: one
  that moves by over 150 units, a quarter of its camera distance and three times its instance's
  median step (+60) in a game frame, or did so in the last 16 game frames (`Tape::worldErratic`), or
  sits more than 4x the instance's spread (and 250 units) from the median place of its members. The
  nursery's floating teddy bears (one J3D model, 49 matrices) carry draw matrices that land 800-1000
  units elsewhere every few game frames (unused or zero-weighted), with runs of small steps in
  between, and switched all the bears to 30 Hz (sweep B: 1.1-1.9k switches per 5 s near them, 0
  after). A whole instance moving fast (Chauncey's swoops) moves alike and keeps its say.
  (Correction, sweep B: that 49-matrix J3D instance is Luigi, seen near the bears; it follows him
  everywhere.) An instance of 4 or more voting members is switched for a jump only when at least half
  of them jumped (30f232a): a real respawn or teleport moves most members alike, while one stray
  member's first erratic step after a quiet spell switched all of Luigi. `LM_INTERP_TRACE=1` logs
  the votes ("jump vote ... median", "switched (n of m members jumped)"). Earlier
  attempts tagged fn_80091A70 (a cloth sheet) and J3D models only, which never reached the hose.
- Present hold (pacing::beforeEnd): on an even grid (VRR or VSync off) a frame built faster than
  usual is held until the usual time after its scheduled start (90th percentile of the last 60
  frames, at most 85% of the period), so every frame reaches the screen a constant time after the
  time its picture is for. Bogmire's fight at 4K with Enhanced lighting Ultra built frames in
  3-10 ms: presents came 5-13 ms apart while the motion advanced 8.6 ms each (display error p95
  3-5 ms, 1.2 ms elsewhere): an animated sky with a still camera juddered. `LM_PRESENT_HOLD=0`: off.
- Fast motion is not a jump: a pair that moves at most 2x + 60 units of what the same identity moved
  over the game frame before (`Tape::worldStep`, recorded by the replays) blends however far it goes
  (Chauncey's swoops, ~500 units per game frame, were snapped at 30 Hz).
- Spinners and tumblers (`noteTurn`): every model / normal matrix's world turn since its partner
  (angle and axis) is recorded per identity. One that turned by over 60 degrees in the game frame
  before (a spinner), or tumbles (two turns in a row over 20 degrees about axes more than 45 degrees
  apart; the evidence holds for 8 game frames), blends a turn over ~150 degrees the short way instead
  of switching. The element streams' drops take a random orientation every game frame (35-180
  degrees about any axis): those over 150 were shown at the game frame while the rest blended, and
  the water spray shimmered at 30 Hz (user report). Water spray, Luigi standing: 10-18k such
  switches per 5 s before, 1.4-2.9k after (drops in their first game frames, no history yet).
  Luigi turns about one axis (20-25 degrees per game frame), so his instant turns stay switches.
  A turn over 150 degrees also counts as evidence for the 8 game frames after it (drops whose random
  turns alternate small and large): 0.1-0.45k switches per 5 s left. A spinner / tumbler's blended
  matrix is then translated so the centre of its draw's position bounding box (sampled from the
  indexed positions when the display list is parsed, `Tape::bundleCentre`) moves in a straight line:
  turned 150-180 degrees about an origin outside the mesh, the spray's chunks swung along an arc and
  sat above both game-frame places mid-interval (sweep C, blips.py). The detector's ideal does the same.
- Squashed-flat matrices (columns nearly coplanar: |det| < 0.1 x the product of the column lengths;
  Chauncey's squash-and-stretch bones) have no meaningful rotation or mirroring (their Gram-Schmidt
  frame flips between frames): only the axis lengths (16x) and the place are checked, and they blend
  entrywise in world space (`flatMatrix`). Rolling balls and Chauncey's spin (150-175 degrees per game
  frame) were switches (the turn direction is not knowable from two frames); with the spinner rule
  above they blend the short way once the previous game frame showed the spin.
- Projections: 2D (orthographic) ones are layout and never blended (the HUD anchors each element with
  its own projection; paired by order they slid HUD elements in the in-betweens); perspective ones
  blend only with one of the same aspect, except the shadow caster pass, which blends with its light.
- The shadow caster pass blends everything while its light blends (makeLightPair): its frustum is
  refitted every game frame ("shadows at any distance"); a caster shown as captured sits a frame
  apart from the receivers' lookup through the blended light (the shadows swam).
- Texture scrolls that wrap (offset back from 1 to 0, or faster than half the texture per frame) blend
  the short way modulo 1 (animated skies, water).
- SubView scopes (`fn_80114E18`: event scenes, E. Gadd's call portrait, the door close-ups) have their
  own camera: their values are not factored by the game camera; a model that moves by more than a
  tenth of its distance in one game frame switches.
- Appear / disappear: b's set of draws is drawn for the whole interval (a new thing appears, a gone
  thing disappears, at the game frame); nothing is faded or redrawn from a.
- Camera cuts: b's main camera (the view most main-pass model matrices use) has no counterpart in a,
  or jumps more than 250 units / turns more than 30 degrees AND more than three times its previous
  frame's move/turn (a discontinuity; Bogmire's fast camera does that much every frame and is not a
  cut): b is shown as captured. Also a cut (ab2c506, bf38f99): b's main camera paired with a view of
  a that was not a's main camera and is far from it; a move over 1500 units; the number of views
  changing while the main view moves to another index (the order-based view keys then pair other
  cameras wrongly: chimney drop into the Sealed Room, the new-game foyer transition). Views drawn
  through exactly b's main camera follow its pair.
- The 60 Hz scenes, pauses and lockstep transitions keep the pacing of main.

### Tag sources (src/recomp/tags.cpp; hooks in tools/recomp/hooks.txt)

| category | source | identity |
|---|---|---|
| View | `fn_8000BB98(view)` (per-view scene draw; enhanced_lighting.cpp's hook) | view index |
| View | `fn_8006033C` (events 5/6: a model rendered with its own camera into a 128x128 copy, Mario in the painting) | its argument |
| View | shadow caster pass (`shadows.cpp`: `setPass(1)`) | pass number in every key |
| Object | `fn_80059C54`, `fn_80059F6C` (S3DModel draw), `fn_8005A280` (S3DModel shadow-caster draw) | model instance (r3), its data (+4) |
| Object | `fn_800597C0`, `fn_800598E0`, `fn_800599E8` (S3DModel node draws) | model, node index |
| Object | `fn_80058A60` (mesh matrix load), `fn_800595B8` (mesh draw) | model, mesh entry / index |
| Object | `fn_80057F90`, `fn_800581E8` (actor lighting in `fn_80060E70`'s actor loop) | the actor (the loop's r21) |
| Object | `fn_8001F098`, `fn_8001F138` (furniture draw) | furniture object |
| World / Object | `fn_80015CF4` (bin model draw: rooms, furniture models) | model, node matrices, pass; World unless inside furniture or animated |
| World / Object | `fn_80015844` (bin model node, recursive) | node index |
| Object | `draw__12J3DMatPacketFv`, `draw__14J3DShapePacketFv` (J3D) | the packet (per model instance and shape/material); group: the model's matrix buffer (packet +0x14) |
| Object | `J3DShapeMtx::loadMtxIndx_PNGP/PCPU/NCPU/PNCPU` | joint index (J3D skips loads already in place, so the n-th load is not always the same joint) |
| Effect | `fn_80091A70` (a cloth sheet: 16x16 world-space grid, from `fn_8008E580`; not the hose, which is a J3D model) | object, mode; cell by call-site tag |
| Particle | call-site tag in `fn_8012FD14` (dust groups, `fn_801304B0`) | particle r30, group r29; generation on link/unlink (`fn_80127A74`, `fn_80127AA8`) |
| Particle | call-site tag in `fn_8012B6C4` (second particle draw, `fn_8012CB74`) | particle r29, group r28, birth stamp from the remaining life at +0x24 (counts down) |
| Particle | call-site tag in `fn_801796EC` (45 fixed slots, `fn_80162AE8`) | slot offset r28, array r25 |
| Particle | call-site tag in `fn_8016169C` (JPA-style emitters, `fn_801604FC`) | particle r19, renderer r30, birth stamp from the age at +0x32 (counts up) |
| Particle | call-site tags in `fn_801620AC` (particle r19, renderer r26, age +0x32) and `fn_801611CC` (child item r30, owner r29): the other JPA-style renderers (Bowser's flames, boss arenas) | as listed |
| Object | `fn_8005E110` (one CPU-built quad per ghost, from `fn_80061690`'s loop over the ghost list 0x803D48A0; large in Bogmire's arena) | the ghost (its position pointer r5 - 8) |
| Particle | `fn_800F3120` called from `fn_8011FB40`'s item loops (0x801201F4: items of 0x34 bytes at [r31+0x4C]+r27; 0x801202F8: 0x68 bytes at [r31+0x48]+r26): the element streams' drops, one shared S3D model drawn per item; `fn_80059F6C` inside groups by item | the item |
| World | `fn_80015BDC(room lighting, ambient colour)` from the room loop `fn_80011824`: the room's lights and ambient colour (0x100A); World-scope colours blend without the draw check | r3 |
| Particle | `fn_80120C30` (one GX point per call: the water spray's and other element streams' points, from `fn_8011FB40` / `fn_80120514`) | the point (24-byte entry: position, velocity); a new generation when it is not at its last position + velocity or was not drawn the game frame before (refilled from the nozzle: idle ice mist identity flags 538 -> 27 per run; `LM_POINT_PEEK=<n>`) |
| Effect | `fn_80118E44` (CPU-built strips of 0xA8-byte items, from `fn_80118D3C`) | the item |
| Hud | `draw__7J2DPaneFiiPC14J2DGrafContextb` | the pane |
| (lights) | GX light objects (`GXLoadLightObjImm`) | light object address + slot, in the current scope |
| (extra lights) | enhanced lighting's ExtraLight ids (ghost actor, world-light emitter, fixed sources) | id + the object's generation |
| (generation) | `free__10JKRExpHeapFPv` | freed block |

A **call-site tag** (`interp::addSiteTag`) covers a draw loop inside one game function: while a GX
call comes from inside that function's code range, the item in the named register is a scope of its
own (a new item when the register changes). Birth stamps (`SiteTag::ageOffset/ageStep`): a u16 in the
particle that counts its age (+1 per game frame) or its remaining life (-1) makes its birth frame part
of the identity (found with `LM_TAG_PEEK=<words>`: 98-100% of particles step by exactly one).

### Tools

- `LM_TWIST_LOG=<file>`: model matrices turning by over 30 degrees in one game frame and how far each
  local axis turned. `LM_MTX_DUMP=<lo>,<hi>`: world matrices a, b and the blend for sources in that
  range. `LM_ARRAY_LOG=<file>`: every tagged position array per tape, its partner and how far it
  moved.
- `LM_TAG_LOG=<file>,<first>[,<last>]`: every value noted in those game frames with its key,
  category, call site, scope depth and view, and for untagged values the guest call chain; per tape
  the paired / unpaired / untagged counts by kind and the views with their keys. Finding the next
  tag source: run, take the most frequent untagged chains, look the function up
  (`tools/recomp/addr2sym.py`, the recompiled code in build/**/recomp_gen), hook it.
- **Judder detector** (`LM_JUDDER_LOG=<file>`, `LM_JUDDER_PX=<px>` default 1.5; live and in
  `LM_INTERP_TEST` runs). Aurora patch 0218 (`AuroraSetDrawProbe`) reports every draw with the
  matrix (and its PNMTX slot), projection and first vertex it is drawn with. Every in-between replay
  is tagged draw by draw; each draw of the main pass is judged against its ideal: for a tagged pair,
  the world transforms of that slot's matrix in a and b blended rigidly and seen through the ideal
  camera (rotation slerped, eye linear); for untagged / unpaired / world draws, b's world position
  under that camera. `J` lines: present, tape, alpha, cause (blend, snapped, camera, ...), error in
  px of a 2160-line picture, category, key, call site; `R` lines per replay; a `[judder]` summary in
  the game log every 5 s. **Identity check** (same log, `P` lines): a tagged model matrix followed
  over three game frames by its identity must not jump there and back in the middle frame (jerk > 30
  units and > 1.5x both steps): a wrong tag.
- `tools/interp/tint.py <run> [steps]`: every in-between whose mean colour (whole picture,
  or one of 16 tiles) lies outside its two game frames' range.
- `tools/interp/tagstats.py <tag log>`: values by kind/category and the most frequent untagged chains.
- `LM_INTERP_NOBLEND=<mask>` gained bit 7: enhanced lighting's extra lights shown as captured.
- `LM_PRESENT_DUMP=1` (with `LM_FRAME_DUMP=<dir>,<first>,<last>`): in a real-time run, every present
  of the tapes in the dump range is written as `f<frame>_p<present>_a<alpha x 1000>.ppm` (640x480
  EFB through the copy-to-RAM path, so the extra lights, shadows and bloom composited into the EFB
  are in it; not the post-process chain). It shows what each present looks like without
  `LM_INTERP_TEST`, which pauses the guest and changes the timing. Works at 60 fps. At 120 fps the
  dump's own cost made most tapes lose their dump marker (one file per run), so use 60 there.
- `LM_XL_LOG=<file>`: per replay (`P seq alpha f<dump frame> a <a seq> lights n`), every extra light
  with its id, identity key, paired or new, intensity in a and b and the intensity shown, and flags
  (shadow weight in bits 8-15).

### Foyer entry flicker report (October 2026, fixed: see "Cause" at the end)

User report (live, 60 fps presented on a 120 Hz G-SYNC TV, 4K, Enhanced lighting Ultra): "all the
lights flicker" in the new-game foyer intro. In the capture the shot after the cut at guest frame
2583 (left wall sconce, about 19.5-20.5 s into the clip) has the sconce's light pulse. Bright
presents show a wide, sharp-edged halo with no shadows. Dim presents show the sconce's own shadows
(bracket, table). They alternate about every 3 game frames, and values are often held for 3
presents. That session ran while other agents' builds kept the CPU at 100% (guest 21-26 fps at
times).

Not reproduced, in any of the following runs. The light stayed steady and its shadow faded in
smoothly in every run:
- Original 30, vanilla and Enhanced lighting;
- 60 and 120 fps with the user's own settings.ini (VSync on), render scale 2, 4 and 8, with
  `LM_JUDDER_LOG`;
- the shipped LuigisMansionRecomp package with a fresh setup;
- CPU starved to 2 or 1 cores (affinity), also with a cold pipeline cache;
- a second new game after a reset in the same session.

Every present was checked with `LM_PRESENT_DUMP`. The data paths were checked as well:
- extra-light intensities per present (`LM_XL_LOG`): smooth, every light paired;
- shadow slots and weights (`LM_LIGHT_SELECT_LOG=2`): the two candle groups gain their slots at the
  cut and fade in 64 -> 255 over 12 game frames, then stay;
- no lamp is lit (room-lit test) during the intro.

Seen in passing: under starvation, presents hold the same tape (alpha 0 or 1 repeated), which
matches the user's 3-present holds. Candidates left: what differs between the user's 4K fullscreen
swap chain and an off-screen window; state from the save played earlier in that session (not
reproduced with a reset and a second new game).
Follow-up (Aurora 0221, docs/RENDERING.md "Skipped draws are logged"): light shadow casters whose
pipeline was still compiling were skipped, which drops that light's shadow for the present. They
now wait like other draws, and every skipped draw is logged (per-second count plus the config of
each pipeline the first time). The user's next session log will show whether skips occurred.

**Cause (found later):** the runs above all used a 4:3 off-screen window. The user's window is 16:9,
and the flicker only shows at 16:9. Why the 4:3 runs stay clean was not checked; the effect may be
culled at 4:3. At 16:9 the effect at fn_801191B4 (Effect scope, GXSetProjection at 0x80119208) sets
the camera's projection again in this shot. Its scope's identity alternates between two keys, so it
pairs with the previous game frame only every third frame. Unpaired, it stayed b's projection while
the camera's other projections were blended: the intro camera zooms (p00 0.920 -> 0.916 -> 0.911 per
game frame). The draws after it, and with them enhanced lighting's shadow mask, had a projection
that disagreed with the rest of the in-between frame. The mask rebuilds view positions from the
depth pre-pass with the last world draw's projection. The left sconce's light sits 20 units in front
of the wall, and on those in-betweens the wall and the table came out fully shadowed, leaving only
the flames' glow. It faded back in with alpha. That happened in 2 of 3 game frames, every few
presents. Reproduced with `--window 1920x1080` and the user's settings.ini at 60 and 120 fps. The
light list was steady (`LM_XL_LOG`, now also with world positions), and the dark presents went away
with `LM_XL_SHADOWS=1,0` and with `LM_INTERP_NOBLEND=0x08` (projections as captured).

Fix (`replayPair`): an unpaired projection whose captured values (6 values + type) equal those of a
projection blended in the same replay gets that projection's blended values (`[interp] unpaired
projections per present given an identical projection's blend`: 0.67 in this shot). Verified with
`LM_PRESENT_DUMP` (frames 2585-2620, region luma per present): 60 fps, 68 presents, no drop (before:
drops of 6-9 every 2-3 presents); 120 fps, 144 presents, no drop. Original 30 is untouched (no
tapes there): vanilla 81 frames byte-identical, and Enhanced-lighting Ultra 80 frames byte-identical
apart from a run-to-run shift of one dump number (`LM_INPUT_CLOCK=frames`, keys
`1200:START*10,1600:A*6`, the shot at frames 3200-3240).
Testing note: test widescreen-sensitive rendering with a 16:9 window (`--window 1920x1080`). The
off-screen default window is 1280x960, so the game runs at 4:3 there.

### Enhanced lighting

- Extra lights (world candles/fires/lamps/sprays/lightning, ghost lights, test lights) pair by the
  providing code's id plus the generation of the object behind it (ghost actor, particle emitter); a
  pair whose world position jumps by more than 200 units is another light (b's place). Only position
  and radius blend: colour, intensity, flags and the shadow weight are always b's, so the yellow-tint
  class (a light's colour carried to another light's place) cannot happen.
- Ghost lights appear and vanish at the game frame (no partner: b's world place under the in-between
  camera; gone: not drawn).
- The light shadow maps, the flashlight's shadow map, AO, bloom/glow and the volumetric beam are
  computed by Aurora from each replayed (blended) scene, so they follow the blended casters, lights
  and camera. GX light directions are slerped (the flashlight's light stays on its cone's axis, which
  is blended by rotation); the realistic flashlight's anchors are world points blended by id.
- Not interpolation: the realistic flashlight's flare (Aurora `enhanced.cpp`, `g_flare`) follows
  wall-clock time between presents; in `LM_INTERP_TEST` runs (replays out of order) it makes
  in-betweens brighter than both game frames (identical numbers on main). Use
  `AURORA_BEAM_FREEZE_TIME=1` for comparable dumps.

### Text boxes: 2D layout blending and the phase lock (October 2026)

User report (live session, 120 Hz VRR TV): "some judder on text boxes as the text draws in"
(Neville's dialogue, the purple message window). The user's capture (120 fps; the session itself
presented 60 fps, `[interp] presented 60.0 fps` in its log) showed the window opening in steps that
lasted 4, 5, 2 and then 4 capture frames each (top edge 256 -> 196 -> 136 -> 100 px at 1080p), the
letters then every 4 frames except one 5 / 2 pair, the camera behind the translucent window smooth.
Two causes, both ours:

1. **The window's layout stepped at 30 Hz.** fn_80044138 draws the window as one 2D quad
   (`fn_8003B054`, texture 480x120, direct s16 positions, direct colours) that grows from a thin
   bar to its full height over 3-4 game frames (E. Gadd's call window: top 443 -> 413 -> 383 -> 353
   in the 640x480 layout). 2D blocks only blended their colours, so its edges jumped 30 units
   (60 px at 1080p) once per game frame while everything else moved every present.
   `blend2DLayout`: a 2D block paired by identity whose positions changed blends them too when it
   is the same picture (identity now includes the texture loaded into map 0; every vertex's
   texture coordinates identical in both frames), at least 48 layout units across in both frames
   (glyphs are smaller: letters still appear and lines still scroll at the game frame, as on the
   console), every vertex moving by at most 96 units, and it either changes size (opening,
   closing, pulsing) or moved the same way in the game frame before (sliding; a single jump such
   as a menu cursor stays a jump). Positions keep their format (s16: whole layout units, 4.5 px
   steps at 2160 lines). Also blended now: the health heart's pulse, the Game Boy Horror sliding
   up/down, the title menu boxes (the message windows and the call screen no longer: "Text boxes at
   the game frame" below). `LM_2D_LOG=<file>` logs every blended
   picture (tape, alpha, key, bounds a / b / shown); `LM_INTERP_2D_LAYOUT=0` turns it off; the
   `[interp] 2D pictures that moved` line counts blended / kept per present.
   Call window opening at 120 (4 presents per game frame), top edge shown per present:
   442 434 427 419 | 412 404 396 389 | 381 374 366 359 (before: 443 x4, 413 x4, 383 x4, 353).
2. **Presents at the tape boundaries** (`pace()`, phase lock). Whatever is shown as the newest
   game frame (2D layout, text, unpaired things, 30 Hz texture animations) changes at the first
   present past a tape's time. The delay put the presents anywhere in the game frame; when they sat
   next to the tape times (the drift between 29.97 Hz and the display, or a change of the measured
   delay, moves them there every few seconds), the ~1-2 ms jitter of the smoothed tape times decided
   whether a change came a present early or late: 1 / 3 presents per game frame at 60 fps instead
   of 2 / 2 (the user's 5 / 2 capture frames), 3 / 5 at 120, sometimes in bursts (1 3 2 1). The
   delay used (`lockD`) now keeps the target halfway between tape times: measured on the target,
   corrected by 10% of the phase error per present, kept between d - 0.25 and d + 1 present periods
   and moved by a whole period only when d leaves that range (so small changes of d no longer
   move the target either). Only when the presents per game frame are an integer (+-0.08).
   `LM_PHASE_LOCK=0`: off; the `[interp]` line shows `phase lock +x ms (n wraps)`.
   Measured with `LM_PACING_LOG` (call scenario, steady 30 Hz stretches, same build, hidden window
   on a busy machine): 60 fps, off: 0.87% of game frames shown for 1 or 3 presents (10 events in
   1151 frames, one a burst), first-present alpha spread over the whole game frame; on: 0.24%
   (4 in 1683, each a single wrap), 91% of first presents at alpha 0.20-0.25; stalls 11 -> 5,
   shown 25.9 -> 29.3 ms behind the newest game frame. 120 fps: alpha at the first present
   clustered at 0.10-0.125 (87%) instead of uniform.
3. Not changed: letters appear at the game frame (inherent; the game reveals one or two per game
   frame). Original 30 is byte-identical (21 frames 2600-2660 across the call window opening).

#### Text boxes at the game frame (follow-up)

User feedback on the above: "the text on textboxes is still jumpy perhaps we just leave all text
boxes at 30fps". The box opened smoothly while its letters still appeared at the game rate, so the
text jumped against the moving box. Now every text box is shown exactly as in its game frame, at
30 Hz, as on the console: the box, its text, the speaker icon, the "more" arrow and choices, and the
portrait that goes with it. Nothing inside a text box blends (position or colour).

- `interp::pushFrameLock` / `FrameLockScope` (interp.h): nothing noted inside, so it replays as
  captured: matrices, lights, colour registers (`note`, `noteColors`, display-list colours), 2D and
  3D vertex blocks with their colours, CPU position arrays (`arrayKey`). A direct-vertex draw is noted
  at the next GX call, which can come after the scope has closed, so the lock is taken at its
  `GXBegin` (`vertsBegin`). Without that, the window's last quad still blended.
- Where text boxes are drawn (found with `LM_INTERP_DRAWS` call chains and `LM_2D_LOG`):
  - `fn_80044138` (tags.cpp; from the frame loop `fn_8000BCE8`) draws every message window through
    `fn_800433B4`: the box picture (`fn_8003B054`), the speaker icon, the text, choices (the lab's
    green "The Mansion / The Training Room / The Gallery" box). All dialogue goes through it:
    Neville, Toad, E. Gadd in the lab and on the call, signs, the file-select messages.
  - Room / area name (`fn_80045BDC`, widescreen.cpp).
  - While a message window is on screen (it drew in this game frame or the one before;
    `tags::textBoxShown`, counted with `interp::drawCount`), the Game Boy Horror (`fn_8003D634`), its
    noise (`fn_8003BE9C`) and the event portrait (`fn_80114E18`): during E. Gadd's call the GBH is his
    call window. It shakes as the call comes in; its bezel used to blend while the picture inside
    stayed put. (The GBH screen mode `lbl_804D846C` stays 4 in the `call` scenario's call, so it
    cannot tell the call apart.)
- Still blended (not text boxes): the health heart's pulse, the HUD sliding out when a dialogue
  starts and back after, the Game Boy Horror sliding up/down outside dialogue, the title menu boxes,
  the area-result iris wipe.
- The phase lock in `pace()` stays: it keeps the game-frame changes evenly spaced. Call scenario,
  game frames 2560-2900 (`LM_PACING_LOG`, presents per game frame): 120 fps 339 x 4 and 2 single 5s
  (the lock moving by one present, `1 wraps`); 60 fps 340 x 2 and 1 single 3. No alternating 5/3 or
  3/1 runs. With the frame cost above the present period (another instance running, the lab's
  ~11 ms at 120) the rate falls below 120 and 3/4 alternate, as before.
- Checked per present with `LM_PRESENT_DUMP`: at 120 in the call (frames 2624-2700) the window's text
  area and the GBH change only at the first present of each game frame; at 60 in the lab dialogue
  (`LM_START_MAP=4`) the window opens in whole game-frame steps over the smoothly moving scene.
  `LM_2D_LOG` (which now also prints the draw's call site and `f<game frame>`) lists no
  message-window picture in either.
- Original 30 byte-identical (41 frames 2620-2660 of the call scenario, window opening included).

### Results (first build, October 2026)

Test mode (`LM_INTERP_TEST=3`, 640x480 dumps; metrics.py and tint.py; "before" = the heuristic design):

| scenario | stale px | snap blobs start / end | back-and-forth | tint (in-betweens outside both neighbours) | detector |
|---|---|---|---|---|---|
| foyer strafing 2600-2700 | 0 | 28,368 / 28,083 (before 26,598 / 22,642) | **18,080** (before 31,913) | 7 of 198 (tiles only: Luigi's instant turn moves the lit area) | 0 blend errors; 36 snapped (instant turns) |
| foyer, enhanced lighting Ultra | 0 | 56,269 / 78,840 | 27,981 | flare frames (see above) | 0 blend errors; 54 snapped |
| vacuum, heavy dust 2655-2662 | 0 | 0 / 0 | 0 | 0 of 12 | 0 |
| nursery 2600-2780 (lightning) | 0 | 765,857 / 84,985 (lightning, lights on) | **54,247** (before 63,749) | lightning frames | |
| Boolossus (map 11) 2600-2700 | 0 | 9,765 / 2,546 | 20,040 | 6 of 198 (tiles) | 0 blend errors; snapped: joints flipping >150 degrees |

Live (judder detector every 3rd in-between, 120 target): vacuum scene 0 of ~1.27 M judged draws per
5 s off by more than 1.5 px; gold ghosts room with fire and enhanced lighting Ultra (9 min): 0 blend
errors. Original 30: 41 foyer frames byte-identical to main. Pacing (hidden window): 60.00 fps at 60,
116.40 at 120 (as main); tape close 0.6 ms (before 1.4-2.6 ms).

User session 1 (4K, 120 Hz, enhanced lighting Ultra): "things were looking exceptionally smooth, I
didn't notice anything looking wrong or juddering". Fixed after the sessions: hook overhead
(`callOriginal` cached: the game logic had dropped to 18-25 Hz in heavy scenes), HUD judder (2D
projections were paired by order and blended across HUD elements; now never blended).

## History: the heuristic design (passes 1-5)

The sections below describe the design that tagged interpolation replaced. Their pairing,
fade, swarm and blink parts no longer apply; the pacing, copies, camera factoring, shadow and
measurement parts still do.

## What was wrong (stability)

Measured with the deterministic test mode below in the nursery (`LM_START_MAP=2
LM_START_APPEAR=1`, Luigi walking with scripted stick input):

1. **Vertex data read after the game had changed it (the popping/vanishing objects).** A tape
   referenced the game's vertex arrays by pointer, and Aurora read them when the tape was
   replayed, while the guest was already building the next frame. Luigi's body (vertex data the
   game rewrites every frame) was drawn from half-rebuilt memory: in the A/B run
   (`LM_INTERP_ARRAYS=0`) his body disappears entirely from replays of some frames (frame 2608:
   only the hose and Poltergust remain, ~3300 pixels wrong), and which frames are hit depends on
   timing, so in real play it flickers. Original 30 never shows it because there the frame is
   drawn before the guest continues.
2. **Matrices paired by source address and occurrence.** The game builds many matrices in
   per-frame pools (e.g. 0x814E1000), so "the n-th load from address X into slot Y" is not
   necessarily the same object from one frame to the next when the set of drawn objects changes;
   such pairs blend one object toward another's position. (The pixel metrics could not separate
   this from the other causes; the trace shows pool matrices whose partner is thousands of units
   away.)
3. **Too-tight jump thresholds and entrywise blending.** Rotation entries changing by more than
   0.35 or translations by more than 400 units were shown unblended. That snapped Luigi's limbs
   and body whenever he turned (about 20-25 degrees per game frame), and far objects whenever the
   camera turned (they move a lot in view space), while everything around them was blended:
   visible as jitter. Texture-projection matrices (the flashlight's projected texture) were
   compared relative to entries that pass through zero, so about 300 per frame snapped.
   Blending matrix entries linearly also shrinks a turning object at the in-between frames.

## What it does now

- **Vertex-array copies** (`interp_vtx.cpp`). While capturing, every draw is parsed: display
  lists at `GXCallDisplayList`, and the raw vertex stream after `GXBegin` (game code writes it
  through the write-gather pipe). For each indexed attribute the highest index gives the range
  of the array the draw reads; that range is copied at the time of the draw (when the console's
  GPU would read it), and the tape's array-base commands are pointed at the copies (with their
  size, so Aurora uploads exactly the copy). Copies are shared per address until
  `GXInvalidateVtxCache`, after which the current arrays are loaded again so later draws get fresh
  copies. Every tape starts by loading the current arrays, so it never depends on another tape.
  Indexed matrix loads (`LOAD_INDX`) extend the matrix arrays the same way. About 1.2 MB of copies
  per game frame in a room.
- **Pairing by draw.** The values noted before a draw (position/normal/texture matrices,
  projection, lights) and the draw's own CPU-built vertices form a *bundle*, identified by what is
  drawn (display-list address and size, or primitive + vertex layout + position array for
  immediate draws) and by the kinds and sizes of its values. Bundles of the same identity are
  paired between frames, closest values first (rotation weighted against translation, a sprite's
  texture spin weighted strongly), then their values one by one. XF slots are deliberately not
  part of the identity: J3D hands out matrix slots per frame. Where many draws share an identity
  (particles), the common motion (the camera's, in view space) is taken out first and a draw may
  move at most 12 units plus 1% of its distance on its own: a particle that died and one born
  elsewhere are never paired, they just disappear/appear. What is left is paired by (kind, source
  address) only where that is unambiguous. Pairing runs once per tape on the guest thread (about
  0.6 ms including the copy bookkeeping). `LM_INTERP_PAIRING=source` uses only address pairing,
  for comparison.
- **Blending.** Position and normal matrices are split into rotation (quaternion, slerp) and an
  upper-triangular scale/shear part (Gram-Schmidt), so turning objects keep their shape and
  sheared matrices from non-uniformly scaled parents still blend. A pair is shown unblended
  (snapped) when the rotation is over ~150 degrees in one game frame (no telling which way it
  turned), a scale changes over 2x, or the translation jumps by more than half its distance from
  the camera (at least 200 units): teleports, respawns, wrong pairs.
- **Camera cuts.** If more than half of a tape's paired position matrices jump, the whole tape is
  shown as is. LM hides room changes and warps behind fades, so no smear was seen there.
- **Spawn/despawn.** A value with no partner is drawn as captured.

## Interpolating everything

- **Lights**: `GXLoadLightObjImm` position and direction (view space) are blended per light
  slot, the direction keeping its length (spot-cone falloff).
- **Texture matrices**: projection-style ones (flashlight projector, shadows) are blended
  row-wise; the rows are compared by their rotation/scale part only. Matrices that alternate with
  zero matrices are never blended toward zero. 2x4 matrices that spin texture coordinates in their
  plane (particle sprites) are blended by angle and scale about their pivot (up to ~150 degrees per
  game frame). Other small texture-coordinate matrices snap on a wrap (an entry jumping by more
  than 0.5).
- **Billboards and particles**: camera-facing sprites (the particle system's one-quad display
  lists, J3D billboards such as the spiderwebs) have no rotation in view space; slerp keeps it that
  way, so they stay camera-facing at every in-between frame while their positions, sizes and
  spins are blended.
- **CPU-built geometry**: immediate-mode draws under a perspective projection with direct float
  XYZ positions are noted as vertex blocks and paired by layout, vertex count and the nearest
  first vertices; positions are blended (a block moving more than 200 units snaps).
- **EFB-copy effects** are replayed with the rest of the tape, so they show the interpolated
  scene.

## Test mode and evidence

`LM_INTERP_TEST=<steps>` with `LM_FRAME_DUMP=<dir>,<first>,<last>`: the guest waits after each
game frame N while the host renders, with guest memory as it is at the end of N:
`fN_fresh.ppm` (tape N as captured = the 30 Hz frame) and `fN-1_s1..s<steps>.ppm` (frames N-2 to
N-1 at alpha s/steps; `s<steps>` is tape N-1 replayed one game frame late, as in real play).
With `LM_INTERP_TRACE=1` it also logs, per frame, what was snapped (with values) and pairing
statistics. Scripts used for the numbers below (pixel metrics on these dumps) are not in the
repo; the checks are:

- *stale*: pixels where `fN_s<steps>` differs from `fN_fresh` (a late replay must look exactly
  like the 30 Hz frame). 100 walking frames: 3308 with `LM_INTERP_ARRAYS=0` (Luigi's body
  missing), **0** with the copies. On main after the shadows merge (703f378) frame 2608 shows
  3161 again, with or without the copies: there Luigi's body is missing from the *fresh* replay
  and present in the late one, and the same body dropout happens in Original 30 (frames
  2608-2609, also with `LM_EFB_RAM=0` and the shadow options off). So it is not interpolation
  but GX state the frame inherits from the previous one (a fresh replay starts from the previous
  frame's end state, like the console; the late replay starts from its own end state); to be
  looked at with the shadow/GX accuracy work.
- *billboard tracking* (`LM_INTERP_TRACE` "match" lines): camera-facing sprites followed over
  three tapes; "jerky" = the middle position is far off the line through its neighbours (a wrong
  pair). 100 walking frames in the nursery, ~33,000 sprites tracked: 7,709 jerky with address
  pairing, 5,274 with the first per-draw pairing (large groups paired in order), **827** now.
- *snapped*: blobs that jump all at once between fN-1_fresh and fN_s1 and then stay. Over 100
  walking frames these are now almost only Luigi's instant 180-degree turns (the game turns him
  within one frame: nothing to interpolate) and the flashlight switching on. Position matrices
  shown unblended per frame went from ~90 (entrywise thresholds) to ~25 (rotation/shear
  blending); texture matrices from ~300 to ~50 (the remaining ones are 2D texture rotations of
  more than 90 degrees per frame). With per-draw bundles and sprite spins (October 2026) the
  nursery walk is at ~6 position and ~6 texture matrices per frame, and the pixel blobs that
  snap at the start of an in-between dropped from 166,000 to 18,000 pixels over 100 frames (what
  is left is Luigi's instant turns).
- Teleport (`LM_SKIP_TEST=2700:pos:...`) and warp (`LM_DEV_WARPS=2:3@2700`): no smearing; the
  room change happens under the fade.

Performance (RTX 3090 Ti, i9-12900KS, nursery, walking): 60.0 fps presented at a 60 target,
game 29.6-30.1 fps; tape ~600 KiB + ~1.2 MiB array copies per game frame; 1.1 ms guest time per
tape, 0.4 ms host time per present for the blended replay.

Test aids added with interp-everything: `LM_INTERP_NOBLEND=<mask>` shows the value kinds of the
mask (bit = `interp::Kind`: 0 position, 1 normal, 2 texture matrices, 3 projections, 4 lights,
5 CPU-built vertices, 6 colors) as captured, to find which kind causes an artifact;
`LM_INTERP_POISON=<item>,<phys>,<size>` (hex) fills that
guest memory with garbage during one `LM_INTERP_TEST` replay (e.g. `f2762_fresh`): if the replay
changes, it still reads that memory live (bisect the range); the trace logs a self-check of the
camera-factored and shadow-receiver values at alpha 1, the values without partner (with their draw
identity) and the camera cuts with eye move/turn.

## Dust and spiderwebs (October 2026)

User report: with interpolation on, the dust Luigi kicks up and the spiderwebs misbehaved.
`LM_INTERP_DRAWS=<first>[,<last>]` (frame numbers as in `LM_FRAME_DUMP`) logs every draw of
those tapes with its guest call chain, the values noted before it, and what each was paired
with.

- **Spiderwebs** (nursery, foyer) are J3D draws (`draw__8J3DShapeCFv`) of camera-facing
  billboards. J3D loads their matrices into whichever XF slots are free that frame (slots 6/9/12/15
  in one frame, 15/18/21/24 in the next), and pairing used the slot as part of the key, so they
  never found a partner and were drawn as captured: they moved in 30 Hz steps while the room
  around them moved smoothly (template tracking in the dumps: web +4 px at once, wall +1 px per
  in-between; now both +1 px per in-between).
- **Dust and other particle effects** come from the game's particle system (`fn_8012FD14`): every
  particle is the same 32-byte quad display list with a billboard position matrix and a spinning
  2x4 texture matrix, all loaded from one scratch address. About 180 such draws per frame were
  one group, too large for the nearest-value pairing then (it fell back to pairing in draw
  order, which shifts whenever a particle dies or spawns), so puffs were blended toward other
  puffs (flying across the cloud, stretching, doubling). Their texture spins (30-90 degrees per
  frame) also snapped. Now particles are paired as whole draws with the camera motion taken out,
  spins are blended by angle, and new/dead particles are left unpaired.
- A field-order bug in the value records also gave CPU-built vertex blocks a stride of 1, so they
  were never blended (they always snapped as "too far"); fixed.
- The scanner now sizes immediate-mode vertices with the layout as it was at `GXBegin` (the game
  changes it right after `GXEnd`); the "unparsed stream" (a full-screen 2D quad) is gone.

## Fixes from the SMS port (October 2026)

- Aurora 0022: `GXEnd`'s "vertex data not evenly divisible" check measures the draw in the
  calling thread's recording, not the FIFO (which the host replaying tapes appends to and
  resets). The draws were fine; the warnings were noise (LM: sporadic, 4-5 in some 60 fps runs,
  none in the A/B runs, so the reduction is not measured here).
- Immediate draws with direct positions: the draw identity no longer includes the bound
  `GX_VA_POS` array (stale for direct positions). In LM's nursery the CPU-built vertex blocks
  already paired (7.2 of 7.9 per tape before and after), so this changes nothing there; it keeps
  LM safe if a direct-position draw follows indexed ones with changing arrays.
- Light jump limit relative to distance (at least 400 units, else half the light's distance),
  and non-finite light values never blended or rescaled.
- Camera cuts to a nearby viewpoint: a tape is also a cut when the median relative move of its
  paired position matrices exceeds 0.08 and four times the previous tape's (`Tape::camStep`;
  `LM_INTERP_TRACE` logs "scene step"). In the opening cutscene it catches two shot changes the
  half-of-matrices test missed (scene step 0.34 / 0.39 after ~0.003); gameplay walking in the
  nursery and foyer stays at 0.04 or less (no false cuts).

Measured with `LM_INTERP_TEST` (nursery walk at 60 and 120 fps, 210 frames each; foyer walk at
60 fps, 160 frames): 0 stale pixels before and after; pairing statistics (position matrices,
billboards, vertex blocks, jerky counts) within run-to-run noise of main.

(Fixed by interp-everything, texture copies and copy generations: 0 stale pixels over frames
1880-2120.) Was open: in the attract-mode opening (crows on the branch, the second-to-last frame of that shot)
the late replay of one tape draws the lit models darker than its fresh replay (88,685 px; the sky
and grass are unchanged). It is the same with `LM_NO_SHADOW=1`, `LM_EFB_RAM=0` and with or
without the replay state slots, so it is neither the shadow pass, copies to RAM nor carried GX
state; display lists and vertex arrays are copied into the tape. Remaining suspects: data Aurora
still reads by pointer at replay time (textures and TLUTs via `GX_AURORA_LOAD_TEXOBJ`), which the
game may already have replaced for the next shot.

## Cost of closing a tape (guest thread)

Pairing runs on the game thread when a tape is closed, so it eats into the 33 ms game frame.
The 5-second `[interp]` stats line reports it as `tape close X ms (guest: arrays, copy, draws,
rest, cut)`: vertex-array bookkeeping, copying the tape, pairing draws, pairing leftover values by
source, and the camera-cut check.

In the foyer (~3,300 values per tape) the per-draw pairing took 13.4 ms: groups of 448, 224 and
125 identical particle draws were compared all-pairs (up to 200,000 candidate pairs per group,
every one sorted, including the ones rejected by the motion limit), and every draw's position was
re-read from the tape for every comparison. Now (paired/unpaired counts unchanged within
run-to-run noise):
- each draw's position is read once; the common motion is the median over at most 64 sampled
  draws instead of all of them;
- in groups of more than 1,024 pairs, only predecessors within the motion limit are visited
  (sorted along the axis the group is most spread out on, then a range scan), so a draw has a
  handful of candidates instead of hundreds; rejected pairs are never stored;
- particles (one CPU-built vertex block per draw) have their first vertices decoded once per
  group, and candidates are sorted as packed integer keys;
- the per-draw index lists are one flat array (one allocation instead of one per draw).

Foyer: tape close 13.7 ms -> 1.05 ms (draws 13.4 -> 0.8 ms; copy 0.1-0.2 ms); opening cutscene
and title 0.1-0.3 ms.

## Everything interpolates (interp-everything, October 2026)

Goal: every visible thing moves smoothly between the 30 Hz game frames, a replay never shows
memory the game has already changed, and shadows are both smooth and stable. Parts marked
**generic** carry over to other games on the same runtime (SMS); the rest is LM glue.

### Test scenarios and metrics

`tools/interp/scenarios.sh <out dir> <scenario> [fps] [steps]` runs one with a scratch
APPDATA/`LM_CARD_DIR`, `LM_INTERP_TEST=3` (fresh replay + two in-betweens + late replay per game
frame, see below), `LM_INTERP_TRACE=1` and its deterministic `--input`, and prints
`tools/interp/metrics.py`'s summary (`LM_DISC`, `LM_PYTHON` with numpy + Pillow):

| Scenario | What happens | Frames |
|----------|--------------|--------|
| nursery (`LM_START_MAP=2 LM_START_APPEAR=1`, `LM_LIGHTNING=0,90,1`) | walking, fast turns (stick reversals), nozzle aiming (C stick), flashlight, lightning strikes with shadows, GBH open/close (Y), dust, spiderwebs, door to the next room | 2580-2960 |
| ghosts: gold ghosts room (`LM_TELEPORT=0,550,-3300,100`, `LM_GHOST_HUNT=9 ...`, see RENDERING.md) | ghosts appear, hover, attack, vanish; stun, vacuum, capture | 3280-3600 |
| opening (title, `--input "600:START*10,1000:A*6"`, nothing skipped) | the attract/opening cutscene: crows, camera cuts, gate, foyer entrance | 1880-2120 |
| fire: nursery, fire element (`LM_TEST_STATE=poltergust+fire`) | fire blown (L), vacuum (R) with its air stream and dust, turns | 2580-2920 |
| water | as D with the water element | 2580-2920 |
| ballroom (`LM_TELEPORT=1800,0,-1400,100`) | ballroom, quick left/right turns, nozzle aiming, walking | 2580-2900 |

Per game frame (from the dumps; pixel threshold 40/255, blobs = after a 2-pixel erosion so
edges that legitimately move one pixel per in-between are not counted):
- **stale**: pixels where the late replay differs from the fresh one (must be 0);
- **snap blobs**: regions that change completely between the previous frame and the first
  in-between and then stay (something shown unblended), or only at the last step;
- **back-and-forth**: pixels whose brightness goes up and then down (or the reverse) by more than
  24 within one interval (inconsistent blends, e.g. a shadow sliding away and back);
- per in-between, the values shown unblended (snapped) and the values without partner.

### What changed

1. **Textures and TLUTs are copied at capture time** (`interp_tex.cpp`, generic). A replay read
   texel data by pointer when it first needed a texture; a late replay could find the memory
   already reused. Every `GXLoadTexObj`/`GXLoadTlut` in a tape now points at a capture-time copy
   (shared by texture object + data version, else by content hash, so a texture is copied once).
   Textures go through Aurora patch 0028's new *content* pointer, so they keep their address for
   EFB-copy lookups. A texture over memory an EFB copy wrote is left to Aurora's copy paths only
   while that memory still holds what it held at the copy (hashed at the copy and at the load;
   copy-to-RAM writebacks update the hash); the GBH, for one, later loads ordinary textures into a
   buffer it once copied the screen into.
2. **Normals bound through `GX_VA_NBT`** were not tracked by the vertex-array copies (the slot was
   computed from the attribute number, 25), so bump-mapped models (the GBH body, ...) were drawn
   from live memory. Found by poisoning guest memory during a replay and bisecting the range
   (`LM_INTERP_POISON`). Generic.
3. **EFB copies per replay** (Aurora 0028 `GX_AURORA_COPY_GENERATION`, generic). A replayed tape
   re-runs its EFB copies; they used to overwrite the same copy textures the tape's own early draws
   read (copies made in the previous frame), so every replay after the first showed the frame's
   own copies where the previous frame's belonged. Copies now go into one of four generations
   (the tape's sequence number), and the replay state slots (0021) save/restore the copy-texture
   map and Aurora's copy-to-RAM entries with the rest of the state. During replays copies are not
   dropped for memory the CPU changed (the guest is on a later frame by then).
4. **Copy-to-RAM readbacks only from the first replay of a tape.** Every replay wrote the frame's
   copies back into guest memory again, up to a frame later, over memory the game may have reused
   meanwhile (the GBH's screen-capture buffer holds GBH model data once it closes): guest memory
   corruption, visible as the GBH vanishing while it closes. Generic (Aurora 0028, bit 6).
5. **Colors blend** (generic): channel ambient/material colors, light colors, TEV color and
   konst registers (also when a display list sets them), fog color. They belong to their draw and
   are paired through the draw pairing (the n-th write of a register for that draw). Lightning
   flashes and fades now fade at the presented rate instead of in 30 Hz steps.
6. **Camera factoring** (generic; LM supplies the view matrix address, `interp::viewMatrixSource`
   in shadows.cpp: the game camera's GX view matrix, `lbl_803A3820 + 0x18C`). Every position/normal
   matrix and light is noted with the camera it was drawn under (sampled from guest memory at the
   load, so the shadow pass is factored by the light's camera). A model-view matrix is blended as
   blended camera x blended world matrix: the scene moves as one rigid whole (each product used
   to follow its own path in view space), billboards stay exactly camera-facing, far objects keep
   their distance while the camera turns. Draws are paired by their world-space positions, and
   draws of different passes (main, shadow caster) are kept apart.
7. **Shadows: smooth and stable** (LM; `LM_SHADOW_SNAP=3`, the new default). The caster pass is
   factored by the light camera LV and blended like everything else, so the shadow map is
   re-rendered at every presented frame from the blended light and casters. The receivers' shadow
   texture matrices T = K x G (K = the game's light projection or depth-ramp matrix x LV, read from
   the shadow object after the caster setup; G = the receiver's world matrix, or the inverse view
   the game built the actors' post-transform matrices with) are rebuilt as K(t) x G(t) with
   K(t) = P(t) x LV(t) from the same blended light. G is solved from the projection matrix of the
   same draw (the depth-ramp K is singular). In test mode a self-check rebuilds every factored
   value at alpha 1 and compares it with the captured one (`self-check: 0 of N ... differ`).
   `LM_SHADOW_SNAP=1` keeps the old 30 Hz shadows.
8. **2D is interpolated** (generic): HUD panes, screen transitions and dialogue boxes are paired
   per draw like 3D (not camera-factored); a 2D matrix that jumps more than 96 units snaps.
9. **Camera cuts from the game camera** (generic): a tape is a cut when the camera the most
   position matrices were drawn under moves its eye by more than 250 units or turns by more than
   30 degrees in one game frame (the matrix-based tests stay as a fallback). In the opening
   cutscene this catches all five shot changes; walking and turning stay far below.
10. **Lights stay themselves**: a GX light slot holds whichever light object the game loaded for
    that draw; a light is blended only with the same light object of the previous frame, and its
    color only when its position was blended (a lamp's color at the flashlight's position made the
    in-between frames brighter than both neighbours in the water scenario). Values drawn from a
    camera that jumped (the shadow pass when another light takes over: flashlight, room light,
    lightning, a spray) are shown as captured, and so are the receivers' shadow matrices.
11. **Pairing within a draw**: a skinned model that loads its joints in another order the next
    frame (index pairing would make them jump) is paired nearest-first instead. Scale changes blend
    up to 16x (particles growing, eyelids closing); environment/reflection texture matrices blend
    like rotations unless their translation wraps.

### Results

Before (main at the start of this work, 55748e4) and after, same scenarios and frames (snapped and
unpaired are averages per in-between frame over all value kinds; "snap blobs" include the
game's own instant events: lightning strikes, the GBH opening/closing cut, the flashlight or room
lights switching on, a fire burst appearing):

| | stale px | snap blobs | back-and-forth | snapped / in-between | unpaired / in-between |
|---|---|---|---|---|---|
| nursery, before | 83,150 (GBH closing frame) | 1,015,870 | (not measured) | 1.8 | 23.2 |
| nursery, after | **0** | 640,861 (412,806 are 3 instant events: lightning strike, GBH open/close cuts) | 63,810 (snapped shadows: 68,317) | 4.3 | 32.4 |
| ghosts, after | **0** | 135,006 (52,681 the flashlight going off/on for the vacuum) | 28,592 | 3.8 | 38.2 |
| opening, after | **0** (the crow frame had 88,685) | 118,614 (the shot changes, all detected as cuts) | 11,087 (texture detail sliding under the moving camera) | 0.0 | 4.2 |
| fire, after | **0** | 599,103 (room lights on, fire bursts appearing) | 30,076 | 14.9 | 45.0 |
| water, after | **0** | 708,590 (room lights on, spray starting) | 63,009 | 19.6 | 70.1 (13 are spray droplets moving too fast to pair) |
| ballroom, after | **0** | 254,740 | 35,786 | 2.0 | 42.3 |

More values are interpolated now (colors, 2D, the shadow caster pass), so the snapped/unpaired
counts per frame are not comparable to before one to one; per tape about 1.5% of ~5,300 values
have no partner (spawns, despawns, the frame a light or effect switches) and ~0.1% are shown
unblended.

Shadows: with the old blending (`LM_SHADOW_SNAP=0`) the flashlight shadows slid back and forth
(documented in RENDERING.md); snapped (`=1`) they were stable but moved at 30 Hz (all of a
shadow edge's change lands on the first in-between: counted in the snap blobs); now (`=3`) they
move with the presented frame and the back-and-forth count over scenario A is at the snapped
level (63,773 vs 68,317; the first attempt, with the actors' receivers rebuilt from the sampled
camera, had 400,650 and the self-check showed 124 mismatching values per frame, now 0).
Original 30 is byte-identical to main (17 dumped frames of scenario A at 30 fps, incl. lightning,
GBH and the door, compared pixel by pixel with main's executable: 0 pixels differ).

### Cost

Nursery walk (scenario A without test mode), RTX 3090 Ti, 120 Hz display with VSync: target
120 -> 120.0 fps presented (240 -> 120: the display), game 29.8-30.2 fps. Per game frame: tape
~630 KiB + vertex-array copies ~360 KiB; texture copies are made once per texture (4 KiB per
frame while walking, a few MiB when a room loads). Tape close (guest thread) 1.4-2.6 ms, of which
pairing 1.1-2.3 ms (up from ~1 ms: colors, more values per draw); replay 0.7-0.8 ms per presented
frame (host). About 5,300 values paired per tape, 60-130 without partner, 1-7 shown unblended per
presented frame.

## Polish: fades, particles, shadow depth (interp-polish, October 2026)

1. **Spawns and despawns fade** (generic; Aurora patch `0030-interp-fades`). A draw without a
   partner used to pop in at the first in-between frame (spawns) or vanish there (despawns).
   - `GX_AURORA_SET_FADE q` makes later draws drop q/255 of their pixels with a 4x4 ordered
     dither (the fragment shader discards; the amount rides in bits 8..15 of the per-draw
     `current_pnmtx` immediate, so no pipeline or uniform changes). Dither instead of alpha works
     for opaque and blended draws alike and keeps depth right; at 120+ fps it reads as a fade.
   - Fade-in: each 3D main-pass draw of b without a partner (a model-view matrix or CPU-built
     vertices, none paired) is bracketed by `SET_FADE (1 - alpha)` / `SET_FADE 0` in the replayed
     stream (draw byte ranges are recorded at capture: a display list's inlined bytes, an
     immediate draw from its setup to its last vertex).
   - Fade-out: a's draws without a partner are drawn again from a's own commands, so their state
     (textures, TEV, lights, arrays) is a's. `GX_AURORA_SET_REPLAY_MASK` runs a's commands with
     draws, EFB copies, offscreen passes, depth snapshots, extra lights and texture evictions
     skipped; only the vanished draws are unmasked, at `SET_FADE alpha`. The pass is spliced into
     b's stream after the draw paired with the closest earlier paired draw of a, between state
     slot 4 (b's state, saved and restored around it) and slot 5 (a's state so far: starts as the
     end state of a's predecessor; slots 4..7 hold GX state only). At most 8 passes per replay,
     in a's order.
   - Values without partner (spawned draws, vanished draws) are placed under the blended camera
     (their world placement seen from the in-between camera), so they move with the scene.
   - Not faded: draws sampling an EFB copy (the TEV reads a copy destination; screen effects that
     redraw the frame, the GBH screen, shadow receivers: fading a full-screen redraw of the frame
     darkened the whole picture on the lights-on frame), the shadow caster pass, 2D, and cut frames.
   - `LM_INTERP_FADES=0|in|out` (test aid) turns them off or keeps one direction.
2. **Fast particles pair by predicted position** (generic). For groups of many draws of one
   kind, each draw of a is expected where its own motion since its predecessor carries it (falls
   back to the group's common motion); candidates are limited and, for CPU-built particles,
   ranked by the distance from that prediction. Water spray droplets (point sprites) without
   partner went from 13.0 to 10.4 per in-between frame (the rest are born or die and now fade).
3. **Shadow depth from b** ("shadows at any distance" refits the light's near/far every frame,
   and the depth ramp is not linear in them): the caster pass's projection keeps b's depth terms
   (x/y blended) and the receivers' ramp matrix uses b's projection with the blended light view,
   so caster depth and receiver ramp always use the same near/far.
4. **New scenarios** (`tools/interp/scenarios.sh`): `call` (E. Gadd's GBH call: call screen,
   portrait, four message windows), `pause` (pause screen, its pages, back), `ending` (ending
   scene, rank A, caption fade). Env: `LM_RANGE`, `LM_SECS`, `LM_EXTRA_INI` (e.g. enhanced lighting
   and world lights), `LM_EXE` (another build), steps 0 = a real-time run.

Results (same metrics as above; "before" = the interp-everything results):

| | stale px | snap blobs | back-and-forth | snapped / in-between | unpaired / in-between | fades in/out |
|---|---|---|---|---|---|---|
| nursery | 0 | 639,731 (before 640,863) | 63,749 (64,824) | 4.3 | 32.4 | 6.7 / 5.7 |
| ghosts | 0 | 39,413 (131,433) | 25,142 (28,230) | 3.0 | 12.8 (36.8) | 2.6 / 2.3 |
| opening | 0 | 126,450 (126,450) | 11,137 (11,137) | 0.0 | 4.1 | 0 / 0 |
| fire | 0 | 684,471 (630,933; room lights on, fire burst: game events) | 27,484 (31,919) | 14.3 | 48.3 | 8.3 / 7.5 |
| water | 0 | 594,325 (647,350) | 57,155 (89,140) | 20.3 | 70.4 (13 droplets -> 10.4) | 22.1 / 20.4 |
| ballroom | 0 | 242,973 (254,740) | 36,694 (35,786) | 2.0 | 38.3 (42.3) | 5.3 / 5.0 |
| call | 0 | 95,865 | 6,829 | 1.8 | 11.9 | 2.2 / 2.0 |
| pause | 0 | 368,019 (opening/closing the pause screen) | 4,713 | 1.1 | 16.6 | 1.8 / 0.5 |
| ending | 0 | 1,081 | 0 | 0.0 | 1.1 | 0 / 0.1 |
| ballroom + enhanced lighting, world lights, light shadow maps, 120 / 240 | 0 / 0 | 263,675 / 262,684 | 35,820 / 35,669 | 2.0 | 38.3 | 5.3 / 5.0 |

- Fades do not add dither noise to the back-and-forth metric (nursery 63,775 without, 63,749 with).
- The call's message windows, the pause screen and the ending caption interpolate (their opening
  and closing are the game's own one-frame events). The message window appears in one frame and
  is not faded (2D).
- Light shadow maps and world lights: the same tape at 120 and 240 targets gives the same metrics;
  they are rebuilt per replay from blended lights and casters.
- Original 30 is byte-identical to main (19 frames of the nursery scenario at 30 fps, main's
  executable vs this branch: 0 pixels differ).
- Scheduler changes (guest threads release the CPU during the frame handoff, GXDrawDone and DVD
  waits): a decoupled guest records and closes its tapes as before (the present path never waits
  for the host); real-time nursery run at 120: alpha rises monotonically within a tape (5 of
  2,000 presents stepped back), tape close 1.7-4 ms on a machine shared with two other runs.

## Pass 3: pacing, presentation cost, pairing, blinks (interp-pass-3, October 2026)

User report after a full playthrough at high frame rates: "many things still judder frequently
while moving" (easiest to see: the foyer candlesticks and door while Luigi strafes with the camera
following), parts of Luigi (the flashlight) blink out, and the damage blink looks wrong.

### What was wrong

1. **Presentation collapsed in heavy rooms** (the static door and candlesticks judder). A replay is
   processed by Aurora's FIFO thread inside `aurora_end_frame` (drain). In the foyer that took
   ~25 ms per present, so a 120 target presented at ~36 fps: frames landed on alternating 3 and 4
   refreshes of the 120 Hz display, and everything that moves on screen (the whole room under a
   moving camera) juddered. The cause was the early-Z accuracy split (Aurora 0170/0179, F1 >
   Visuals > Rendering "Early depth for cut-out textures"): ~16,000 draws per foyer frame were
   split into a depth-only and a colour pass, each with two whole-config pipeline lookups, and the
   0179 fix stopped consecutive draws from merging, so every triangle strip became two draw
   commands. Measured (FIFO drain per present, foyer, 120 target): 25-30 ms with the split,
   3.7-8 ms with it off.
2. **Alpha from the guest's finish time** (`LM_INTERP_PACING=old`). alpha = (now - time tape b was
   finished) / (b's stamp step): every in-between depended on when the game thread happened to
   finish its frame (+-4 ms of work jitter), and the frame stamps (the retrace count at the
   present) step 1 or 3 fields instead of 2 whenever a frame's work crosses a retrace (3-4% of
   frames in the foyer): alpha then reached 1 after half the time and held (a stall), or jumped.
3. **Luigi's draws lost their partners when he started moving.** The polish pass predicted where a
   draw would be from its own motion in the previous frame (fast particles) and rejected partners
   more than 12 units (+1% of the distance) away. That also applied to models drawn a few times per
   frame (Luigi is drawn in several passes, so his draws are "many"): on his first walking frame
   all 231 of his position matrices (body, Poltergust, hose, flashlight) were unpaired, vanished
   and reappeared as a whole: the flashlight blinking out, and a stray flashlight cone across the
   room in the in-betweens (his vanished draws re-expressed under the wrong camera: the camera of
   a tape was mapped to the other tape's by the first paired value).
4. **Blinks became fades.** Draws without a partner faded with a dither (polish pass) and color
   registers were always blended, so anything the game switches on and off per frame passed
   through half-states that neither frame shows.

### What it does now

1. **Aurora 0192** (`patches/aurora/0192-early-z-split-cache-merge.patch`): the split decision and
   its two pipelines are cached with the draw cache, and while interpolating
   (`GX_AURORA_ACCURACY_EARLY_Z_MERGE`, set by `gx::applyAccuracySettings` when the frame-rate
   target is above 30) consecutive draws with the same state merge into one split pair (both halves
   extended alike). Foyer FIFO drain per present: 25-30 ms -> ~11 ms (cache) -> **3.9 ms**
   (merged). The merged pair differs from the per-draw order only where merged geometry overlaps
   itself with alpha-rejected pixels: nursery test-mode dumps, merged vs exact, 578 px in 345
   frames (edges of the Game Boy Horror hand, max 28 levels). Original 30 keeps the exact split.
   `AURORA_EARLY_Z_MERGE=0/1/ab` overrides.
2. **Presentation clock** (`interp.cpp`, `Pacer`, `pace()`). Tapes go into a queue; per present:
   - a smoothed present clock (advances by the measured present period, pulled 10% toward the
     real time, resynchronized when a present is more than 4 ms off: a missed refresh);
   - a game timeline: each tape one nominal step (median of the recent stamp steps, 2 fields at 30
     fps) after its predecessor, pulled 10% toward its stamp; a stamp more than 1.5 fields off (a
     real slowdown, a load) is taken as is;
   - the shown game time G = clock - D, where the delay D covers the slowest recent tape (finish
     time minus its predecessor's game time, over 90 tapes, load hitches excluded) plus 1.5 ms;
     G follows its target with at most +-5% speed changes, never runs backwards and never beyond
     the newest tape (counted as a stall; 30 stalls in a row measure D afresh);
   - the pair around G and alpha from their times. `LM_INTERP_PACING=old` restores the old rule.
   The 5-second stats line reports the delay, stalls and resyncs.
3. **Pairing**: groups of up to 8 draws of one thing pair nearest first without the motion limit
   (it is for particle swarms; swarms accept their own or the common motion); draws left unpaired
   are paired again by display-list content (FNV hash: a list that moved in memory); cameras are
   mapped between tapes by majority vote of the paired values.
4. **Switches stay switches**: draws in a *snap scope* (`interp::pushSnapScope`, LM: the actor draw
   pass `fn_80060E70`, i.e. Luigi, ghosts and other characters) and draws whose identity vanishes
   or appears as a whole and did so within the last 24 tapes (blink patterns) are never dithered:
   a vanished one stays fully drawn and a new one hidden until alpha 1, the console's timing. A
   color channel that jumps by more than 96 levels in one game frame changes at the game frame
   too (a's value until alpha 1). Particles (many draws of one identity) keep their fades. The
   damage blink is Luigi drawn solid and translucent on alternate game frames (Original 30
   dumps of the ghosts room: frames 3590-3650 after a hit at 3512), done with color registers:
   during the blink the trace counts 146 color switches per in-between. Test mode, ghosts room,
   hit at frame 3546, Luigi's screen region at the half-way in-between of frames 3650-3653: mean
   brightness 24.3 / 20.2 / 23.4 / 19.7 with the previous frame at 24.9 / 21.4 / 23.6 / 20.6 and
   the next at 21.4 / 23.6 / 20.6 / 23.4 (mean abs. difference to the previous frame 2.5-4.6, to
   the next 4.3-5.0): every in-between shows the previous frame's blink state, no half-states.
5. **Lifetime**: the last replays' tapes are kept alive until their FIFO work is drained (a
   crash reading a vertex-array copy in `push_gx_draw` was seen twice under heavy load).

### Regression suite

`tools/interp/scenarios.sh` (all runs muted: `LM_AUDIO=0` unless set) gained:

| Scenario | What happens | Frames |
|----------|--------------|--------|
| foyer (`LM_START_APPEAR=6`) | foyer by the front door: strafing left/right with the camera following (candlesticks, door, stairs), turns | 2600-2900 |
| hit (`LM_TEST_HIT=2620,60,5`) | nursery, Luigi hit every 60 frames through the game's health setter while walking (hurt sparkles, heart effects) | 2600-2760 |

Steps `0` is now a **real-time judder run**: no dumps, `LM_INTERP_JUDDER=<run>/judder.log` (per tape:
stamp, finish time, frame number; per present: the tapes and alpha shown, the delay; host-loop
phase times), summarized by `tools/interp/judder.py <log> --frames first,last [--dump N]`:

- *present interval*: mean / p50 / p99 / max time between presents;
- *step error*: per present |du x P - dt|, u = a + alpha (the frame shown as a fractional tape
  index), P the nominal frame period: how far one present's motion step is from the ideal (x an
  object's screen speed = its judder in pixels);
- *shown-time error*: |u x P - t| after a 1 s moving median: late/early frames, uneven alpha
  (also counts the deliberate +-5% slews);
- *stalls* (same frame shown twice), *reversals*, *alpha-1 presents* (waiting for the game) and the
  tape stamp steps.

Test-mode metrics (`metrics.py`) now also report dithered vs snapped fades and color switches.
Test aids: `LM_TEST_HIT`, `LM_HEALTH_LOG=1` (Luigi's health changes with frame numbers),
`LM_INTERP_PACING=old`, `AURORA_EARLY_Z_MERGE`.

### Results

Real time, foyer scenario frames 2600-2900, 120 target on a 120 Hz display with VSync, new and old
pacing run at the same time on a machine shared with 2-4 other game instances (absolute rates are
lower than on an idle machine; the comparison is fair):

| | presented | step error p95 / p99 / max | stalls | alpha-1 presents |
|---|---|---|---|---|
| before (main: early-Z unmerged, old pacing; earlier run, 1 other instance) | 36 fps (27.8 ms intervals) | (pre-metric: G - t error p99 2.8 ms at 36 fps) | | 31 |
| old pacing, 0192 | 71.8 fps | 9.27 / 12.69 / 19.19 ms | 10 | 73 |
| **new pacing, 0192** | 86.9 fps | **3.59 / 7.12 / 10.39 ms** | **0** | 3 |
| new pacing, 0192, less load | 99.0 fps | 2.99 / 3.61 / 5.41 ms | 0 | 2 |
| 144 target, VSync off, old pacing (run at the same time as the next) | 105.7 fps | 8.73 / 13.94 / 21.84 ms | 44 | 107 |
| **144 target, VSync off, new pacing** | 101.0 fps | **3.22 / 3.64 / 4.59 ms** | **0** | 0 |

The remaining step error is the host loop's own timing jitter (present intervals p50 9.6 ms, p99
14.8 ms under this load); the presented rate is limited by the replay cost on a shared CPU (host
replay 2.7-3.7 ms + FIFO drain), not by the pacing.

Test mode (same scenarios as before; foyer and hit are new):

| | stale px | snap blobs | back-and-forth | snapped / in-between | unpaired / in-between | fades dithered in/out |
|---|---|---|---|---|---|---|
| foyer 2600-2700, before pass 3 | 0 | 21,425 / 55,349 | 163,665 (the stray flashlight cone) | 19.5 | 29.4 | 11.4 / 8.3 |
| **foyer 2600-2700, after** | 0 | 26,598 / 22,642 | **31,913** | 21.0 (Luigi's instant 180-degree turns) | **9.1** | 6.5 / 3.5 |
| foyer + enhanced lighting | 0 | 24,954 / 24,155 | 31,649 | 12.4 | 6.6 | 6.0 / 1.1 |
| foyer, 16:9 | 0 | 16,915 / 15,773 | 18,511 | 12.5 | 6.6 | 6.0 / 1.1 |
| hit 2600-2760 | 0 | 34,293 / 35,869 | 22,092 | 1.5 | 6.5 | 7.2 / 5.2 |

## Pass 4: what reaches the screen (interp-final, October 2026)

User report after pass 3: "dust, completely static objects, many things still judder", with
screen recordings (ShadowPlay, 4K 120 fps) showing a candlestick shaking under the moving camera
and a small extra copy of Luigi for a moment. The earlier passes measured dumps and the game's
own pacing log; this pass measures the display output under the user's real conditions.

### The user's conditions

- RTX 3090 Ti, LG TV 3840x2160 at 120 Hz with G-SYNC on, VSync on in the game (Aurora picks the
  Fifo present mode), frame rate "match display" (120), enhanced lighting (realistic flashlight,
  colored lights, light shadow maps, AO), CRT filter, windowed or fullscreen.
- G-SYNC confirmed from the display's refresh timing (D3DKMTWaitForVerticalBlankEvent): with the
  game paced at 90 fps fullscreen the refreshes come every 11.0 ms (p5-p95 9.7-12.8 ms), not on a
  8.33 ms grid.
- The user's recordings show 22-32 distinct frames per second (08.31.34: 74% of the 120 captured
  frames repeat the previous one, steps of 36 px at the game rate; 08.37.23: 81%, down to 17 fps).
  The machine was shared with agents' test runs then: a second game instance halves the GPU
  (measured: 120 -> 65 fps presented, 3-field tape stamps, the game itself slowing down).
  Parallel game windows are now serialised by a machine-wide lock (`LM_GAME_LOCK`).

### Method: `tools/interp/screen_run.py` (the primary regression test)

Runs the game visibly (fullscreen, topmost: `LM_TOPMOST`), muted, with a scratch APPDATA holding
a copy of the user's settings.ini (read only) plus `--set` overrides, waits for the scenario's
frame and records the display with ffmpeg's ddagrab (Desktop Duplication, `dup_frames=0`: one
captured frame per screen update; NVENC HEVC). Then:

- `screen_judder.py`: per pair of shown frames, the shift of the static scenery (phase
  correlation of image blocks in the top third of the picture, away from Luigi, his beam and the
  dust; median of the blocks; flash frames left out). Reports shown fps and interval spread, the
  position error of the scenery against a smooth path (px at 4K), "even steps" (step within 25%
  of the local speed x interval).
- `LM_FRAME_TAG=1` draws a bar code in the top-left corner (ImGui, after the CRT): present number,
  tape and alpha. The capture then knows which present and which interpolation position u = tape +
  alpha reached the screen: repeated and skipped presents, and the blending check: the scenery
  step of each shown frame divided by (du x the local speed per game frame), which must be 1;
  reported as `|r - 1|` and the share off by more than 25%, and per place in the tape (#0 = the
  present that crosses a game frame).
- `judder.py` on the game's log: present intervals, and with Aurora patch 0197 the GPU completion
  time of every frame (P lines). With VRR a frame is shown about when the GPU finishes it, so the
  display step error is |du x 33.37 ms - (gpu done step)|. `--camera`: the blended main camera of
  every present (V lines) and a fixed world point projected through it (the math alone).
- `blips.py`: one-frame artifacts (pixels unlike both neighbours), with crops of the worst frames;
  `osc_map.py`: a heat map of back-and-forth; `track_bright.py`: a self-lit feature (candle
  flames) tracked against u; `dump_motion.py`: scenery steps through the in-betweens of an
  `LM_INTERP_TEST` run (deterministic).
- `screen_suite.py`: all scenarios (foyer strafing, turning, vacuum/heavy dust, nursery,
  ballroom, ghosts) into one table. `lockrun.py` runs any command under the game lock.

### What was wrong

1. **Pacing under GPU load and VRR.** Presents were started whenever the swap chain let the loop
   continue; alpha came from the time the replay started. At the VRR ceiling (120 on 120 Hz) the
   display falls back to fixed timing and the queue adds latency; below it (the GPU busy: 65 fps
   in the shared-machine run the user recorded) frames 13-17 ms apart carried motion computed for
   other times: display step error 4.1 ms mean, 8.7 ms p95.
2. **Pairs across cameras.** A position matrix whose partner in the previous tape was drawn under
   another camera (a GX camera slot used by the shadow light in one frame and by the main camera's
   draws in the next) was blended between the two cameras: drawn in between, small and far off,
   or snapped to b for all in-betweens while the rest of the scene moved (static scenery stepping
   at 30 Hz in some tapes). The camera-cut test compared b's most used camera with a's most used
   camera on its own, which can be another pass's camera.
3. **Redrawing vanished draws from the previous tape** (fade-outs, and characters kept fully drawn
   until alpha 1) used GX state that was not the draw's own in some frames: a stray flashlight
   cone across the room when the vacuum switched it off (foyer, test mode frame 2603), a miniature
   of the whole picture over a black screen among heavy dust (frame 2685), and Luigi's body
   replaced by a small copy for a game frame (the user's video).
4. **Dust.** Particle swarms (hundreds of identical one-quad draws) were paired nearest-first with
   second choices: a puff whose nearest was taken blended toward another puff; the unpaired ones
   faded in and out with a 4x4 dither (29.5 fade-ins and 20.3 fade-outs per in-between while
   vacuuming): flicker and popping.
5. **Menus**: groups of thousands of identical glyph quads were paired all-pairs (tape close up to
   3.4 s per tape on the title/file screens: boot took 3-4 minutes in test runs).

### What it does now

1. **Pacing** (`src/recomp/pacing.cpp`, Aurora `0197-present-timing`):
   - interpolated frames start on an even schedule and are interpolated for their scheduled
     time (the constant latency to the display is absorbed by the interpolation delay);
   - variable refresh (detected from the refresh timing, `[pacing] variable refresh rate
     detected`, or F1 > Visuals > Variable refresh rate On/Off): a grid a little below the refresh
     rate (116 fps on 120 Hz: inside the VRR range), slowed evenly to what one frame costs (p90 of
     max(CPU, GPU) time per frame + 8%) when the PC cannot hold it;
   - fixed refresh: the refresh rate (Fifo) or an even fraction of it (60, 40, 30 fps on 120 Hz),
     started at the vblank, never an uneven rate (`LM_PACING_DIVISOR=n` forces one for tests);
   - the GPU is kept at most one frame behind (`aurora_wait_frame_done`), so the time from a
     frame's start to its display stays nearly constant;
   - the F1 frame-rate section and the FPS overlay show the pacing ("116 fps (variable
     refresh)", "60 fps = 120 Hz / 2 (frame cost limits)").
2. **Cameras**: the cut test uses b's main camera and the camera of a its draws were paired with
   (majority). A value whose partner was drawn under another camera than its own camera's
   counterpart is placed under the blended camera of its own camera pair (or shown as captured on
   a cut), never blended across cameras (`crosscam` in the test-mode trace).
3. **No redraw of vanished draws** (`LM_INTERP_FADES=out|all` restores it for tests): a vanished
   draw disappears at the first in-between frame (at most one game frame early); a snapped spawn
   (characters, blinks) is shown at once. Spawns still fade in by dithering.
4. **Swarms** pair mutual nearest neighbours only; the other particles are this frame's particles
   placed under the in-between camera, drawn fully, never dithered or redrawn from the previous
   tape (`LM_INTERP_SWARM=old|any|fades` for A/B).
5. Huge identical-draw groups use the range-scan pairing too (menus: tape close <= 3.5 ms).

### Results

Real time, fullscreen 3840x2160 at 120 Hz (G-SYNC), the user's settings with enhanced lighting,
one game instance (lock), 20 s per scenario. "Before" = main (e86513c) as built at 01:31 (no frame
tags: no blending columns), "after" = this branch.

Static scenery on screen (`screen_suite.py`; position error = distance of the scenery from a
smooth path, px at 4K; even = steps within 25% of speed x interval; blending = the step against
linear interpolation between game frames, from the frame tags; blob = largest one-frame
artifact of the run, px at 1280 wide):

| scenario | build | presented (screen) | position error mean / p95 px | even steps | blending off >25% | display step err mean / p95 ms | worst one-frame blob |
|---|---|---|---|---|---|---|---|
| foyer strafing | before | 120 (118.7) | 0.82 / 2.78 | 89.0% | - | - | 764 |
| | after | 116 (114.4) | **0.69 / 2.08** | **92.1%** | 4.0% | 0.43 / 1.05 | 265 |
| turning | before | 120 (117.8) | 2.21 / 9.18 | 59.6% | - | - | 966 |
| | after | 116 (103.5) | **1.82 / 7.47** | **69.3%** | 11.6% | 0.56 / 0.98 | 231 |
| vacuum, heavy dust | before | 120 (89.6) | 0.46 / 1.21 | 96.2% | - | - | 1024 (dust puffs for one frame) |
| | after | 116 (75.3) | 0.49 / 1.31 | **98.6%** | 1.4% | 0.71 / 1.18 | 276 (Luigi's instant turn) |
| nursery | before | 120 (112.1) | 1.76 / 7.72 | 77.6% | - | - | 328 |
| | after | 116 (108.8) | 1.77 / 7.95 | 76.7% | 12.3% | 0.56 / 1.01 | 123 |
| ballroom | before (240 Hz capture timestamps) | 120 (88.3) | 1.32 / 5.08 | 79.8% | - | - | 908 |
| | after | 116 (86.2) | 1.15 / 4.22 | 81.1% | 6.8% | 0.36 / 0.89 | 947 (a flying chair) |
| ghost fight (gold ghosts room, the camera nearly still) | after | 116 | - | - | - | 0.25 / 0.57 | 426 (ghost glow) |

(The screen column counts distinct captured frames: frames that differ by less than 0.25 levels
on average, e.g. a slow pan with the flashlight off, are counted once.)

GPU-bound (foyer at 8x internal resolution: about 28 ms of GPU work per frame), the case of a
shared or slower GPU:

| | presents (game log) | shown on screen | shown intervals p50 / p95 / max ms | position error mean / p95 / max px | even steps |
|---|---|---|---|---|---|
| before | 61.8 fps | 36.1 fps | 32 / 37 / 52 (34.7% off the median) | 1.47 / 4.44 / 27.9 | 93.8% |
| after | 35.4 fps | 35.3 fps | 28 / 30 / 32 (0.2% off) | 1.09 / 3.74 / 18.0 | **97.7%** |

Before, the game believed it presented 62 fps while the screen showed 36 at uneven times, each
frame interpolated for a time it was not shown at; after, every frame is shown, evenly, at the
rate the GPU holds (display step error 0.62 / 2.10 ms). Original 30 is unchanged: 41 nursery
frames at 30 fps, main's executable vs this branch, 0 pixels differ (two runs of this branch: 0).

Pacing alone (game log), foyer strafing at the user's settings:

| | presented | present interval p99 / max ms | display step error (vs GPU done) mean / p95 ms |
|---|---|---|---|
| before, machine shared with a second game (what the user recorded) | 64.7 fps | 19.6 / 44.6 | 4.06 / 8.70 |
| before, idle machine | 120.0 fps | 10.1 / 14.2 | (no GPU times) |
| after, idle machine | 116.1 fps | 9.2 / 9.3 | 0.39 / 0.89 |

Test mode (`scenarios.sh`, 640x480 dumps, deterministic), vacuum scenario frames 2600-2700,
`LM_INTERP_SWARM=old` vs the new swarm handling: dithered fades per in-between 29.5 / 20.3 -> 0.2 /
0.3; back-and-forth 73,490 -> 32,152 px (46,060 -> 4,722 without frame 2603, the vacuum switching
the flashlight off); the miniature-picture frame 2685 (41,683 px) gone. With the fade-out redraw
off the stray flashlight cone of frame 2603 is gone too (back-and-forth 27,430 -> 141 px).

**Enhanced lighting's lights were never interpolated with the realistic flashlight on** (the
user's setting; found with the camera check below): `beginScene` noted the light count before
the flashlight's three anchor lights were appended, the capture check saw another count and
dropped the note, so every colored light and the anchors stepped at the game frame while the
scene moved. All lights are noted now and blended in world space under the blended main camera
(unpaired ones keep their world place, room boxes turn with the camera). The "after" rows above
include this fix.

The interpolation math itself is exact: the blended main camera's path (`judder.py --camera`, a
fixed point projected through every present's camera) is piecewise linear between game frames to
0.02% (median). On screen, with the game-frame speed changes of linear interpolation taken into
account, steps that cross a game frame are 1.087 x the expected step in the foyer with enhanced
lighting (1.168 before the light fix), 1.000 inside a game frame, and 0.986 / 0.999 in a lit room
without enhanced lighting (`LM_ROOMS_LIT=1`): what is left moves at the game frame by design
(Luigi turns instantly and his flashlight with him, so the lit region jumps; candle flicker; the
light shadow maps change shape: `blips.py` in the turn scenario shows one-frame orange glows and
shadow shapes behind the candles: enhanced lighting's light shadow maps, not geometry).

Fixed refresh with VRR off (`video.vrr=2`, `LM_PACING_DIVISOR=2`) on this VRR display first ran
at 26 fps: the vblank wait for every second refresh never came while the display waited for
frames; the start now falls back to the grid after one period: 59.9 fps presented, 59.4 shown,
shown intervals p95 19 ms (1 kHz capture timestamps), 94.4% even steps, display step error 0.67 /
1.64 ms.

## Pass 5: locked frame rates (fps-lock, October 2026)

User report: RTX 3090 Ti, 4K 120 Hz G-SYNC TV, VSync on. A 60 fps target showed 50-56 fps and 120
was low and uneven, with the GPU far from busy, while "Unlimited frame rate" ran 98-110 fps with
the same settings (enhanced lighting Ultra, ghost/world lights, AO, realistic flashlight 3, CRT,
smooth banding, 4K fullscreen).

### Method

- `LM_PACING_LOG=<file>` (pacing.cpp): one `F` line per presented frame with the time it entered
  `beforeFrame`, its scheduled start, the end of the schedule wait and of the GPU wait,
  `aurora_begin_frame` done, `aurora_end_frame` done, the render worker's submit, Present and the
  GPU completion (Aurora 0197), the frame cost per stage, the governor's limit, the period, the
  mode (G grid, V vblank, F swap-chain FIFO, U unlimited, L original 30), the shown and newest
  game frame, stall flag and keep-warm units; an `S` summary line per second.
- `tools/interp/pacing_stats.py <log> [--skip s] [--until s] [--dump] [--per-second]`: presented
  fps from the Present and GPU-done times, interval percentiles, intervals over 1.5x the target
  and which phase each one spent its time in (schedule wait = pacing chose a long period; worker =
  render worker until Present, where compiles show; cpu; gpu wait), cost and limit.
- `tools/interp/pacing_run.py <out> --scenario foyer|menus|mixed|... [--set k=v] [--fullscreen |
  --window WxH] [--secs S] [--lock FILE]`: runs the game muted with a scratch APPDATA (copy of the
  user's settings.ini + overrides), waits for the scenario's start frame, runs, prints the stats.
  `mixed` cycles strafing, vacuuming with heavy dust, turning and the pause screen + Game Boy
  Horror (60 Hz menu logic: the game rate switches 30 -> 60 -> 30 every cycle) for the whole run.
- The user played 9 minutes on a logging build (old pacing) at 4K Ultra, 60 target, naming the
  moments that felt bad (key pickup, Parlor scene, lab -> mansion, Toad's save menu, doors,
  chandelier); nvidia-smi logged clocks during menu tests.

### What was wrong

1. **The cost estimate poisoned the schedule.** The period was max(target, p90 of the last 45
   frames' cost x 1.08). One 100-670 ms frame (a first-time pipeline compile in the render worker)
   or a few 35-60 ms frames (a scene switch) put the p90 at 35-85 ms: the schedule then ran at
   12-30 fps for 2-10 s until the samples left the 45-frame window (longer the slower it ran) and
   sped up 2% per frame. Every moment the user named had this signature. In the user's session:
   618 intervals over 1.5x the target in 520 s, 576 of them chosen by the schedule itself.
2. **Feedback through the GPU clocks.** While slowed down the GPU is mostly idle and clocks down;
   the measured GPU time per frame rose from 9 ms (60 fps) to 20-27 ms (12 fps), which kept the
   estimate above the target.
3. **Compile hitches**: enhanced lighting draws known configs into its own render targets (the
   depth pre-pass and light shadow-map casters, the glow buffer); the precache built every config
   for the scene layout only, so these compiled on first use (64 MISSes in a 10-minute foyer run).
4. **Idle-clocked GPU after menus.** The pause screen and the Game Boy Horror draw ~0.5 ms of GPU
   work over a still picture; within half a second the GPU drops from P0 (1995/10501 MHz
   core/memory) to P3/P5/P8 (345-1000 / 405-5001 MHz). The first 3-4 frames of the full scene
   after the menu took 30-60 ms instead of 6 (at 120 fps: 20-28 ms for 5 frames): a stutter on
   every menu exit, and with the old estimate a 2-second slowdown after it.
5. **Present timing held Dawn's device lock.** Aurora 0197's waiter thread blocked in `WaitAny`
   with a timeout, which with implicit device synchronization holds the device lock until the GPU
   finishes: every other thread's Dawn call waited for the GPU. Interpolated modes hid it (the host
   waits for the previous frame anyway); original 30 at 4K Ultra fell from 28 to 18.5 fps whenever
   the record was on.
6. **Measurement artefacts**: the `[interp]` line's "game N fps" counted distinct game frames
   *shown* (never above the presented rate: "presented 14, game 13.9" looked like a guest
   slowdown when the guest made 28-30); its "pacing delay ~1350 ms" mixed the host clock with the
   VI retrace clock's epoch (not a latency). The FPS overlay itself was right.
7. The grid wait (`SDL_DelayPrecise`) overslept by ~10 ms about once in 5 minutes; the vblank
   mode's fallback used a condition-variable timeout (59.5 fps at a 60 target with VRR off).

### What it does now

- **Governor** (pacing.cpp): the target holds unless frames are actually late. A frame starting
  more than 25% late is a miss; a single very late frame (> 3 periods: a load, a compile) only
  restarts the grid. The rate is lowered only when a quarter of the last second's frames (at
  least 4) missed, or when half of them were late by more than 8% (the PC a little too slow:
  intervals alternating 8.6/10.7 ms are uneven motion on a VRR display), and then to the rate
  actually achieved + 3%. After 1 s without misses it speeds up 2% per frame, not below a rate
  that failed in the last 10 s. The overlay/F1 status says why: "(PC too slow for 120: GPU 9.8 ms
  per frame)".
- **Aurora 0214**: configs that are enhanced-lighting depth or caster variants (depthOnly /
  lightDepth) are precached for the depth-only target (only), glow-shaped ones also for the glow
  target. 64 -> 0 MISSes in the 10-minute mixed run.
- **GPU keep-warm** (Aurora 0215, F1 > Visuals > Rendering "Keep GPU clocks up in menus", on):
  while a frame is much lighter than recent play, a compute pass streams through a 64 MiB buffer
  after the frame's own work so the GPU stays about as busy as in play (target min(recent play GPU
  time, 90% of the period; at 120 fps 40%/60% caps still let the clocks drop: 69 -> 14 intervals
  over 1.5x in 2 minutes of menu cycling with 90%); grows below 75% of it, eases off above 115%, stops at once above 160%
  = the scene is back). ALU-only filler kept the core clock but the driver still dropped the
  memory clock (P3). Menu-cycle test (120 s, 60 target): intervals over 1.5x 44 -> 1, worst 55.6 ->
  29.7 ms. Costs GPU power in menus only; off for light settings (recent play under 3 ms).
- **Aurora 0216**: the present-timing waiter polls with a zero timeout every ~0.25 ms.
- Host thread at high priority; the vblank wait ends with a precise sleep.
- `[interp]` stats: "game N fps (shown M)" = game frames made (shown); "shown X ms behind the
  newest game frame" instead of the old delay figure.

### Results

4K (resolution x4) fullscreen 120 Hz G-SYNC, VSync on, the user's settings (enhanced Ultra etc.)
unless noted, `mixed` scenario, one game instance (GAME_LOCK). "Over" = present intervals above
1.5x the target period (loads and compiles included).

| run | build | presented fps | p99 / max interval ms | over 1.5x |
|---|---|---|---|---|
| user's own 9-minute session, 60 | before (logging only) | 57.47 | 35.9 / 638 | 618 |
| 60, 10 min | before | 55.27 | 41.8 / 76.2 | 2032 |
| 60, 10 min | governor + 0214 | 59.72 | 17.8 / 72.6 | 148 (menu exits) |
| 60, 10 min | + keep-warm | **60.00** | 18.1 / 27.7 | **2** |
| 60 windowed 1920x1080, 10 min | + keep-warm | **60.00** | 18.2 / 37.0 | **2** |
| 40, 4 min | + keep-warm | **39.98** | 26.2 / 62.0 | 5 (loads) |
| 60 vanilla (enhanced/lights/CRT off), 10 min | + keep-warm | **60.00** | 18.0 / 33.2 | **1** |
| 120 vanilla, 4 min | + keep-warm | **116.40** (VRR cap) | 9.6 / 12.3 | **0** |
| 120, 10 min | before | 102.98 | 23.4 / 43.7 | 3459 (3289 chosen by the schedule) |
| 120, 10 min | governor + keep-warm, before 0216 | 110.31 | 13.2 / 37.0 | 1357 |
| 120, 10 min | + 0216 (present timing polled) | 116.04 | 9.6 / 81.4 | 208 |
| **final build** (governor per-interval, keep-warm 90% cap): | | | | |
| 60, 10 min | final | **60.00** | 17.6 / 50.0 | **2** (one 37 ms render-worker stall, not a compile: 0 MISSes) |
| 120, 10 min | final | **116.32** (VRR cap 116.4) | 9.7 / 23.1 | 74 (GPU-bound frames right after a menu closes) |
| 60, 4 min | final (90% keep-warm cap) | **60.00** | 17.6 / 21.3 | **0** |
| 120 windowed 1920x1080, 4 min | 0216 | **116.05** | 10.2 / 32.1 | 88 (same) |
| 90, 4 min | final governor | **90.00** | 11.95 / 16.4 | **0** |
| 60, VRR set Off (vblank mode), 4 min | 0216 | **59.99** | 17.9 / 42.1 | 10 |

The A/B at 60 in one line: 55.27 -> 60.00 fps presented, intervals over 1.5x 2032 -> 2, p99
41.8 -> 17.6 ms. At 120: 102.98 -> 116.32 (the VRR cap), 3459 -> 74, p99 23.4 -> 9.7 ms. The 0216 fix also lowered the
GPU time per frame at 120 (p50 8 -> 5.3 ms): the device lock had been stalling the render worker
and the FIFO thread behind the GPU. No governor slowdown happened in any final run after the boot
(one during the boot's loading screens, recovered within seconds).

Original 30 (no interpolation, the game paced by its VI) at 4K Ultra: 18.5 fps whenever the
present record was on before 0216 (LM_PACING_LOG, and now keep-warm, enable it), 28.7 after in the
foyer (28 with the record off: its own limit), and in the mixed scenario gameplay seconds average
30.0 (p10 29.9, min 28.0) with the menus at 60; vanilla 29.97 flat. What limits it at Ultra in the
foyer is the original-30 path's serialisation: the game's mid-frame GPU syncs (EFB copies read back
by the CPU) make each frame wait for the enhanced passes so far (GPU 17.5 ms per game frame with
the CPU waiting on it). The interpolated targets do not have it (the game runs decoupled); a 30
target through the interpolated path would hold 30, but would no longer be the unchanged original
path.

Remaining: at 120 with enhanced Ultra the first ~5-8 frames after a menu closes cost 15-20 ms of
GPU time (the scene and its enhanced passes return; at 60 they fit the period), and a rare 37-80 ms
gap between frames outside the frame itself (seen 1-4 times per 10 minutes; not a compile).

## Known limitations

- On the test machine presentation never exceeded the display's 60 Hz even with VSync off
  (Aurora's Mailbox or Immediate present mode, D3D12 or Vulkan): `aurora_begin_frame` waits for
  the previous present. That looks like a driver-level limit (e.g. a frame-rate cap or forced
  VSync in the GPU control panel); targets below the refresh rate are hit exactly (45 and 50 fps
  measured). `LM_BACKEND=d3d12|d3d11|vulkan` picks the backend for such tests.
- Things the game snaps itself (Luigi's instant turns, a light switching on) stay snapped.
- Props that appear fade in by dithering (see Polish); anything that vanishes disappears at the
  first in-between frame (pass 4: the redraw from the previous tape is off); characters and blink
  patterns switch at the first in-between frame; particles of a swarm without an unambiguous
  partner move with the camera but their own motion is at the game rate.
- Variable refresh is detected from the display's refresh timing once frames come at uneven
  times (boot, loads); before that, or on a display whose VRR is off, a GPU-limited frame rate
  drops to an even fraction of the refresh rate. F1 > Visuals > Variable refresh rate forces it.
- The vblank monitor (VRR detection, fixed-refresh frame starts) is Windows only
  (D3DKMTWaitForVerticalBlankEvent); elsewhere the schedule is a plain timer grid.
- Original 30 is unchanged by construction (no capture, Aurora 0191 keeps the exact per-draw
  split at 30), but it could not be shown pixel for pixel this time: two runs of the *same*
  executable no longer give identical dumps (lightning timing, particles; 2.4 M px differ over 61
  nursery frames, the same as between main's and this branch's executables, 2.2 M px).
- Presentation rate depends on the replay cost (FIFO drain): heavy rooms with expensive accuracy
  options may still present below the target on slower CPUs; the pacing then keeps motion even at
  the reached rate (no stalls) instead of juddering.
- 2D/HUD interpolation is verified for the GBH, health, element meter, room name, E. Gadd's call
  and its message windows, the pause screen and the ending caption; GBH map/inventory pages
  beyond the pause screen and the PAL GBH screens are untested.
- Shadow depth (near/far) changes at the game frame, not in between (by design, see Polish 3).
- The `back-and-forth` metric also counts fine texture detail sliding under a moving camera, so
  it never reaches 0 in textured scenes.
- (Fixed) Replaying a tape used to start from the GX state the previous replay left; each
  replay now starts from the end state of the tape before it (Aurora patch 0021 replay state
  slots, `replayPair`; from the SMS port).
