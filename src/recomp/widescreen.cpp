#include "recomp/widescreen.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/version.h"
#include "recomp/coop.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <atomic>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
namespace tags {
bool textBoxShown();  // tags.cpp: a message window is on screen
}
}
#include <cmath>
#include <vector>

namespace recomp::ws {
namespace {

constexpr float kBaseAspect = 4.0f / 3.0f;
std::atomic<float> sFactor{1.0f};
std::atomic<float> sFov{1.0f};
std::atomic<bool> sAnchorHud{false};
bool sOriginal = false;  // 4:3 mode: Aurora fits the 4:3 frame into the window
bool sPolicySet = false;

float factorFor(float aspect) { return aspect > kBaseAspect ? kBaseAspect / aspect : 1.0f; }

}  // namespace

// --aspect on the command line wins; otherwise the Visuals tab's setting (live).
float configuredAspect() {
    if (port::options().aspectRatio > 0.0f) return port::options().aspectRatio;
    switch (port::settings().video.aspect) {
    case port::AspectMode::Original4x3: return kBaseAspect;
    case port::AspectMode::Wide16x9: return 16.0f / 9.0f;
    case port::AspectMode::Wide16x10: return 16.0f / 10.0f;
    case port::AspectMode::Ultra21x9: return 21.0f / 9.0f;
    case port::AspectMode::Ultra32x9: return 32.0f / 9.0f;
    default: return 0.0f;  // match the window
    }
}

void applyPolicy(float aspect) {
    const bool original = aspect > 0.0f && aspect <= kBaseAspect + 1e-3f;
    if (original != sOriginal || !sPolicySet) {
        sOriginal = original;
        sPolicySet = true;
        AuroraSetViewportPolicy(sOriginal ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
        log("[ws] aspect %s", sOriginal ? "4:3 (original)" : aspect > 0.0f ? "fixed" : "auto (window)");
    }
}

void init() { applyPolicy(configuredAspect()); }

void beginFrame() {
    const float aspect = configuredAspect();
    applyPolicy(aspect);
    float f = 1.0f;
    if (!sOriginal) {
        if (aspect > 0.0f) {
            f = factorFor(aspect);
        } else {
            u32 w = 0, h = 0;
            AuroraGetRenderSize(&w, &h);
            if (w && h) f = factorFor(float(w) / float(h));
        }
    }
    const float old = sFactor.exchange(f);
    if (old != f) log("[ws] widescreen factor %.4f (aspect %.4f)", f, kBaseAspect / f);
    sFov = std::clamp(port::settings().video.fovScale, 0.5f, 2.0f);
    sAnchorHud = !port::settings().video.hudKeep4x3;
}

float fovRatio(float m11) {
    const float k = sFov.load(std::memory_order_relaxed);
    if (k == 1.0f || m11 <= 0.0f) return 1.0f;
    const float half = std::atan(1.0f / m11);
    const float scaled = std::clamp(half * k, 0.01f, 1.5f);
    return std::tan(half) / std::tan(scaled);
}

bool adjusting() { return active() || sFov.load(std::memory_order_relaxed) != 1.0f; }

namespace {
bool sShadowLightSetup = false;
}  // namespace
void setShadowLightSetup(bool on) { sShadowLightSetup = on; }


float factor() { return sFactor.load(std::memory_order_relaxed); }

Background backgroundMode() { return Background(port::options().wsBackgrounds); }

// fn_80008D58(camera, top, bottom, left, right, near, far) builds the camera's
// four side culling planes. All 3D projection setters (fn_80008654,
// fn_800087D0, fn_80008928) end with it, so widening left/right here keeps the
// culling volume in step with the Hor+ projection from the GX HLE.
// The field-of-view setting scales all four sides by the same ratio as the
// projection (from the camera's fovY at +0x90, degrees).
HLE(fn_80008D58) {
    const u32 cam = c.r[3];
    // Viewport x and width (the European camera has 12 more bytes before them; fovY is unmoved).
    const bool fullScreen = rd32(cam + VOFF(0x1EC, 0x1F8)) == 0 && rd32(cam + VOFF(0x1F4, 0x200)) >= 640;
    if (adjusting() && fullScreen && !sShadowLightSetup) {
        const float fovY = rdf32(cam + 0x90) * 3.14159265f / 180.0f;
        const double sides = fovY > 0.0f ? 1.0 / fovRatio(1.0f / std::tan(fovY * 0.5f)) : 1.0;
        const double inv = sides / factor();
        c.setSingle(1, float(c.f[1] * sides));
        c.setSingle(2, float(c.f[2] * sides));
        c.setSingle(3, float(c.f[3] * inv));
        c.setSingle(4, float(c.f[4] * inv));
    }
    callOriginal(c, "fn_80008D58");
}


// HUD anchoring (widescreen.h, AnchorScope). The in-game HUD is drawn once per frame by
// fn_8003E3D0, which calls one function per element; those are hooked here, so whatever
// they draw (J2DScreens, panes, pictures, text, the GBH's screen) moves as one unit.
// Elements not listed (the element meter, the ghost counters) stay centered with the
// dialogue and menus. The guest thread is the only caller.
namespace {
int sAnchorStack[16];
int sAnchorDepth = 0;
int sRttDepth = 0;  // RenderToTextureScope nesting
}  // namespace

AnchorScope::AnchorScope(int anchor) {
    if (sAnchorDepth < int(std::size(sAnchorStack))) sAnchorStack[sAnchorDepth] = anchor;
    sAnchorDepth++;
}
AnchorScope::~AnchorScope() { sAnchorDepth--; }

int hudAnchor() {
    if (sAnchorDepth <= 0 || !sAnchorHud.load(std::memory_order_relaxed) || !active()) return 0;
    return sAnchorStack[std::min(sAnchorDepth, int(std::size(sAnchorStack))) - 1];
}

bool inHudScope() { return sAnchorDepth > 0 && sRttDepth == 0 && active(); }

RenderToTextureScope::RenderToTextureScope() { sRttDepth++; }
RenderToTextureScope::~RenderToTextureScope() { sRttDepth--; }

// Game Boy Horror (bottom right): frame, LEDs and the J2DScreen of whatever its screen
// shows (money tally, E. Gadd's call, the cut E3 clock).
// Frame interpolation: while a message window is on screen the GBH is shown at the game frame like
// the text box (interp::FrameLockScope; tags.cpp fn_80044138): during E. Gadd's call it is his call
// window, shaking as the call comes in, with his portrait on its screen.
HLE(fn_8003D634) {
    AnchorScope s(1);
    const bool call = interp::capturing() && tags::textBoxShown();
    if (call) interp::pushFrameLock();
    callOriginal(c, "fn_8003D634");
    if (call) interp::popFrameLock();
}
// The static noise drawn over the GBH screen (gbnoise01..03), from the HUD and from the
// GBH view's own pass (fn_8003E7B0(1)).
HLE(fn_8003BE9C) {
    AnchorScope s(1);
    // The noise as E. Gadd's call connects belongs to his call window (fn_8003D634 above).
    const bool call = interp::capturing() && tags::textBoxShown();
    if (call) interp::pushFrameLock();
    callOriginal(c, "fn_8003BE9C");
    if (call) interp::popFrameLock();
}
// Sets up view n of the frame loop: its viewport/scissor (and for view 1 the depth-clear
// quad, fn_8000B228) are set here, and the view's draws that follow use them. View 1 is the
// GBH screen's live 3D view (unused camera mode). View 0 is the main view; it is smaller
// than the screen while the GBH frame surrounds it (r13+0x200, fn_80010F20), and then
// follows the centered frame. In the unused 2-player mode (coop.cpp) view 1 is player 2's
// half of the split screen, which spans the full width like view 0.
HLE(fn_800115F8) {
    AnchorScope s(c.r[3] == 1 && !coop::active() ? 1 : 0);
    callOriginal(c, "fn_800115F8");
}
// Event portrait renderer (<PDEMO>): 3D drawn into its own viewport. During E. Gadd's call
// (GBH screen mode lbl_804D846C = 3) that viewport is the GBH screen.
HLE(fn_80114E18) {
    // Frame interpolation: event scenes have their own camera (interp.h TagCat::SubView).
    interp::TagScope subView(interp::TagCat::SubView, VA(0x80114E18), 0, c.r[3]);
    // The call portrait keeps the GBH's anchoring while it closes: the GBH leaves call mode (3)
    // before the portrait's last frames are drawn, and centering those jumped the portrait to the
    // left at 16:9. The anchoring is kept while the portrait is drawn on consecutive game frames
    // after the call (a door close-up later starts centered).

    static u32 sLastFrame = ~0u;
    static bool sCallAnchored = false;
    const u32 frame = ::recomp::copyDispCount();
    const bool inCall = rd32(VA(0x804D846C)) == 3;
    if (inCall) sCallAnchored = true;
    else if (sLastFrame == ~0u || (frame != sLastFrame && frame != sLastFrame + 1)) sCallAnchored = false;
    sLastFrame = frame;
    AnchorScope s(sCallAnchored ? 1 : 0);
    // A portrait shown with a message window (E. Gadd's on the GBH during his call) belongs to the
    // text box: shown at the game frame (interp::FrameLockScope; tags.cpp fn_80044138). Portraits
    // without text (door close-ups) interpolate.
    const bool lock = interp::capturing() && tags::textBoxShown();
    if (lock) interp::pushFrameLock();
    callOriginal(c, "fn_80114E18");
    if (lock) interp::popFrameLock();
}
// The event scene's render-to-texture pass, called by fn_80114E18 between the scene and the
// portrait draws: when the scene defines a texture target (door scenes: the reflection of
// Luigi's hand on the brass knob) it sets the camera viewport to (0,0,w,h) (fn_8011548C), clears
// that rectangle with a copy, draws the scene's models and copies (0,0,w,h) into the target
// (fn_801168A8). Inside fn_80114E18's scope that viewport used to follow the centered HUD
// (moved right and narrowed in widescreen) while the copies kept the game's rectangle: the
// knob sampled a cleared texture, and the pass stayed on screen above-left of the knob, its
// depth hiding the door there.
HLE(fn_801142C4) {
    RenderToTextureScope rtt;
    callOriginal(c, "fn_801142C4");
}
// Game Boy Horror map screen (the frame loop's scene for it): the map is drawn into a viewport
// over the GBH's screen (112,84 416x312, fn_8002AC8C), which the bezel overlaps in 4:3. Left
// plain, that viewport is stretched across the wide frame and the map shows beside the
// centered bezel; inside a (centered) scope it is placed like the picture-in-picture views.
HLE(fn_8000C598) {
    AnchorScope s(0);
    callOriginal(c, "fn_8000C598");
}
// Room name when entering a room: the floor (top right) and the room's name (top left).
// Frame interpolation: text, shown at the game frame like every text box (tags.cpp fn_80044138).
HLE(fn_80045BDC) {
    AnchorScope s(kAnchorBySide);
    interp::FrameLockScope lock;
    callOriginal(c, "fn_80045BDC");
}
// ---- widened 2D masks (widescreen.h) ----
namespace {
int sWipeDepth = 0;
int sAccountDepth = 0;  // inside fn_80050524 (the area-clear / result screens)
int sEdgeDepth = 0;     // drawing the area-clear mask screen: pictures reach the display's edges

// Logical pixels between the 4:3 frame's edge and the display's edge (plus a little overlap).
s32 sideExtent() { return s32(std::ceil(320.0f / factor() - 320.0f)) + 2; }
}  // namespace
WipeScope::WipeScope() { sWipeDepth++; }
WipeScope::~WipeScope() { sWipeDepth--; }
bool inWipe() { return sWipeDepth > 0 && active(); }

// Result screens after the Portrificationizer (event 02's <ACCOUNT>: E. Gadd's lab, map 4):
// "Now you've caught: (Area n)" with Luigi's flashlight on a yellow wall, "You've caught a total
// of ...", "You've found:" with the money list. Drawn each frame by this function as 2D in the
// 640-wide frame, with Luigi as 3D on top (account object r3, state at +1):
//   - state 0: the iris wipe from the lab, four J2DFillBox boxes around the /timg/ad_wipe_0
//     circle picture, as in fn_80055C54 (WipeScope);
//   - states 3..7: a black J2DFillBox, then three J2DScreens from lbl_803CFA50: +8 the wall
//     (a 1280-wide tiled picture scrolled by the game, so it runs past the 4:3 frame's right
//     edge), +0 the black mask (a tiled picture over x 0..240 and the round spotlight texture in
//     two halves over 240..642) with the text, +4 the money list (behind a fading fill).
// Hor+ showed the wall beside the mask, and in the wipe the lab beside the circle: the mask only
// covered the 4:3 frame. Inside the mask screen the pictures touching the 4:3 frame's edges are
// extended to the display's edges (J2DPicture::drawTexCoord below), so the black surround covers
// any width while the spotlight and the text stay where they are.
HLE(fn_80050524) {
    const bool wipe = rd8(c.r[3] + 1) == 0;
    sAccountDepth++;
    if (wipe) sWipeDepth++;
    callOriginal(c, "fn_80050524");
    if (wipe) sWipeDepth--;
    sAccountDepth--;
}

// J2DScreen::draw(screen, x, y, graf): marks the area-clear mask screen (above).
HLE(draw__9J2DScreenFiiPC14J2DGrafContext) {
    const bool mask = sAccountDepth > 0 && active() && c.r[3] == rd32(VA(0x803CFA50));
    if (mask) sEdgeDepth++;
    callOriginal(c, "draw__9J2DScreenFiiPC14J2DGrafContext");
    if (mask) sEdgeDepth--;
}

// J2DPicture::drawTexCoord(this, x, y, w, h, s0, t0, s1, t1, s3, t3, s2, t2, mtx): one quad,
// corners (x,y) (x+w,y) (x+w,y+h) (x,y+h) with texcoords f1/f2, f3/f4, f7/f8, f5/f6, placed by
// mtx * the pane's matrix (+0x84). In the mask screen, a quad whose screen-space side lies on
// the 4:3 frame's left or right edge (or beyond it) is lengthened to the display's edge, its
// texture coordinates extrapolated along the quad: a tiled (repeat) texture keeps tiling and a
// clamped one repeats its edge texels, i.e. the black graphic itself continues to the edge.
HLE(drawTexCoord__10J2DPictureFiiiiffffffffPA3_A4_f) {
    if (sEdgeDepth > 0 && active()) {
        const u32 pane = c.r[3], mtx = c.r[8];
        const s32 x = s32(s16(c.r[4])), w = s32(s16(c.r[6]));
        // x row of mtx * pane matrix
        f32 a[4], b[3][4];
        for (int j = 0; j < 4; j++) a[j] = rdf32(mtx + 4 * j);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) b[i][j] = rdf32(pane + 0x84 + 16 * i + 4 * j);
        const f32 m00 = a[0] * b[0][0] + a[1] * b[1][0] + a[2] * b[2][0];
        const f32 m01 = a[0] * b[0][1] + a[1] * b[1][1] + a[2] * b[2][1];
        const f32 m03 = a[0] * b[0][3] + a[1] * b[1][3] + a[2] * b[2][3] + a[3];
        if (w > 0 && m00 > 1e-3f && std::fabs(m01) < 1e-4f) {
            const f32 left = m00 * f32(x) + m03, right = m00 * f32(x + w) + m03;
            const f32 ext = f32(sideExtent());
            s32 dl = 0, dr = 0;  // local units added on each side
            if (left <= 0.5f) dl = s32(std::ceil((left + ext) / m00));
            if (right >= 640.0f - 0.5f) dr = s32(std::ceil((640.0f + ext - right) / m00));
            if (dl > 0 || dr > 0) {
                f32 st[8];
                for (int i = 0; i < 8; i++) st[i] = f32(c.f[1 + i]);
                // per local unit along the top (v0 -> v1) and bottom (v3 -> v2) edges
                const f32 topS = (st[2] - st[0]) / f32(w), topT = (st[3] - st[1]) / f32(w);
                const f32 botS = (st[6] - st[4]) / f32(w), botT = (st[7] - st[5]) / f32(w);
                st[0] -= topS * f32(dl), st[1] -= topT * f32(dl);
                st[4] -= botS * f32(dl), st[5] -= botT * f32(dl);
                st[2] += topS * f32(dr), st[3] += topT * f32(dr);
                st[6] += botS * f32(dr), st[7] += botT * f32(dr);
                for (int i = 0; i < 8; i++) c.setSingle(1 + i, st[i]);
                c.r[4] = u32(x - dl);
                c.r[6] = u32(w + dl + dr);
            }
        }
    }
    callOriginal(c, "drawTexCoord__10J2DPictureFiiiiffffffffPA3_A4_f");
}

// J2DFillBox(x, y, w, h, color*): inside the iris wipe, a box touching the 4:3 frame's left or
// right edge (or lying beyond it: the side boxes get a negative width once the circle is wider
// than the screen) is extended to the display's edge. Widths stay as the game computed them
// relative to the edge it does not touch, so the hole is unchanged.
HLE(J2DFillBox__FiiiiQ28JUtility6TColor) {
    if (inWipe()) {
        const s32 ext = sideExtent();
        s32 x = s32(c.r[3]), w = s32(c.r[5]);
        const s32 right = x + w;
        if (x <= 0) {
            w = right + ext;
            x = -ext;
        }
        if (right >= 640) w = 640 + ext - x;
        c.r[3] = u32(x);
        c.r[5] = u32(w);
    }
    callOriginal(c, "J2DFillBox__FiiiiQ28JUtility6TColor");
}

// Health meter (bottom left). In the training room it shows over Luigi instead
// (fn_80041214, a projected position): that call opens a centered scope.
HLE(fn_800417F8) {
    AnchorScope s(-1);
    callOriginal(c, "fn_800417F8");
}

}  // namespace recomp::ws
