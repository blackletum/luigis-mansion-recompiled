# PC enhancements in the recompiled build

The recompiled build (`lmrecomp`, see [RECOMPILER.md](RECOMPILER.md)) runs the retail game
code unchanged. Enhancements therefore live in the host layer: the GX HLE
(`src/recomp/hle_gx.cpp`), the frame handoff (`src/recomp/frame.cpp`), the VI emulation
(`src/recomp/hle_vi.cpp`), and hooks on individual recompiled functions
(`tools/recomp/hooks.txt` plus `HLE(name)`, with `callOriginal(c, name)` to wrap the
retail body instead of replacing it).

| Feature | Status |
|---------|--------|
| Arbitrary resolution | Works. Aurora renders at window size; EFB copies scale with it. |
| Widescreen / ultrawide | Works: Hor+ 3D, 4:3 or edge-anchored HUD, culling widened. Limitations below. |
| Field of view | Works (F1 > Visuals > Field of view); culling follows. |
| High frame rate (interpolation) | Works: 60 fps or display rate, game logic stays 30 Hz (section 3). |

## 1. Arbitrary resolution

The game always renders a 640x480 logical frame. Aurora maps it onto its content
framebuffer, which matches the window size:

- `GXSetViewport` and `GXSetScissor` take logical coordinates. Aurora multiplies them by
  `render size / 640x480` (`map_logical_viewport` and `map_logical_scissor` in
  `extern/aurora/lib/gx/gx.cpp`).
- `GXSetTexCopySrc` goes through the same scissor mapping. The copy destination is
  scaled the same way (`scale_copy_dst` in `lib/dolphin/gx/GXFrameBuffer.cpp`). An EFB
  copy therefore has native resolution, and anything that samples it with normalized
  texture coordinates stays correct. This covers the full-screen copies that the
  foyer, flashlight and ghost effects make each frame (`copy src (0,0 640x480)` in the
  trace), and the 128x128 render-to-texture in `fn_8006033C`.
- Under the widescreen STRETCH policy the scale factors differ in x and y. A 128x128
  logical copy at 1280x720 becomes a 256x192 texture. Its content is still correct,
  because the viewport it was drawn with was stretched by the same factors. It only has
  more horizontal than vertical resolution.

Nothing in the port assumes 640x480 pixels. Captures at 960x720, 1280x720, 1680x720,
1707x960 and 1920x822 all render correctly.

## 2. Widescreen and ultrawide

### Options

| Option | Meaning |
|--------|---------|
| `--aspect auto` (default) | Follow the window shape. A 4:3 window behaves exactly like the original. |
| `--aspect 4:3` | Original presentation, pillarboxed by Aurora in a wider window (FIT policy). |
| `--aspect 16:9`, `21:9`, `<w>:<h>`, `<ratio>` | Fixed aspect. The window opens at that shape (960 px tall). |
| `--window <w>x<h>`, `--fullscreen` | Initial window size, or fullscreen. |
| `--ws-backgrounds pillarbox\|stretch\|zoom` | Full-screen 4:3 pictures (title backdrop, file-select clouds): keep them 4:3 with bars (default), stretch them, or enlarge them to the full width and crop the top and bottom. |
| `LM_WS_TRACE=1` (env) | Logs each frame's widescreen decisions whenever they change: draw path, projection type, viewport, result, plus EFB copies. |
| `LM_CACHE_DIR=<dir>` (env) | Uses a separate shader/pipeline cache. Useful when several builds share one machine. |

### How it works (`src/recomp/widescreen.*`, `src/recomp/hle_gx.cpp`)

`s = (4/3) / display aspect` (`ws::factor()`, below 1 when the display is wider). It is
read on the host thread before each guest frame, so window resizes take effect on the
next frame.

1. **Stretch everything first.** `AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH)` maps
   the logical frame onto the whole window.
2. **Track the guest's projection, viewport and scissor; send the projection lazily.**
   `GXSetProjection` only records the matrix. Before each draw, `prepareDraw()` sends one
   of three variants:
   - *plain*: unchanged, so the draw is stretched to the window.
   - *scaled*: row 0 of the projection multiplied by `s`, so `x_ndc' = s * x_ndc`. For a
     perspective projection this is **Hor+**: the vertical FOV is kept and the
     horizontal FOV grows. For an orthographic projection it keeps 640x480 2D at **4:3,
     centered**. A partial-width scissor is moved by the same transform.
   - *zoom*: row 1 divided by `s`, for the `zoom` background mode.

   Draws reach Aurora through `GXBegin`, `GXCallDisplayList` and `GXDraw*`, and also
   through raw FIFO writes from inlined game code. J3D inlines its display-list calls
   and matrix loads. Before the first raw FIFO write after a projection or viewport
   change, `hw::fifoWrite` brings the projection up to date (`sRawCheck`).
3. **Choose per draw.**
   - Draws into a viewport that does not cover the logical width (render-to-texture)
     are *plain*. They then stay consistent with the EFB copy that reads them back.
     Partial viewports set by a HUD element (picture-in-picture: the GBH views, see
     *HUD anchoring*) are moved and narrowed like the 2D around them instead.
   - Perspective draws into the full-width viewport are *scaled* (Hor+).
   - Orthographic immediate-mode draws into the full-width viewport are **held**:
     `GXBegin` and the FIFO writes that follow are buffered until the draw ends. Then
     `coversScreen()` decodes the vertex positions from the mirrored vertex descriptor,
     VAT, position array and position matrices, and projects them to NDC x.
     - A draw that spans the whole width and samples no texture, or only an EFB copy, is
       a fade or a screen-space effect. It stays *plain* so it lines up with the
       stretched EFB.
     - A draw that spans the whole width and samples an ordinary texture is a 4:3
       background picture. It follows `--ws-backgrounds`.
     - Everything else is HUD or text: *scaled*, so 4:3 and centered.
   - Orthographic display lists are *scaled*.
4. **Widen culling.** (Both releases; the European camera's viewport fields are at
   +0x1F8/+0x200.) `fn_80008D58(camera, top, bottom, left, right, near, far)` builds
   the camera's four side culling planes. Every 3D projection setter ends with it
   (`fn_80008654`, `fn_800087D0`, `fn_80008928`). The hook in `widescreen.cpp`
   multiplies `left` and `right` by `1/s` when the camera viewport is full-width, then
   calls the retail body. Without this, objects at the screen edges pop in.

Overlays stay in place. Game code that projects a 3D position to the screen (for
markers over ghosts and similar) uses the 4:3 projection and gets a 640-space x.
Drawing that in the centered 4:3 2D space gives `s * x_ndc`, which is exactly where the
Hor+ projection put the object.

### Field of view

`settings().video.fovScale` multiplies the vertical FOV of full-screen perspective
projections. `ws::fovRatio(m11)` turns it into a scale for rows 0 and 1 of the projection
(`cot(f/2) / cot(k*f/2)`), applied together with Hor+ in the *scaled* variant, so a 4:3
window with FOV 1.0 is still untouched. The `fn_80008D58` hook scales all four culling
sides by the inverse, using the camera's `fovY` (`+0x90`).

### HUD anchoring

With "Keep HUD at 4:3" off, the in-game HUD moves to the display edges instead of the 4:3
safe area. Anchoring works per HUD *element*, not per draw, so a box and its text, or the
Game Boy Horror and everything on its screen, always move as one unit
(`ws::AnchorScope`, `src/recomp/widescreen.cpp`).

The in-game HUD is drawn once per frame by `fn_8003E3D0`, which calls one function per
element. The element functions (and a few related ones) are hooked; each runs the original
inside an anchor scope:

| Hook | Element | Anchor |
|---|---|---|
| `fn_8003D634` | Game Boy Horror: frame, LEDs, its screen's J2DScreen (money tally, E. Gadd's call, E3 clock) | right |
| `fn_8003BE9C` | static noise over the GBH screen (from the HUD and from the GBH view pass) | right |
| `fn_800417F8` | health meter | left |
| `fn_80041214` (inside it) | health shown over Luigi at his projected position (training room) | centered |
| `fn_80045BDC` | room name: floor (top right) and name (top left) | each draw to its own side |
| `fn_800115F8` | frame-loop view setup; view 1 = GBH live camera view | view 1 right, others centered |
| `fn_80114E18` | event portrait renderer (`<PDEMO>`); E. Gadd's call portrait on the GBH screen | right during the call (GBH mode `lbl_804D846C` = 3), else centered |

Everything else stays centered as one unit: the element meter, dialogue/message windows
(`fn_80044138`), menus, the GBH map and item screens, the GBH first-person (X) view, markers
placed at projected 3D positions.

Inside a scope the GX HLE gives every orthographic draw the same anchor: the centered 4:3
projection is shifted by `±(1 - s)` (a held draw keeps the anchor of its `GXBegin`, since it
is only sent at the next GX call). A partial scissor moves with it.

**Picture-in-picture 3D.** A viewport smaller than the screen that is set inside a scope is
sent to Aurora moved and narrowed exactly like the 2D around it
(`x' = 320 + (x - 320)s + a(1 - s)320`, `w' = ws`), and every scissor set while that viewport
is current (the room portals drawn into it) gets the same transform. The projection is left
alone, so the image keeps the aspect of the game's rectangle. This applies with the HUD kept
at 4:3 too (anchor 0), where it keeps the 3D inside the centered frame. Covered: the GBH live
camera (view 1, 110x82), E. Gadd's call portrait (110x84), the small main view inside the GBH
frame in first-person/scan mode (`r13+0x200`, 416x312). None of these is read back through an
EFB copy; the render-to-texture views the game does copy (e.g. 140x200 at 250,140, copied to
70x100) are not set inside a scope and stay *plain*.

Test aid: `LM_HUD_TRACE=<first>,<last>[,<step>]` logs, for those frames (GXCopyDisp count),
every 2D draw with its NDC x extent and anchor, every viewport/scissor/texture copy, with the
guest call chain, which is how the element functions were identified.

An earlier per-draw rule (a 2D draw in the outer 30% of the frame moves to that edge) split
dialogue boxes from their text and treated the GBH map screen as HUD; an even earlier
per-pane approach split menu layouts. Both were dropped.

### Scenes fixed for widescreen

Places found in play that looked wrong at 16:9/21:9 and right at 4:3:

| Scene | Cause | Fix |
|-------|-------|-----|
| Portrait gallery (map 6): a black slab beside the pillars while walking, and at 21:9 whole rooms missing at the sides | Portal culling. `fn_8001333C` computes each doorway's screen rectangle with the 4:3 projection*view matrix; the room behind it is then drawn with `GXSetScissor(rect)` (`fn_8001798C`), which the GX HLE passes on unchanged (stretched to the window) while the room itself is drawn Hor+. The scissor sat further out than the doorway, cutting off the inner part of the room behind it, and rooms visible only in the widened sides got an empty rectangle | `portal_fix.cpp`: while `fn_8001333C` runs, rows 0/1 of that matrix (`lbl_803A3C98 + 0x258`, read by nothing else) are scaled like the projection (`factor() * fovRatio`, `fovRatio`), so rectangles are in the drawn frame and the stretched scissor matches. Generic: every map's portals, also with the FOV slider. Not tied to the portal near-plane setting (docs/RENDERING.md) |
| Area-clear / result screens after the Portrificationizer ("Now you've caught: (Area n)" with Luigi's flashlight on a yellow wall, "You've caught a total of ...", "You've found:"; event 02's `<ACCOUNT>` in E. Gadd's lab, map 4, drawn by `fn_80050524`): a vertical strip of the yellow wall beside the black surround on the right, the lab beside the iris wipe that opens the sequence, and the lab on the left during the fade to the list | All 2D in the 640-wide frame (Luigi is 3D on top). `fn_80050524` draws, by state (account object `+1`): state 0 an iris wipe exactly like the ending's (four `J2DFillBox` boxes + `/timg/ad_wipe_0` circle); states 3..7 a black fill and three J2DScreens from `lbl_803CFA50`: `+8` the wall (a 1280-wide tiled picture the game scrolls, so it runs past the 4:3 frame's right edge), `+0` the black mask (a tiled black picture over x 0..240 and the spotlight texture, 180x210 I4, in two halves over 240..642) with the text, `+4` the money list behind a fading fill. The mask only covered the 4:3 frame | `HLE(fn_80050524)`: state 0 runs in a `ws::WipeScope` (as the ending's wipe). Inside the mask screen (`J2DScreen::draw` hook, screen = `lbl_803CFA50[0]`) a `J2DPicture::drawTexCoord` hook lengthens every quad whose screen-space side lies on the 4:3 frame's left or right edge (or beyond) to the display's edge, extrapolating its texture coordinates along the quad: the tiled black panel keeps tiling, the clamped spotlight texture repeats its black edge texels. The black graphic itself now reaches the edges at any aspect; the spotlight, the text and Luigi stay where they are in 4:3. An earlier attempt (`ws::Lock43Scope` on `fn_8018721C`) targeted the wrong screen and is removed |
| Gallery plaque screen (map 6, event 79 `<GALLERY>` with the plaque's `event_parameter`, game mode 6, `fn_8018712C` setup / `fn_8018721C` draw: a portrait frame on a stone wall, 3D lit by a spot light) | Hor+ like the rest of the game (no special handling; the 4:3 scissor it had before is removed). At 16:9 the wall fills the screen; at 21:9 and wider the wall model ends at about 16:9 and the clear colour (black) shows beyond it, a clean vertical edge | none needed |
| Door cutscenes (Luigi's hand on the knob, `/Ajioka/ADemo/{oo,co,ho,no}demoNN`): a dark shape floating above-left of the knob (the knob's reflection of the hand, drawn in the wrong place), and the knob's reflection texture partly cleared | Event scenes are drawn by `fn_80114E18`, which runs in an `AnchorScope` (its portraits sit in HUD viewports, e.g. on the GBH screen during E. Gadd's call). Inside it, `fn_801142C4` renders the scene's texture target: viewport (0,0,128,128) at the EFB origin (`fn_8011548C`), a clearing copy of that rectangle, the scene's models from the reflection camera, then `GXCopyTex` of (0,0,128,128) (`fn_801168A8`). In the scope the viewport "followed" the centered HUD (moved to x = 80 and narrowed at 16:9) while both copies kept the game's rectangle, so the pass stayed on screen right of the cleared 128 px (its depth hiding the door there) and the knob sampled a half-empty texture. Not interpolation, FOV or resolution related (same at 30 fps and scale 1) | `HLE(fn_801142C4)` runs in a `ws::RenderToTextureScope`, in which viewports never follow a HUD scope: the render-to-texture viewport stays where the game copies it from (stretched like any render-to-texture viewport, so the copy matches at every aspect and resolution). 4:3 is unaffected (no scope logic without widescreen) |
| Ending iris wipe on Mario (`fn_80055C54`, end of event 52): only the 4:3 frame was masked | The mask is a circle picture (`/ad_wipe_0.bti`) plus four `J2DFillBox` boxes reaching x = 0 and x = 640. Fills spanning the width were stretched, the side boxes were centered 4:3, so the scene showed beside them | `fn_80055C54` runs in a `ws::WipeScope`: every 2D draw in it is centered 4:3 (`classifyHeld`), and a `J2DFillBox` hook extends boxes touching the frame's left/right edge (or beyond it, negative widths once the circle outgrows the screen) to the display's edge. The circle stays round and centered |

Verified with `LM_FRAME_DUMP` (16:9 and 21:9 against the unfixed build at the same frames, map
6 walking left/right with `LM_START_MAP=6`, plaque 7 with `LM_TELEPORT=2025,0,-1750,20` +
STICKLEFT + A, the iris with `LM_START_MAP=12` + A presses through event 52, frames
2600-2760). 4:3 runs are pixel-identical to the unfixed build (the hall differs only where the
vacuum dust differs between two runs of the same build). Not checked on the European base (the
hooks are tagged `us pal`). The 21:9 view of the gallery hall can still see past the end of a
room's outer wall at the far right (nothing is modelled there; the same with the fix off).

Area-clear screens: reached with `LM_INPUT_CLOCK=frames LM_START_MAP=4 LM_SKIP_TEST=2300:flag:66`
(flag 66 makes event 02 call `<ACCOUNT>` on arrival in the lab), the scenarios' menu input and
`A*4` every 40 frames from 2700; wipe ~2860-2920, "Now you've caught" ~2940-3020, fade to the
list ~3040, list 3060-3100, save prompt 3120. `LM_FRAME_DUMP` 2860-3140 every 20 at 16:9, 21:9
and 32:9: nothing outside the 4:3 frame but the extended black mask (and the wipe's circle where
it is wider than the frame); 4:3 byte-identical to the unfixed build, as is the plaque screen
(plaque 7, frames 2700-2950). `LM_HUD_TRACE` lines now name each held 2D draw's texture
(address, size, format), which is how the mask's pieces were identified. Test aid
`LM_GALLERY_PARAM=<n>` (dev_tools.cpp) opens gallery screen n from any plaque. European base
not run (hooks tagged `us pal`, mapped by gen_version_map.py; the state byte `+1` matches there).

Door knob: the nursery's locked door (`LM_INPUT_CLOCK=frames LM_START_MAP=2 LM_START_APPEAR=1
LM_TEST_STATE=poltergust`, the `nursery` scenario's input up to `2900:STICKUP*40` from
tools/interp/scenarios.sh, then `2945:A*3,2975:A*3,3005:A*3`; the knob close-up runs from
frame ~3010). `LM_HUD_TRACE=3038,3038` shows the 128x128 viewport marked `follow` before the
fix. Dumps 3012-3100: at 16:9 the floating shape is gone (the only changed pixels, apart from
the knob's faint reflection, which now matches the 4:3 frame more closely), in every fresh and
interpolated frame at 60 fps with Enhanced lighting Ultra and render scale 4 too; 4:3 at 30 fps
is byte-identical to the unfixed build (frames 2940-3140).

### Screenshots

Captured with PrintWindow, so other windows on screen cannot leak into them. Files are
under `build/` and are not committed:

- Title: `title43_14.png` (960x720) vs `title169_14.png` (1280x720, backdrop pillarboxed,
  logo centered) vs `title219zoom_14.png` (1680x720, `--ws-backgrounds zoom`).
- File select: `final43_16.png`, `final169_16.png`, `final219_16.png`.
- Gameplay (`--input "600:START*10,1000:A*6"`):
  - `pw43_sheet.png` vs `pw169_sheet.png`: 4:3 and 16:9 at the same times. Covers the
    intro, the mansion exterior, and the foyer with the HUD.
  - `post_sheet.png` and `game219_*.png`: 21:9.

### Known limitations and next steps

- **HUD anchoring.** HUD elements not in the table above are centered. A new element
  that should sit at an edge needs its draw function hooked. The J2DScreen at
  `lbl_803CE2D8+0xE4` (drawn last by `fn_8003E3D0`, a 24 px piece at the right edge) showed
  nothing in the tested scenes and is left centered.
- **Partial EFB copies computed from 3D positions.** No such copy has been seen so far.
  If one turns up (for example a copy of a rectangle around an object), its source rect
  is in stretched logical space, while the object was drawn Hor+. Fix it by moving the
  copy rect through the `s` transform in `GXSetTexCopySrc` whenever the last full-width
  perspective draw was *scaled*.
- **Embedded 3D in UI.** Picture-in-picture viewports follow their HUD element only when
  set inside an anchor scope. One set elsewhere would still be stretched (none known). If a
  scoped view is ever copied with `GXCopyTex`, its copy rect must get the same transform.
- **Classification relies on the mirrored vertex state.** If a display list changes the
  vertex descriptor or VAT before an immediate 2D draw, `coversScreen()` gives up when
  the computed stride does not match, and the draw is treated as HUD. That fallback is
  safe.
- **Other culling.** Room portal visibility follows the Hor+ frame ("Scenes fixed for
  widescreen"). Per-actor distance culling has not been audited. Nothing popped in the foyer,
  the exterior or the gallery at 21:9.

## 3. High frame rate: frame interpolation

### As built (`src/recomp/interp.*`, `src/recomp/frame.cpp`, Aurora patch 0005)

Current state (frame-rate slider, vertex-array copies, pairing by draw, rotation blending,
lights/texture matrices/CPU-built vertices, test mode and measurements): **docs/INTERPOLATION.md**.
The notes below describe the first version; where they differ, INTERPOLATION.md is current.

The setting is F1 > Visuals > Frame rate: 30 fps (lockstep, unchanged) up to 320 fps, or the
display's refresh rate; above 30 the design below is used.

**Capture instead of snapshotting.**
- Aurora patch `0005-capture-and-replay` makes the FIFO's display-list redirection
  per-thread, and adds `AuroraCaptureBegin/End/Pos` and `AuroraReplay`.
- While interpolating, the guest thread's GX output goes into a capture buffer, which
  becomes that frame's tape. A tape is self-contained: Aurora already serializes texture
  objects by value, and `GXCallDisplayList` inlines display lists. GXDrawDone and
  AuroraGXSync don't wait on the capturing thread.
- The host submits a tape with `AuroraReplay` inside its own frame and never touches the
  guest's `__gx` state.
- A game-recorded display list (`GXBeginDisplayList`) suspends the capture around itself.

**Decoupled guest.**
- `frame::hostFrame()` replaces `runUntilPresent()` in the host loop.
- Once interpolation is on, after the first 60 presents, the guest stops waiting for the
  host in `present()`. Each present closes a tape (`interp::endTape`, stamped with the VI
  retrace count) and opens the next one.
- The guest then runs exactly as on the console, paced only by the VI retrace timer, so
  game logic, audio and VI timing do not depend on the presentation rate.
- Switching back to Original hands control back at the next guest present: the host
  replays that last tape as is, then resumes lockstep.
- While the window is paused, `frame::setHostPaused()` stops the VI retraces, so a
  decoupled guest holds as it would in lockstep.

**What gets blended.**
- Measured before building: every matrix load in a gameplay frame comes through
  `GXLoadPosMtxImm`, about 150 to 450 loads per frame. There were no raw XF writes and
  no indexed loads.
- The GX HLE notes the tape offset of each 3D matrix load (pos, normal, texture) and of
  each perspective projection. The key is `(source address, slot, occurrence)`, which
  matched about 100% across frames in the foyer and the intro.
- The host replays the newest tape. Each noted block is blended toward the previous
  tape's value at the same key: `alpha = (now - tape done) / (game frame interval)`,
  which puts the display one game frame behind real time.
- 2D (anything under an orthographic projection) is not blended. Its matrices come from
  per-glyph scratch matrices that don't line up between frames, and blending them made
  file-select text and HUD pieces fly.
- **Cuts and jumps.** A matrix is shown unblended when it moves more than 400 units or
  0.35 in rotation in one game frame. Texture matrices use a relative bound, because
  scrolling coordinates wrap. Camera cuts therefore snap rather than smear.

**Measured** (1280x720, 120 Hz display, vsync on, `LM_INTERP_TRACE=1` logs alpha and
frame time per present):

| Mode | Presented | Game | Pacing |
|------|-----------|------|--------|
| Interpolated 60 | 59.9 to 60.0 fps | 29.6 to 30.0 fps | present interval 16.7 ± 1 ms, alpha alternating about 0.4/0.9, so displayed poses are evenly spaced |
| Unlimited | 120 fps (the display rate under vsync) | 30 fps | |

Typical figures per present:
- Tape size: 0.7 to 1.1 MiB per game frame.
- Matrices: about 1,000 to 2,000 blended per present; about 1 pos/nrm snapped.
- Texture matrices: about 350 are always snapped. These are projection-style matrices
  that the game alternates with zero matrices between frames, so blending them would be
  wrong.

Runtime toggling between Original and Interpolated every 3 s for 60 s ran cleanly.
Render resolution 2x (`resolution_scale=2`) together with widescreen, HUD anchoring and
unlimited mode also worked: the EFB copies are replayed with the rest of the tape, so
copy-based effects (the foyer window, the flashlight, fades) follow the interpolated
frame.

**Limitations.**
- **Replay starting state.** Replaying a tape twice starts from the GX state left at
  the end of that tape, not from its own start. That only matters if a frame relies on
  state from the previous frame without setting it again. Nothing has shown up.
- **Not blended.** Lights loaded with `GXLoadLightObjImm` (view-space positions),
  immediate-mode vertex positions (particles, the flashlight cone mesh if it is built on
  the CPU) and texture-projection matrices step at 30 Hz.
- **Guest memory referenced at replay time.** Vertex arrays and textures are read from
  guest memory when the tape is replayed, up to one game frame after capture. This is
  the same lag the console's GPU has, and games double-buffer for it. Data that the CPU
  rewrites within a frame could still show up in the next replay.
- **Pacing.** Pacing is driven by the host clock. The guest's completion jitters by a
  few ms, so alpha is computed from the retrace stamps and the wall time of completion.

### Original design notes

### Constraints in this port

- The game simulates and renders at 30 Hz. It paces itself on the emulated 59.94 Hz VI
  retrace (`hle_vi.cpp`), and `VISetNextFrameBuffer` marks a finished frame
  (`frame::present`).
- **Lockstep today.** The guest only runs inside `frame::runUntilPresent()`, between
  `aurora_begin_frame()` and `aurora_end_frame()`, and calls Aurora's GX directly from
  the HLE. While the guest sleeps on retraces inside a frame, the host is blocked, so
  presentation is capped at the game's rate.
- **No source changes.** Unlike Dusklight (`src/dusk/interp`), we cannot split game
  logic and drawing or add record calls in game code. Everything has to come from
  observing the GX stream and from hooks on recompiled functions.

### Architecture: record the frame as a GX "tape", replay it with interpolated matrices

**Stage 1: decouple guest and host (no visual change).**

1. The HLE stops calling Aurora directly and appends to a per-frame tape instead. Every
   SDK GX call already goes through one choke point, the `GXHLE` macro in `hle_gx.cpp`;
   each records an op with its decoded arguments. Raw FIFO writes (`hw::fifoWrite`) are
   recorded as byte runs.
2. `frame::present` closes tape N and hands it to the host through a two-slot queue.
   The guest continues immediately and keeps being paced by the VI timer, not by
   `aurora_begin_frame`.
3. The host loop (`src/recomp/main.cpp`) runs at display rate. Each frame it executes
   the newest tape into Aurora: the existing widescreen logic, then Aurora's GX.
4. **Guest memory lifetime.** A tape refers to guest memory by pointer: display lists,
   vertex and matrix arrays, textures, CI palettes. Once the guest runs ahead, it may
   overwrite that memory while the host still replays the tape. Snapshot only what can
   change:
   - **Immediate vertex data**: already inline in the tape.
   - **Vertex arrays (`GXSetArray`)**: copy the range the frame's draws actually use.
     Aurora patch 0002 already computes that range per draw from the indices. Expose it
     and snapshot at record time. Typical frames reference well under 5 MiB, Aurora's
     own budget.
   - **Matrix arrays**: `GX_POS_MTX_ARRAY`, `GX_NRM_MTX_ARRAY`, `GX_TEX_MTX_ARRAY`.
     J3D loads matrices indexed from these (`J3DShapeMtx::loadMtxIndx_*`, 0x801B79FC to
     0x801B7B5B, CP commands 0x20/0x28/0x30 in the raw stream). Snapshot each referenced
     element.
   - **Display lists**: static model DLs never change, so keep them by pointer. DLs
     built at run time (`GXBeginDisplayList`/`GXEndDisplayList`) are copied.
   - **Textures**: textures loaded from disc are static. CPU-written textures are rare.
     Hash them at `GXInitTexObj` to detect them. EFB copies already live on the host
     inside Aurora.
5. **Fallback.** If a frame's snapshot would be too large, or the tape is incomplete,
   run it the lockstep way.

**Stage 2: interpolate.** Keep tapes N-1 and N. Presenting at time `t` between their
timestamps (`alpha` from the retrace counter, `period = 2 retraces`), replay tape N with
these substitutions:

- **Matrices.**
  - Pos and normal matrices from `GXLoadPosMtxImm`, `GXLoadNrmMtxImm` and
    `GXLoadTexMtxImm`: key = `(guest source address, slot, occurrence #)`.
  - Elements of the matrix arrays: key = `(array role, element address)`.
  - J3D double-buffers its draw matrices (`J3DMtxBuffer`), so element addresses
    alternate between frames. Normalize to `(base of the buffer pair, offset)`. Or key
    J3D draws by `(DL pointer, matrix-index slot)`, because a shape's DL and its
    `loadMtxIndx` slot are stable.
  - Interpolate 3x4 model-view matrices as translation lerp, rotation quaternion slerp
    and scale lerp. A component-wise lerp is acceptable for small deltas.
  - A key that is missing in N-1 means a new object: use N unchanged.
- **Projection.** Lerp the elements, then apply the widescreen transform.
- **Camera cuts.** Hook `fn_80008654` (the game camera apply, camera struct at
  `lbl_803A3820`: eye `+0x0`, target `+0xC`, fovY `+0x90`, view matrix `+0x18C`) with
  `callOriginal`. Record eye, target and fov each sim frame. If they jump past a
  threshold, or the scene changes (the scene machine's change request), present N
  without interpolation for that frame.
- **Immediate-mode geometry** (particles, the flashlight cone, ghost trails): match
  draws by ordinal within runs of identical state (same texture, TEV and vertex
  format). If the vertex counts match, lerp positions; otherwise draw N's vertices.
  Dusklight's `interp/vertex.cpp` and `particle.cpp` do the same at source level.
- **2D (orthographic) draws.** Not interpolated: N is replayed as is. HUD motion is
  rare and steps at 30 Hz.
- **EFB copies.** Replay the copies too (`GXCopyTex` ops). The copy then captures the
  interpolated frame, and the blur, ghost and flashlight effects stay consistent.
  Effects that accumulate over frames (feedback blurs) will run at the display rate.
  Detect copies whose destination is read before it is written again in the same tape,
  and skip those on replayed frames.
- **Latency.** One sim frame (33 ms) of added latency, the same as Dusklight. Option:
  extrapolate the camera only.

**Stage 3: refinements.**

- Tie `begin_frame` pacing to the display (VRR friendly).
- Expose `--fps <n|unlimited>` and `--interp off|camera|full`.
- Optional source-level hook on `J3DModel::viewCalc` (0x801C79F0): rerun view
  calculation with an interpolated camera view matrix. This is cheaper than per-matrix
  matching when only the camera moves.

### Hook points summary

| Purpose | Where |
|---------|-------|
| Frame boundary / tape close | `VISetNextFrameBuffer` HLE -> `frame::present` (`hle_vi.cpp`, `frame.cpp`) |
| Record SDK GX calls | `GXHLE` macro (`hle_gx.cpp`) |
| Record raw FIFO (J3D inlines) | `hw::fifoWrite` (`hle_gx.cpp`) |
| Matrix identities | `GXLoadPosMtxImm`/`GXLoadNrmMtxImm`/`GXLoadTexMtxImm` HLE (source address), `GXSetArray` for matrix arrays, `J3DShapeMtx::loadMtxIndx_*` (0x801B79FC...) |
| Projection | `GXSetProjection` HLE (already deferred for widescreen) |
| Camera state / cut detection | hook `fn_80008654` (`lbl_803A3820`); optionally `J3DModel::viewCalc` |
| Sim pacing | VI timer thread and `VIWaitForRetrace` (`hle_vi.cpp`) |
| Present loop | host loop in `src/recomp/main.cpp` |

### Why not run the draw code twice?

Running the game's draw phase again for in-between frames would need the calc and draw
functions split at a stable point per scene. That needs per-scene knowledge of LM's
scene machine. Draw code also has side effects: particle emitters advance during draw,
J2D animations step, and `GXSetDrawDone` / `VIWaitForRetrace` pacing is mixed in.

The tape design only depends on the GX interface, which the HLE layer already owns. It
also keeps the guest's simulation bit-identical to the console.

## 4. Skippable cutscenes and fast-forward

Options (F1 > Enhancements): **Skippable cutscenes** (off by default) and **Hold Tab to
fast-forward** (on). Code: `src/recomp/skip.{h,cpp}` plus small hooks in `frame.cpp`,
`hle_vi.cpp`, `hle_gx.cpp`, `hle_pad.cpp` and `audio_out.cpp`.

### Approach: run the cutscene, just faster

A skip does not jump over game code. The game keeps executing every logic frame of the
cutscene, so whatever the event script does (flags, items, keys, doors, warps, actor
positions, BGM changes, room loads) happens exactly as when watching. While skipping:

- the VI retrace timer runs up to 32x faster (one retrace in flight at a time, so a
  slow frame never queues a burst of retraces);
- guest frames are batched: `frame::present` keeps the guest running for ~12 ms of host
  time per Aurora frame; only the first frame of a batch is drawn, the others send no
  draws or EFB copies to Aurora (`gx::g_discardDraws`; state calls and display-list
  recording still go through, so nothing is lost for later frames);
- the screen is covered (black, "Skipping...") and audio is muted, then faded back in;
- controller 1 is replaced: the player's input is ignored and A is pressed 4 frames out
  of 8 while the script waits in `<ANYKEY>`, which dismisses message windows. The pad
  reads as connected during the skip (the game ignores a disconnected pad's buttons).

Measured: the opening (about 1,450 logic frames, 48 s) skips in 1.6 to 3 s; the mansion
entrance event (414 frames, 14 s) in 1.6 to 4 s; E. Gadd's Game Boy Horror call with four
message windows in about 2 s.

### When a cutscene can be skipped

Read from guest memory every frame (addresses in `skip.cpp`; USA addresses, mapped to the
European game's with `VA()`, docs/SETUP.md: same layout, the European scene table only adds a
ninth scene):

- the **opening** scene (`sCurScene` = OPENING) when it leads into a new game. The title
  screen's attract demo uses the same scene and is left alone (START already exits it);
- an **event script** is running (interpreter object `0x803D2D40`, cursor at +0x0C) and
  it has taken control away: HUD hidden by `<MENUOFF>` or Luigi stopped by `<LUIGISTOP>`.

The skip ends when the scene changes (opening), the event script ends, or the player
has had control for 3 frames (an event that frees Luigi and keeps running in the
background, like the training room). START is hidden from the game while a cutscene
can be skipped.

The player still decides: a `<CHOICE>` menu (interpreter state 7) or the save screen
(`<SAVE>`/`<HSAVE>`) suspends the skip with the game visible and the controller live;
it resumes after the answer. If the script does not advance for 1,800 frames outside a
`<WAIT>`, the skip assumes it is waiting for the player and gives up for that event.

"Press START to skip" shows for 4 s at the bottom right when a cutscene starts.

### Verification

`LM_SKIP_LOG=1` logs every change of scene/event state and, 60 frames after each event
ends, a snapshot (all 256 event flags, Luigi's position and flags). Skipped and watched
runs of the same scripted input (`LM_INPUT_CLOCK=frames`, `--input`) produce identical
snapshots for the opening + entrance event, the GBH call event (with its save prompt)
and the library book event (with its yes/no choice). Testing aids: `LM_SKIP_CUTSCENES=1`
(force the option on), `LM_SKIP_AUTO=<n>[:<count>]` (press START n frames into each
cutscene), `LM_SKIP_TEST=<frame>:flag:<n>` (set an event flag: flag-triggered events
from `jmp/eventinfo` start by themselves) or `<frame>:pos:<x>,<y>,<z>`, `LM_FF=1`
(fast-forward everything), `LM_FF_SPEED=<x>`, `LM_SKIP_LOG=2` (snapshot every 30
frames). Use `LM_CARD_DIR` for test runs: events save the game.

### Limitations

- Only the opening and event scripts are covered. Sequences played by game code rather
  than an event script (if any: e.g. room-clear light/chest camera, boss defeat
  animations, the ending and credits) are not skippable; Tab fast-forward works on them.
- Timing that depends on wall-clock time (disc reads, audio) does not speed up; the
  number of frames a loading pause lasts can therefore differ from a watched run.
- Music keeps playing at normal speed (muted) during a skip, so after a skip a track
  may be at a different position than after watching.
