#pragma once
// Widescreen presentation for the recompiled game (docs/ENHANCEMENTS.md).
//
// The game renders a 640x480 logical frame. Aurora's STRETCH viewport policy maps
// that frame onto the whole window, so everything is drawn stretched; the GX HLE
// then undoes the stretch per draw:
//   - 3D (perspective projection, full-screen viewport): Hor+. The projection's
//     x row is scaled by factor() so the vertical FOV is kept and the horizontal
//     FOV grows. The game's frustum culling is widened to match (hook on
//     fn_80008D58).
//   - 2D (orthographic projection, full-screen viewport): kept at 4:3 and centered
//     ("pillarboxed" inside the wide frame), except draws that cover the whole
//     logical screen width: fills (fades) and blits of EFB copies stay
//     stretched to the window so they line up with the stretched frame; 4:3
//     background pictures follow backgroundMode().
//   - Draws into a partial viewport (render-to-texture) are left alone, so EFB
//     copies of those regions keep matching what was drawn. Partial viewports set
//     by a HUD element (picture-in-picture, see AnchorScope) follow its 2D instead.
#include "recomp/ctx.h"

namespace recomp::ws {

void init();          // host thread, before the first frame: viewport policy from --aspect
void beginFrame();    // host thread, before the guest runs a frame: picks up window resizes

// (4:3) / display aspect: 1 when the display is 4:3 or narrower or the mode is
// off, < 1 when wider. Constant while the guest runs a frame.
float factor();
inline bool active() { return factor() < 0.9999f; }

// Field of view (settings().video.fovScale): the factor by which a perspective
// projection with this m[1][1] (= cot(fovY/2)) is scaled in x and y so its
// vertical FOV becomes fovY * fovScale. 1 when the scale is 1.
float fovRatio(float m11);
// True if full-screen 3D projections need adjusting (widescreen or FOV).
bool adjusting();

// Guest thread: the game's shadow light camera is being set up (fn_8005D984, shadows.cpp).
// Its culling planes are the light's frustum, not the view's, so fn_80008D58 leaves them alone.
void setShadowLightSetup(bool on);

// HUD elements (guest thread). The game functions that draw one in-game HUD element
// (the Game Boy Horror with everything on its screen, the health meter, ...) are hooked
// and run inside an AnchorScope, so the element moves as one unit:
//   - every 2D draw in the scope gets the same anchor: -1/0/+1 = moved to the left edge,
//     kept centered, moved to the right edge (the centered 4:3 projection shifted by
//     (1 - factor()) of the half-width). Only with settings().video.hudKeep4x3 off;
//   - a viewport smaller than the screen set in a scope (the GBH's live 3D view) gets the
//     same transform as the 2D around it, and so do the scissors set while that viewport
//     is current (the room portals drawn into it). This also applies with the HUD kept at
//     4:3 (anchor 0), where it keeps the 3D view inside the centered GBH frame.
// Everything else (menus, the GBH map screen, dialogue, markers placed at projected 3D
// positions) stays centered.
struct AnchorScope {
    explicit AnchorScope(int anchor);
    ~AnchorScope();
    AnchorScope(const AnchorScope&) = delete;
    AnchorScope& operator=(const AnchorScope&) = delete;
};
// A scope whose element is made of separate pieces on both sides (the room name: floor at
// the top right, name at the top left): each 2D draw goes to the edge on its own side.
constexpr int kAnchorBySide = 2;
// Anchor of the innermost scope (-1/0/+1 or kAnchorBySide); 0 outside scopes or with
// anchoring off.
int hudAnchor();
bool inHudScope();   // inside any scope (widescreen active), and not in a RenderToTextureScope

// A render-to-texture pass run from inside a HUD element's scope (the event-scene renderer
// fn_80114E18 draws the door knob's reflection into a 128x128 viewport at the EFB origin and
// copies that rectangle with GXCopyTex). Its partial viewport must stay where the game put it,
// like every render-to-texture viewport, or the copy reads a different rectangle and the pass
// is left on screen: inside this scope viewports do not follow the HUD scope around it.
struct RenderToTextureScope {
    RenderToTextureScope();
    ~RenderToTextureScope();
    RenderToTextureScope(const RenderToTextureScope&) = delete;
    RenderToTextureScope& operator=(const RenderToTextureScope&) = delete;
};

// The iris wipes (fn_80055C54, and the area-clear screens' fn_80050524): black boxes around a circle picture, laid out in the 640-wide
// 2D frame. Inside a WipeScope (widescreen active) every 2D draw is centered 4:3, and fill
// boxes that reach the 4:3 frame's left or right edge are extended to the display's edge
// (J2DFillBox hook), so the mask covers the whole width and the circle stays round.
struct WipeScope {
    WipeScope();
    ~WipeScope();
    WipeScope(const WipeScope&) = delete;
    WipeScope& operator=(const WipeScope&) = delete;
};
bool inWipe();

// Full-screen 2D pictures (title backdrops, menu backgrounds), --ws-backgrounds.
enum class Background { Pillarbox, Stretch, Zoom };
Background backgroundMode();

}  // namespace recomp::ws
