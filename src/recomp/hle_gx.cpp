// HLE: GX, forwarded to Aurora. Guest arguments are converted to host form
// (big-endian matrices and structs read out of guest memory; guest pointers to
// textures, arrays and display lists translated to host pointers into MEM1).
//
// Raw FIFO writes from game code (vertex data after GXBegin, inlined register
// writes) arrive through the write-gather pipe and are appended to Aurora's
// FIFO unchanged. The console needs no GXEnd; Aurora does, so an open draw is
// closed before the next GX call.
#include "recomp/efb_copy.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/frame.h"
#include "recomp/ghost_lights.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/version.h"
#include "recomp/widescreen.h"
#include "port/settings.h"

#include <cstring>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <aurora/enhanced.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace recomp {
namespace {

bool sInBegin = false;
bool sInDisplayList = false;  // GXBeginDisplayList..GXEndDisplayList: everything is recorded

// Fast-forward (skip.cpp): a frame that is not shown sends no draws or copies.
bool dropDraw() { return gx::g_discardDraws && !sInDisplayList; }

GXColor color(u32 p) { return GXColor{u8(rd8(p)), u8(rd8(p + 1)), u8(rd8(p + 2)), u8(rd8(p + 3))}; }
f32 farg(const Ctx& c, int n) { return f32(c.f[n]); }

template <int R, int C>
struct Mtx {
    f32 m[R][C];
    explicit Mtx(u32 p) {
        for (int i = 0; i < R; i++)
            for (int j = 0; j < C; j++) m[i][j] = rdf32(p + 4 * (i * C + j));
    }
};

// Guest GXTexObj/GXTlutObj are smaller than Aurora's, so the host objects live
// in tables and the guest object holds a tag + index (copies of it still work).
constexpr u32 kTexTag = 0x41555458;   // 'AUTX'
constexpr u32 kTlutTag = 0x4155544C;  // 'AUTL'

template <typename T>
struct ObjTable {
    std::deque<T> objs;
    std::deque<u32> owners;

    T* forInit(u32 guest, u32 tag) {
        const u32 idx = rd32(guest + 4);
        if (rd32(guest) == tag && idx < objs.size() && owners[idx] == guest) return &objs[idx];
        objs.emplace_back();
        owners.push_back(guest);
        wr32(guest, tag);
        wr32(guest + 4, u32(objs.size() - 1));
        return &objs.back();
    }
    T* get(u32 guest, u32 tag) {
        const u32 idx = rd32(guest + 4);
        if (rd32(guest) != tag || idx >= objs.size()) return nullptr;
        return &objs[idx];
    }
};
ObjTable<GXTexObj> sTex;
ObjTable<GXTlutObj> sTlut;

const void* ptr(u32 a) { return a ? hostp(a) : nullptr; }

// --- widescreen (see widescreen.h) -------------------------------------------------
// The guest's projection, viewport and scissor are tracked here, and the host
// projection/scissor are chosen per draw: unchanged ("plain", i.e. stretched to
// the window), with the projection's x row scaled by ws::factor() ("scaled":
// Hor+ for perspective, centered 4:3 for orthographic), or with the y row scaled
// by 1/factor ("zoom": a 4:3 picture enlarged to fill the width, top and bottom
// cropped).

constexpr f32 kLogicalWidth = 640.0f;

enum class ProjMode : u8 { None, Plain, Scaled, Zoom };

struct {
    f32 m[4][4] = {};
    GXProjectionType type = GX_PERSPECTIVE;
} sProj;
bool sProjSet = false;              // the guest has set a projection
bool sProjDirty = false;            // guest projection not yet sent to Aurora
ProjMode sProjEmitted = ProjMode::None;
int sProjEmittedAnchor = 0;         // HUD anchor the emitted projection was built for

struct {
    f32 left = 0, wd = kLogicalWidth;
} sVp;
// A partial viewport set by a HUD element (ws::AnchorScope: the GBH's live 3D view) is sent
// to Aurora moved and narrowed like the 2D around it, with this anchor; scissors set while
// it is current follow it too.
bool sVpFollow = false;
int sVpAnchor = 0;
struct {
    u32 x = 0, y = 0, w = 640, h = 480;
} sScissor;
bool sScissorPillar = false;  // Aurora currently has the centered-4:3 version of the guest scissor
int sScissorAnchor = 0;
// Draws written straight into the FIFO by game code (J3D inlines its display-list
// calls and matrix loads) bypass the HLE draw entry points, so the projection for
// them is brought up to date before the first raw FIFO write after any change.
bool sRawCheck = true;

// HUD anchor for the draw being prepared: a held draw keeps the one from its GXBegin (it
// is only sent at the next GX call, which may come after the HUD element's scope ended).
int sHeldAnchor = 0;
bool sInHeldPrepare = false;
// A by-side scope is resolved per held draw (classifyHeld); anything else in it stays centered.
int scopeAnchor() {
    const int a = ws::hudAnchor();
    return a == ws::kAnchorBySide ? 0 : a;
}
int hudAnchor() { return sInHeldPrepare ? sHeldAnchor : scopeAnchor(); }

// x of the centered (and anchored) 4:3 frame for logical x: what the scaled orthographic
// projection does to 2D, in pixels.
f32 pillarX(f32 x, int anchor) {
    const f32 s = ws::factor(), mid = kLogicalWidth * 0.5f;
    return mid + (x - mid) * s + f32(anchor) * (1.0f - s) * mid;
}

bool viewportFull() { return sVp.left <= 1.0f && sVp.left + sVp.wd >= kLogicalWidth - 1.0f; }

// Frame interpolation: 2D (under an orthographic projection) is blended too (HUD panes,
// dialogue boxes, screen transitions), paired by draw like 3D but not camera-factored.
u32 mark3D() {
    interp::setOrthographic(sProj.type != GX_PERSPECTIVE);
    return interp::mark();
}

void emitProjection(ProjMode mode) {
    f32 m[4][4];
    std::memcpy(m, sProj.m, sizeof(m));
    int anchor = 0;
    if (mode == ProjMode::Scaled && sProj.type == GX_PERSPECTIVE) {
        // Field of view: the vertical FOV times fovScale, then Hor+ for the width.
        const f32 r = ws::fovRatio(m[1][1]);
        const f32 s = ws::factor();
        for (int j = 0; j < 4; j++) {
            m[0][j] *= s * r;
            m[1][j] *= r;
        }
    } else if (mode == ProjMode::Scaled) {
        // 2D at 4:3, centered; HUD elements anchored to a side move to that edge.
        const f32 s = ws::factor();
        anchor = hudAnchor();
        for (int j = 0; j < 4; j++) m[0][j] *= s;
        m[0][3] += f32(anchor) * (1.0f - s);
    } else if (mode == ProjMode::Zoom) {
        const f32 s = ws::factor();
        for (int j = 0; j < 4; j++) m[1][j] /= s;
    }
    const u32 at = mark3D();
    GXSetProjection(m, sProj.type);
    interp::note(at, interp::Kind::Projection, 0, 0, 6);
    sProjEmitted = mode;
    sProjEmittedAnchor = anchor;
    sProjDirty = false;
}

void emitScissor(bool pillar) {
    sScissorAnchor = 0;
    if (pillar) {
        sScissorAnchor = sVpFollow ? sVpAnchor : hudAnchor();
        const f32 l = std::max(0.0f, pillarX(f32(sScissor.x), sScissorAnchor));
        const f32 r = std::min(kLogicalWidth, pillarX(f32(sScissor.x + sScissor.w), sScissorAnchor));
        const u32 li = u32(std::max(0.0f, std::floor(l)));
        GXSetScissor(li, sScissor.y, u32(std::ceil(r)) - li, sScissor.h);
    } else {
        GXSetScissor(sScissor.x, sScissor.y, sScissor.w, sScissor.h);
    }
    sScissorPillar = pillar;
}

// Projection mode for a draw. `ortho2D` is what an orthographic draw into the
// full-screen viewport gets: Scaled (centered 4:3) or Plain (stretched).
ProjMode drawMode(ProjMode ortho2D) {
    if (!viewportFull()) return ProjMode::Plain;
    // The shadow-map pass (light's view, fn_8005D984 .. fn_8005DBE4, flagged by the game in
    // lbl_804D8080) is rendered into the whole EFB and copied as a 640x480 texture that the
    // receivers project with the unmodified light matrices: it must not get the Hor+/FOV or
    // centered-4:3 adjustments, or the map no longer lines up with its projection
    // (docs/RENDERING.md, "Shadows").
    if (rd32(VA(0x804D8080)) != 0) return ProjMode::Plain;
    if (sProj.type == GX_PERSPECTIVE) return ws::adjusting() ? ProjMode::Scaled : ProjMode::Plain;
    return ws::active() ? ortho2D : ProjMode::Plain;
}

// LM_WS_TRACE=1: per frame, a summary of the widescreen decisions (draw path,
// projection type, viewport, result; consecutive repeats counted), logged
// whenever it differs from the previous frame's.
const bool sTrace = std::getenv("LM_WS_TRACE") != nullptr;
using TraceFrame = std::vector<std::pair<std::string, u32>>;
TraceFrame sTraceFrame, sTraceLast;
void traceNote(const char* fmt, ...) {
    if (!sTrace) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!sTraceFrame.empty() && sTraceFrame.back().first == buf) sTraceFrame.back().second++;
    else sTraceFrame.emplace_back(buf, 1);
}

void prepareDraw(ProjMode mode, const char* what = "draw") {
    traceNote("%s %s vp(%.0f,+%.0f) -> %s", what, sProj.type == GX_PERSPECTIVE ? "persp" : "ortho", sVp.left,
              sVp.wd, mode == ProjMode::Scaled ? "scaled" : mode == ProjMode::Zoom ? "zoom" : "plain");
    const bool hud = mode == ProjMode::Scaled && sProj.type == GX_ORTHOGRAPHIC;
    if (sProjSet && (sProjDirty || mode != sProjEmitted || (hud && hudAnchor() != sProjEmittedAnchor)))
        emitProjection(mode);
    const bool pillar = sVpFollow || (hud && (sScissor.x > 0 || sScissor.x + sScissor.w < 640));
    const int anchor = sVpFollow ? sVpAnchor : hudAnchor();
    if (pillar != sScissorPillar || (pillar && anchor != sScissorAnchor)) emitScissor(pillar);
}

// Mirror of the vertex layout state that immediate-mode 2D draws use, so a
// buffered draw's positions can be decoded (see coversScreen).
struct VatEntry {
    u8 cnt = 0, type = 0, frac = 0;
};
u8 sDesc[GX_VA_MAX_ATTR] = {};           // GXAttrType per attribute
VatEntry sVat[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
u32 sPosArray = 0, sPosStride = 0;
f32 sPosMtx[10][3][4];
bool sPosMtxValid[10] = {};
u32 sCurPosMtx = 0;

int attrSlot(u32 attr) { return attr == GX_VA_NBT ? GX_VA_NRM : attr < GX_VA_MAX_ATTR ? int(attr) : -1; }

int compSize(u8 type) { return type <= GX_S8 ? 1 : type <= GX_S16 ? 2 : 4; }

// Bytes one vertex of attribute `a` takes in the stream, or -1 if unknown.
int attrSize(int fmt, int a) {
    const u8 d = sDesc[a];
    if (d == GX_NONE) return 0;
    if (d == GX_INDEX8 || d == GX_INDEX16) {
        const int idx = d == GX_INDEX8 ? 1 : 2;
        return a == GX_VA_NRM && sVat[fmt][a].cnt == GX_NRM_NBT3 ? idx * 3 : idx;
    }
    if (a <= GX_VA_TEX7MTXIDX) return 1;
    const VatEntry& v = sVat[fmt][a];
    switch (a) {
    case GX_VA_POS: return (v.cnt == GX_POS_XYZ ? 3 : 2) * compSize(v.type);
    case GX_VA_NRM: return (v.cnt == GX_NRM_XYZ ? 3 : 9) * compSize(v.type);
    case GX_VA_CLR0:
    case GX_VA_CLR1: {
        static constexpr int kClr[] = {2, 3, 4, 2, 3, 4};
        return v.type < 6 ? kClr[v.type] : -1;
    }
    default:
        if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) return (v.cnt == GX_TEX_ST ? 2 : 1) * compSize(v.type);
        return -1;
    }
}

f32 readComp(const u8* p, u8 type, u8 frac) {
    const f32 scale = 1.0f / f32(1u << frac);
    switch (type) {
    case GX_U8: return f32(p[0]) * scale;
    case GX_S8: return f32(s8(p[0])) * scale;
    case GX_U16: return f32(u16(p[0] << 8 | p[1])) * scale;
    case GX_S16: return f32(s16(p[0] << 8 | p[1])) * scale;
    default: return std::bit_cast<f32>(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]);
    }
}

// Immediate-mode draws under an orthographic projection are held back until the
// draw ends, so the widescreen mode can look at where the vertices land before
// choosing the projection (see classifyHeld).
struct {
    bool active = false;
    GXPrimitive prim;
    GXVtxFmt fmt;
    u16 verts;
    std::vector<u8> bytes;    // the stream as the console's FIFO would see it (big-endian)
    std::vector<u32> writes;  // per FIFO write: byte offset | (size - 1) << 30, for exact replay
} sHeld;

// What the TEV samples, to tell a full-screen picture (a 4:3 background image)
// from a full-screen fill or a blit of an EFB copy.
std::vector<u32> sTexImage;      // guest image address (physical) per host texture object
u32 sMapImage[8] = {};           // image bound to each GXTexMapID
u8 sTevMap[16] = {};             // GXSetTevOrder map per stage (0xFF: none)
u32 sNumTevStages = 1;
std::vector<u32> sCopyDests;     // EFB copy destinations (physical), most recent last

u32 phys(u32 a) { return a & kHostMemMask; }
bool isCopyDest(u32 image) { return std::find(sCopyDests.begin(), sCopyDests.end(), image) != sCopyDests.end(); }

// True if any active TEV stage samples a texture that is not an EFB copy.
bool samplesPicture() {
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) {
        const u32 map = sTevMap[s];
        if (map >= 8) continue;  // GX_TEXMAP_NULL, or GX_TEX_DISABLE set
        if (sMapImage[map] && !isCopyDest(sMapImage[map])) return true;
    }
    return false;
}

f32 sHeldLo = 0, sHeldHi = 0;  // NDC x extent of the last decoded held draw
bool sHeldDecoded = false;
std::string sHeldChain;  // LM_HUD_TRACE
bool coversScreen() {
    const int fmt = sHeld.fmt;
    const size_t n = sHeld.verts;
    if (n == 0 || fmt >= GX_MAX_VTXFMT || sHeld.bytes.size() % n) return false;
    const size_t stride = sHeld.bytes.size() / n;
    int expected = 0, posOff = -1;
    for (int a = 0; a <= GX_VA_TEX7; a++) {
        const int sz = attrSize(fmt, a);
        if (sz < 0) return false;
        if (a == GX_VA_POS) posOff = expected;
        expected += sz;
    }
    if (size_t(expected) != stride || sDesc[GX_VA_POS] == GX_NONE) return false;
    const VatEntry pv = sVat[fmt][GX_VA_POS];
    const int comps = pv.cnt == GX_POS_XYZ ? 3 : 2;
    const int cs = compSize(pv.type);
    f32 lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < n; i++) {
        const u8* v = sHeld.bytes.data() + i * stride;
        const u32 mtxId = sDesc[GX_VA_PNMTXIDX] == GX_DIRECT ? v[0] : sCurPosMtx;
        if (mtxId % 3 || mtxId / 3 >= 10 || !sPosMtxValid[mtxId / 3]) return false;
        u8 tmp[12];
        const u8* p = v + posOff;
        if (sDesc[GX_VA_POS] != GX_DIRECT) {
            const u32 idx = sDesc[GX_VA_POS] == GX_INDEX8 ? p[0] : u32(p[0] << 8 | p[1]);
            if (!sPosArray) return false;
            for (int b = 0; b < comps * cs; b++) tmp[b] = u8(rd8(sPosArray + idx * sPosStride + b));
            p = tmp;
        }
        const f32 pos[4] = {readComp(p, pv.type, pv.frac), readComp(p + cs, pv.type, pv.frac),
                            comps == 3 ? readComp(p + 2 * cs, pv.type, pv.frac) : 0.0f, 1.0f};
        const auto& m = sPosMtx[mtxId / 3];
        f32 eye[4] = {0, 0, 0, 1};
        for (int r = 0; r < 3; r++) eye[r] = m[r][0] * pos[0] + m[r][1] * pos[1] + m[r][2] * pos[2] + m[r][3];
        const f32 ndc = sProj.m[0][0] * eye[0] + sProj.m[0][1] * eye[1] + sProj.m[0][2] * eye[2] + sProj.m[0][3];
        lo = std::min(lo, ndc);
        hi = std::max(hi, ndc);
    }
    sHeldLo = lo;
    sHeldHi = hi;
    sHeldDecoded = true;
    return lo <= -0.98f && hi >= 0.98f;
}

// A held 2D draw into the full-screen viewport:
//  - covers the logical width and is a fill or samples an EFB copy (fades,
//    screen-space effects that copy the frame and draw it back): stretched,
//    so it lines up with the stretched EFB;
//  - covers the width and samples an ordinary texture (a 4:3 background
//    picture): per --ws-backgrounds, centered by default;
//  - anything else is HUD: centered 4:3 (moved to an edge with its HUD element's anchor).
ProjMode classifyHeld() {
    sHeldDecoded = false;
    // The iris wipe's boxes and circle picture all stay in the centered 4:3 frame (the boxes
    // are extended to the display's edges in that frame, widescreen.cpp J2DFillBox).
    if (ws::inWipe()) return ProjMode::Scaled;
    if (!coversScreen()) {
        if (sHeldAnchor == ws::kAnchorBySide) sHeldAnchor = !sHeldDecoded ? 0 : sHeldLo + sHeldHi < 0.0f ? -1 : 1;
        return ProjMode::Scaled;
    }
    if (sHeldAnchor == ws::kAnchorBySide) sHeldAnchor = 0;
    traceNote("full-screen 2D, %s", samplesPicture() ? "picture" : "fill/copy");
    if (!samplesPicture()) return ProjMode::Plain;
    switch (ws::backgroundMode()) {
    case ws::Background::Stretch: return ProjMode::Plain;
    case ws::Background::Zoom: return ProjMode::Zoom;
    default: return ProjMode::Scaled;
    }
}

}  // namespace

// Vertex layout for interp's vertex-array tracking (interp_vtx.cpp).
int gx::vtxAttrSize(int fmt, int attr) { return fmt < GX_MAX_VTXFMT ? attrSize(fmt, attr) : -1; }
// Whether an active TEV stage samples an EFB copy (frame interpolation: such draws don't fade).
bool gx::samplesCopy() {
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) {
        const u32 map = sTevMap[s];
        if (map < 8 && sMapImage[map] && isCopyDest(sMapImage[map])) return true;
    }
    return false;
}
u8 gx::vtxDesc(int attr) { return attr < GX_VA_MAX_ATTR ? sDesc[attr] : 0; }
bool gx::vtxAttrFormat(int fmt, int attr, u8& cnt, u8& type, u8& frac) {
    if (fmt >= GX_MAX_VTXFMT || attr >= GX_VA_MAX_ATTR) return false;
    cnt = sVat[fmt][attr].cnt, type = sVat[fmt][attr].type, frac = sVat[fmt][attr].frac;
    return true;
}

namespace {

void rawFifoWrite(u32 v, int size) {
    interp::snapBytes(v, size);
    switch (size) {
    case 1: GXCmd1u8(u8(v)); break;
    case 2: GXCmd1u16(u16(v)); break;
    default: GXCmd1u32(v); break;
    }
}

gx::DisplayCopy sDisplayCopy;

}  // namespace

const gx::DisplayCopy& gx::displayCopy() { return sDisplayCopy; }

void gx::recheckDrawState() { sRawCheck = true; }

// Enhanced lighting used to force the smoothing on; it now turns the 6-bit framebuffer off
// altogether (enhancedOverridesEfbDepth), so nothing forces the smoothing any more.
bool gx::smoothBandingForced() { return false; }

int gx::smoothBandingLevel() {
    static const int sForce = [] {
        const char* e = std::getenv("LM_SMOOTH_BANDING");
        return e ? std::clamp(std::atoi(e), 0, 2) : -1;
    }();
    if (sForce >= 0) return sForce;
    return port::settings().rendering.smoothBanding || smoothBandingForced() ? 1 : 0;
}

// F1 > Visuals > Rendering accuracy options -> Aurora. Test overrides: LM_ACCURACY=<mask>
// (bit 0 integer TEV, bit 1 EFB color depth, bit 2 texture copy filter, bit 3 6-bit blending, bit 4 early-Z alpha
// depth) and LM_ACCURACY_AB=<seconds>
// (alternate between the configured options and all off every <seconds>, for A/B screenshots of one run).
bool gx::enhancedOverridesEfbDepth() { return enh::configuredOn(); }

void gx::applyAccuracySettings() {
    const auto& r = port::settings().rendering;
    u32 flags = (r.integerTev ? GX_AURORA_ACCURACY_INT_TEV : 0) | (r.efbColorDepth ? GX_AURORA_ACCURACY_EFB_DEPTH : 0) |
                (r.efbColorDepth && r.efbBlend6 ? GX_AURORA_ACCURACY_EFB_BLEND : 0) |
                (r.earlyZAlphaDepth ? GX_AURORA_ACCURACY_EARLY_Z : 0) |
                // Interpolated presentation replays every frame on the host after the fact: the
                // per-draw early-Z split cost ~20 ms per replay in the foyer (presentation fell to
                // ~35 fps); merged split pairs keep it at ~4 ms. Original 30 keeps the exact split.
                (r.earlyZAlphaDepth && (port::settings().video.matchDisplay || port::settings().video.targetFps > port::kMinFps)
                     ? GX_AURORA_ACCURACY_EARLY_Z_MERGE
                     : 0) |
                (r.textureCopyFilter ? GX_AURORA_ACCURACY_COPY_FILTER : 0);
    // Enhanced lighting turns the 6-bit EFB (and with it 6-bit blending, the dither and the
    // smoothing of 0190) off: its smooth per-pixel and colored-light gradients went through the
    // 6-bit framebuffer and the game's RGB565 scene copy, which banded them into visible steps
    // (docs/RENDERING.md "Enhanced lighting fixes"; the F1 toggles are greyed out while it is on).
    const bool efbDepth = r.efbColorDepth && !enhancedOverridesEfbDepth();
    if (!efbDepth) flags &= ~(GX_AURORA_ACCURACY_EFB_DEPTH | GX_AURORA_ACCURACY_EFB_BLEND);
    // Smooth color banding (Aurora 0190): an enhancement on top of the EFB color depth
    // (docs/RENDERING.md "Smooth color banding").
    const int smooth = gx::smoothBandingLevel();
    if (efbDepth && smooth > 0)
        flags |= GX_AURORA_ACCURACY_EFB_SMOOTH | (smooth == 1 ? GX_AURORA_ACCURACY_EFB_SMOOTH_NOISE : 0);
    static const char* sMask = std::getenv("LM_ACCURACY");
    if (sMask) flags = u32(std::strtoul(sMask, nullptr, 0));
    static const double sAb = [] {
        const char* e = std::getenv("LM_ACCURACY_AB");
        return e ? std::atof(e) : 0.0;
    }();
    if (sAb > 0.0) {
        static const auto sStart = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - sStart).count();
        const long long phase = (long long)(t / sAb);
        const bool off = phase % 2 == 1;
        // Frame rate per phase (presented frames; use an uncapped frame rate to measure cost).
        static long long sPhase = 0;
        static int sFrames = 0;
        if (phase != sPhase) {
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            log("[accuracy] phase %lld (%s): %.1f fps, ended at %.1f", sPhase, sPhase % 2 ? "off" : "on", sFrames / sAb, now);
            sPhase = phase;
            sFrames = 0;
        }
        sFrames++;
        static const bool sFilter = port::settings().rendering.displayCopyFilter;
        if (off) flags = 0;
        port::settings().rendering.displayCopyFilter = off ? false : sFilter;
    }
    if (flags != GXGetAuroraAccuracy()) GXSetAuroraAccuracy(flags);
    // LM_TEXGEN_BEYOND=0|1|2 (test aid): what a stage reads from a texcoord beyond GXSetNumTexGens
    // ((0,0) / generated from its texgen (default, the console's behaviour) / texcoord 0).
    static const int sBeyond = [] {
        const char* e = std::getenv("LM_TEXGEN_BEYOND");
        if (e) GXSetAuroraTexGenBeyondCount(u32(std::atoi(e)));
        return 0;
    }();
    (void)sBeyond;
}

// LM_HUD_TRACE=<first>,<last>[,<step>]: for those frames (GXCopyDisp count), every 2D draw, viewport,
// scissor and texture copy with its guest call chain, to map HUD pieces to game functions.
static u32 sDiagFrame = 0;
static bool hudTraceOn() {
    static const auto range = [] {
        u32 a = ~0u, b = 0, step = 1;
        if (const char* v = std::getenv("LM_HUD_TRACE")) std::sscanf(v, "%u,%u,%u", &a, &b, &step);
        return std::tuple<u32, u32, u32>{a, b, step ? step : 1};
    }();
    const auto [first, last, step] = range;
    return sDiagFrame >= first && sDiagFrame <= last && (sDiagFrame - first) % step == 0;
}
static std::string guestChain(const Ctx& c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", c.lr);
    std::string s = buf;
    u32 sp = c.r[1];
    for (int i = 0; i < 12; i++) {
        sp = rd32(sp);
        if (sp < 0x80000000u || sp >= 0x81800000u) break;
        std::snprintf(buf, sizeof(buf), "<%08X", rd32(sp + 4));
        s += buf;
    }
    return s;
}
#define HUD_TRACE(fmt, ...) \
    do { if (hudTraceOn()) log("[hud] f%u " fmt, sDiagFrame, __VA_ARGS__); } while (0)

// LM_IND_LOG=1: every draw made while indirect texturing is active (GXSetNumIndStages > 0 or an
// active TEV stage with an indirect setup), grouped per frame by guest call chain + indirect
// state, printed whenever a frame's set differs from the previous frame's. Mirrors the state
// from the HLE'd GX calls (all of LM's indirect setup goes through them: fn_8005FDD4 and J3D);
// AURORA_GX_AUDIT=1 cross-checks state that would arrive as raw BP writes in display lists.
namespace {
struct IndShadow {
    u32 numInd = 0;
    u32 tev[16] = {};        // GXSetTevIndirect word: st | fmt<<2 | bias<<4 | alpha<<7 | mtx<<9 | ws<<13 | wt<<16 | lod<<19 | prev<<20
    u8 order[4][2] = {};     // texcoord, texmap
    u8 scale[4][2] = {};
    f32 mtx[3][6] = {};
    s8 exp[3] = {};
    u8 texGenMtx[8] = {};
    u8 numTexGens = 0;      // GXSetTexCoordGen2 matrix per texcoord
    f32 texMtx[10][8] = {};    // GX_TEXMTX0..9 (2x4 rows), as loaded
} sInd;
std::unordered_map<u32, u32> sImageDesc;  // image -> w<<20 | h<<8 | fmt (LM_IND_LOG only)
std::map<std::string, u32> sIndFrame, sIndLast;
u32 sIndLastFrame = 0;
// LM_IND_OFF=1 (A/B test aid): indirect stages are never enabled, i.e. the warp is drawn unwarped.
bool indOff() {
    static const bool on = [] { const char* e = std::getenv("LM_IND_OFF"); return e && e[0] == '1'; }();
    return on;
}
bool indLogOn() {
    static const bool on = [] { const char* e = std::getenv("LM_IND_LOG"); return e && e[0] == '1'; }();
    return on;
}
u32 indWord(u32 st, u32 fmt, u32 bias, u32 mtx, u32 ws, u32 wt, u32 prev, u32 lod, u32 alpha) {
    return st | fmt << 2 | bias << 4 | alpha << 7 | mtx << 9 | ws << 13 | wt << 16 | lod << 19 | prev << 20;
}
std::string imageDesc(u32 image) {
    char b[64];
    const auto it = sImageDesc.find(image);
    if (it == sImageDesc.end()) std::snprintf(b, sizeof(b), "%08X", image);
    else std::snprintf(b, sizeof(b), "%08X(%ux%u f%u)", image, it->second >> 20, (it->second >> 8) & 0xFFF, it->second & 0xFF);
    return b;
}
}  // namespace

// LM_TEXGEN_LOG=1: every distinct draw (guest call chain) whose TEV or indirect stages read a
// texture coordinate at or beyond GXSetNumTexGens, logged once with the stage setup.
u8 sTevCoord[16] = {};
static void texgenNoteDraw(const Ctx& c, const char* kind) {
    static const bool on = [] { const char* e = std::getenv("LM_TEXGEN_LOG"); return e && e[0] == '1'; }();
    if (!on) return;
    std::string what;
    char b[96];
    for (u32 s = 0; s < sNumTevStages && s < 16; s++)
        if (sTevMap[s] < 8 && sTevCoord[s] < 8 && sTevCoord[s] >= sInd.numTexGens)
            std::snprintf(b, sizeof(b), " tev%u{tc%u map%u}", s, sTevCoord[s], sTevMap[s]), what += b;
    for (u32 i = 0; i < sInd.numInd && i < 4; i++)
        if (sInd.order[i][0] >= sInd.numTexGens)
            std::snprintf(b, sizeof(b), " ind%u{tc%u map%u}", i, sInd.order[i][0], sInd.order[i][1]), what += b;
    if (what.empty()) return;
    static std::map<std::string, u32> sSeen;
    std::snprintf(b, sizeof(b), " | nTexGen=%u nTev=%u nInd=%u", sInd.numTexGens, sNumTevStages, sInd.numInd);
    const std::string k = std::string(kind) + " " + guestChain(c) + b + what;
    if (sSeen[k]++ == 0) log("[texgen] f%u %s", sDiagFrame, k.c_str());
}
static void indNoteDraw(const Ctx& c, const char* kind) {
    texgenNoteDraw(c, kind);
    if (!indLogOn()) return;
    bool any = sInd.numInd > 0;
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) any |= sInd.tev[s] != 0;
    if (!any) return;
    std::string k = std::string(kind) + " " + guestChain(c);
    char b[160];
    std::snprintf(b, sizeof(b), " | nInd=%u nTev=%u nTexGen=%u", sInd.numInd, sNumTevStages, sInd.numTexGens);
    k += b;
    for (u32 i = 0; i < sInd.numInd && i < 4; i++) {
        std::snprintf(b, sizeof(b), " ind%u{tc%u map%u %s scale%u,%u}", i, sInd.order[i][0], sInd.order[i][1],
                      sInd.order[i][1] < 8 ? imageDesc(sMapImage[sInd.order[i][1]]).c_str() : "-", sInd.scale[i][0],
                      sInd.scale[i][1]);
        k += b;
        if (const u32 img = sInd.order[i][1] < 8 ? sMapImage[sInd.order[i][1]] : 0) {
            const u8* p = hostp(0x80000000u | img);
            std::snprintf(b, sizeof(b), " bytes[%02X %02X %02X %02X %02X %02X %02X %02X]", p[0], p[1], p[2], p[3], p[4],
                          p[5], p[6], p[7]);
            k += b;
        }
        const u32 tm = sInd.texGenMtx[sInd.order[i][0] & 7];
        if (tm >= GX_TEXMTX0 && tm <= GX_TEXMTX9) {
            const f32* x = sInd.texMtx[(tm - GX_TEXMTX0) / 3];
            std::snprintf(b, sizeof(b), " texmtx%u[%.4f %.4f %.4f %.4f; %.4f %.4f %.4f %.4f]", (tm - GX_TEXMTX0) / 3,
                          x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
            k += b;
        }
    }
    u32 mtxUsed = 0;
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) {
        const u32 w = sInd.tev[s];
        if (!w) continue;
        std::snprintf(b, sizeof(b), " tev%u{ind%u fmt%u bias%u alpha%u mtx%u wrap%u,%u lod%u prev%u map%u %s}", s,
                      w & 3, (w >> 2) & 3, (w >> 4) & 7, (w >> 7) & 3, (w >> 9) & 15, (w >> 13) & 7, (w >> 16) & 7,
                      (w >> 19) & 1, (w >> 20) & 1, sTevMap[s],
                      sTevMap[s] < 8 ? imageDesc(sMapImage[sTevMap[s]]).c_str() : "-");
        k += b;
        // RGB5A3 image (e.g. an EFB copy, read back with LM_EFB_READBACK_ALL=1): opaque/translucent texels
        if (const u32 img = sTevMap[s] < 8 ? sMapImage[sTevMap[s]] : 0; img) {
            const auto it = sImageDesc.find(img);
            if (it != sImageDesc.end() && (it->second & 0xFF) == GX_TF_RGB5A3) {
                const u32 n = (it->second >> 20) * ((it->second >> 8) & 0xFFF);
                const u8* p = hostp(0x80000000u | img);
                u32 opaque = 0, clear = 0;
                for (u32 t = 0; t < n; t++) {
                    const u16 v = u16(p[2 * t] << 8 | p[2 * t + 1]);
                    if (v & 0x8000) opaque++;
                    else if ((v >> 12) == 0) clear++;
                }
                std::snprintf(b, sizeof(b), " [5A3 opaque %u, alpha0 %u, translucent %u]", opaque, clear,
                              n - opaque - clear);
                k += b;
            }
        }
        const u32 m = (w >> 9) & 15;
        if (m) mtxUsed |= 1u << ((m - 1) & 3);
    }
    for (u32 m = 0; m < 3; m++) {
        if (!(mtxUsed & 1u << m)) continue;
        const f32* x = sInd.mtx[m];
        std::snprintf(b, sizeof(b), " mtx%u[%.4f %.4f %.4f; %.4f %.4f %.4f]e%d", m, x[0], x[1], x[2], x[3], x[4], x[5],
                      sInd.exp[m]);
        k += b;
    }
    sIndFrame[k]++;
}
static void indEndFrame() {
    if (!indLogOn()) return;
    if (sIndFrame != sIndLast) {
        std::string text;
        for (const auto& [k, n] : sIndFrame) text += "\n  " + k + (n > 1 ? " x" + std::to_string(n) : "");
        log("[ind] frame %u (same as previous for %u frames):%s", sDiagFrame, sIndLastFrame,
            sIndFrame.empty() ? " none" : text.c_str());
        std::fflush(stdout);
        sIndLastFrame = 0;
    }
    sIndLastFrame++;
    sIndLast = std::move(sIndFrame);
    sIndFrame.clear();
}

static u32 sDlFrame = 0;
// LM_DL_TRACE_FRAMES=<first>,<last>: only those frames (by the trace's own frame count).
static bool dlTraceFrameOn() {
    static const std::pair<u32, u32> range = [] {
        u32 a = 0, b = ~0u;
        if (const char* e = std::getenv("LM_DL_TRACE_FRAMES")) std::sscanf(e, "%u,%u", &a, &b);
        return std::pair<u32, u32>{a, b};
    }();
    return sDlFrame >= range.first && sDlFrame <= range.second;
}
static FILE* dlTraceFile() {
    static FILE* f = [] { const char* p = std::getenv("LM_DL_TRACE"); return p ? std::fopen(p, "w") : nullptr; }();
    return f && dlTraceFrameOn() ? f : nullptr;
}

u32 copyDispCount();
void gx::endFrame() {
    endPendingDraw();
    interp::snapEndFrame();
    efbcopy::onFrame(copyDispCount());
    indEndFrame();
    if (FILE* f = dlTraceFile()) std::fprintf(f, "\nF%u", sDlFrame), std::fflush(f);
    sDlFrame++;
    if (!sTrace) return;
    static u32 frameNo = 0;
    if (sTraceFrame != sTraceLast) {
        std::string text;
        for (const auto& [note, n] : sTraceFrame) text += "\n  " + note + (n > 1 ? " x" + std::to_string(n) : "");
        log("[ws] frame %u (factor %.3f):%s", frameNo, ws::factor(), text.c_str());
        std::fflush(stdout);
    }
    frameNo++;
    sTraceLast = std::move(sTraceFrame);
    sTraceFrame.clear();
}

void gx::endPendingDraw() {
    if (!sInBegin) return;
    sInBegin = false;
    if (sHeld.active) {
        sHeld.active = false;
        sInHeldPrepare = true;
        const ProjMode mode = classifyHeld();
        prepareDraw(mode, "held");
        sInHeldPrepare = false;
        HUD_TRACE("held %s x[%.3f,%.3f] mode=%d anchor=%d tex %s %s", sHeldDecoded ? "dec" : "nodec", sHeldLo, sHeldHi,
                  int(mode), sHeldAnchor,
                  sTevMap[0] < 8 && sMapImage[sTevMap[0]] ? imageDesc(sMapImage[sTevMap[0]]).c_str() : "-",
                  sHeldChain.c_str());
        sRawCheck = true;
        interp::snapBegin(sHeld.fmt, sHeld.verts);
        GXBegin(sHeld.prim, sHeld.fmt, sHeld.verts);
        for (u32 w : sHeld.writes) {
            const int size = int(w >> 30) + 1;  // 1, 2 or 4 (encoded 0, 1, 3)
            const u32 off = w & 0x3FFFFFFF;
            u32 v = 0;
            for (int b = 0; b < size; b++) v = v << 8 | sHeld.bytes[off + b];
            rawFifoWrite(v, size);
        }
    }
    interp::snapEnd();
    GXEnd();
    interp::snapAfterDraw();
    interp::vtxEnd();
}

void hw::fifoWrite(u32 v, int size) {
    frame::gxWaitForGuestTurn();
    if (dropDraw()) [[unlikely]] return;
    if (interp::capturing()) interp::vtxBytes(v, size);
    if (sHeld.active) [[unlikely]] {
        const u32 off = u32(sHeld.bytes.size());
        for (int b = size - 1; b >= 0; b--) sHeld.bytes.push_back(u8(v >> (8 * b)));
        sHeld.writes.push_back(off | u32(size - 1) << 30);
        return;
    }
    if (sRawCheck && !sInBegin) [[unlikely]] {
        sRawCheck = false;
        prepareDraw(drawMode(ProjMode::Scaled), "fifo");
    }
    rawFifoWrite(v, size);
}

#define GXHLE(name)                                       \
    static void gxImpl_##name(::recomp::Ctx& c);          \
    HLE(name) {                                           \
        frame::gxWaitForGuestTurn();                      \
        gx::endPendingDraw();                             \
        interp::gxCall(c);                                \
        gxImpl_##name(c);                                 \
    }                                                     \
    static void gxImpl_##name([[maybe_unused]] ::recomp::Ctx& c)

// GX state audit (LM_GX_AUDIT=1): logs each distinct argument combination of the state calls
// that matter for hardware accuracy (copy filter, gamma, fog, lights, LOD...), once.
static void gxAudit(const char* fmt, ...) {
    static const bool on = [] {
        const char* e = std::getenv("LM_GX_AUDIT");
        return e && e[0] == '1';
    }();
    if (!on) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static std::vector<std::string> seen;
    if (std::find(seen.begin(), seen.end(), buf) != seen.end()) return;
    seen.emplace_back(buf);
    log("[gx-audit] %s", buf);
}

// --- setup / sync ---------------------------------------------------------------

GXHLE(GXInit) {
    static u32 fifoObj = 0;
    if (!fifoObj) fifoObj = hleAlloc(128);
    GXInit(hostp(c.r[3]), c.r[4]);
    sInd = {};
    c.r[3] = fifoObj;
}
GXHLE(GXFlush) { GXFlush(); }
// The console's GXDrawDone sleeps until the draw-done interrupt, so interrupts and other
// threads run meanwhile; draining Aurora's FIFO here can take a good part of a frame.
GXHLE(GXDrawDone) { frame::gxBlockingWait([] { GXDrawDone(); }); }
GXHLE(GXSetDrawDone) {}
GXHLE(GXWaitDrawDone) {}
GXHLE(GXPixModeSync) { GXPixModeSync(); }
GXHLE(GXSetMisc) {}
GXHLE(GXInvalidateVtxCache) {
    GXInvalidateVtxCache();
    interp::invalidateVtxCache();
}
GXHLE(GXInvalidateTexAll) { GXInvalidateTexAll(); }

// --- vertex format ----------------------------------------------------------------

GXHLE(GXClearVtxDesc) {
    std::memset(sDesc, 0, sizeof(sDesc));
    GXClearVtxDesc();
}
GXHLE(GXSetVtxDesc) {
    if (const int a = attrSlot(c.r[3]); a >= 0) sDesc[a] = u8(c.r[4]);
    GXSetVtxDesc(GXAttr(c.r[3]), GXAttrType(c.r[4]));
}
GXHLE(GXSetVtxDescv) {
    GXVtxDescList list[32];
    int n = 0;
    for (u32 p = c.r[3]; n < 31; p += 8) {
        list[n].attr = GXAttr(rd32(p));
        list[n].type = GXAttrType(rd32(p + 4));
        if (const int a = attrSlot(list[n].attr); a >= 0) sDesc[a] = u8(list[n].type);
        if (list[n++].attr == GX_VA_NULL) break;
    }
    list[n - 1].attr = GX_VA_NULL;
    GXSetVtxDescv(list);
}
GXHLE(GXGetVtxDesc) {
    GXAttrType t;
    GXGetVtxDesc(GXAttr(c.r[3]), &t);
    wr32(c.r[4], u32(t));
}
GXHLE(GXSetVtxAttrFmt) {
    if (const int a = attrSlot(c.r[4]); a >= 0 && c.r[3] < GX_MAX_VTXFMT)
        sVat[c.r[3]][a] = {u8(c.r[5]), u8(c.r[6]), u8(c.r[7])};
    GXSetVtxAttrFmt(GXVtxFmt(c.r[3]), GXAttr(c.r[4]), GXCompCnt(c.r[5]), GXCompType(c.r[6]), u8(c.r[7]));
}
GXHLE(GXSetVtxAttrFmtv) {
    GXVtxAttrFmtList list[32];
    int n = 0;
    for (u32 p = c.r[4]; n < 31; p += 16) {
        list[n] = {GXAttr(rd32(p)), GXCompCnt(rd32(p + 4)), GXCompType(rd32(p + 8)), u8(rd8(p + 12))};
        if (const int a = attrSlot(list[n].attr); a >= 0 && c.r[3] < GX_MAX_VTXFMT)
            sVat[c.r[3]][a] = {u8(list[n].cnt), u8(list[n].type), list[n].frac};
        if (list[n++].attr == GX_VA_NULL) break;
    }
    list[n - 1].attr = GX_VA_NULL;
    GXSetVtxAttrFmtv(GXVtxFmt(c.r[3]), list);
}
// Size 0: Aurora sizes the array from the indices each draw uses (patch 0002).
GXHLE(GXSetArray) {
    if (c.r[3] == GX_VA_POS) {
        sPosArray = c.r[4];
        sPosStride = u8(c.r[5]);
    }
    const u32 at = interp::mark();
    GXSetArray(GXAttr(c.r[3]), ptr(c.r[4]), 0, u8(c.r[5]), false);
    interp::arrayBase(at, c.r[3], c.r[4], u8(c.r[5]));
}

GXHLE(GXBegin) {
    if (dropDraw()) return;
    indNoteDraw(c, "begin");
    sInBegin = true;
    if (interp::drawLogActive()) interp::drawLogCaller(guestChain(c));
    if (interp::capturing()) interp::vtxBegin(c.r[3], c.r[4], c.r[5], sProj.type == GX_PERSPECTIVE);
    if (ws::active() && sProj.type == GX_ORTHOGRAPHIC && viewportFull()) {
        sHeld.active = true;
        sHeldAnchor = ws::hudAnchor();
        sHeld.prim = GXPrimitive(c.r[3]);
        sHeld.fmt = GXVtxFmt(c.r[4]);
        sHeld.verts = u16(c.r[5]);
        if (hudTraceOn()) sHeldChain = guestChain(c);
        sHeld.bytes.clear();
        sHeld.writes.clear();
        return;
    }
    prepareDraw(drawMode(ProjMode::Scaled), "begin");
    interp::snapBegin(c.r[4], c.r[5]);
    GXBegin(GXPrimitive(c.r[3]), GXVtxFmt(c.r[4]), u16(c.r[5]));
}

GXHLE(GXCallDisplayList) {
    // A list outside guest memory (a corrupt pointer: the console's GPU would read garbage) would
    // crash the host reading it (Aurora, the vertex snapshots, interpolation's parse): skipped.
    if ((c.r[3] >> 28) != 0xE && ((c.r[3] & kHostMemMask) + u64(c.r[4]) > kHostMemSize || c.r[4] > (16u << 20))) {
        static int sLogged = 0;
        if (sLogged++ < 8) log("[gx] GXCallDisplayList(%08X, %X) outside guest memory, skipped (lr %08X)", c.r[3], c.r[4], c.lr);
        return;
    }
    if (FILE* f = dlTraceFile()) {
        std::fprintf(f, " %08X:%X@%08X", c.r[3], c.r[4], c.lr);
        u32 sp = c.r[1];
        for (int i = 0; i < 10 && sp >= 0x80000000u && sp < 0x81800000u; i++) {
            sp = rd32(sp);
            if (sp < 0x80000000u || sp >= 0x81800000u) break;
            std::fprintf(f, "<%08X", rd32(sp + 4));
        }
    }
    if (dropDraw()) return;
    if (sProj.type == GX_ORTHOGRAPHIC || !viewportFull())
        HUD_TRACE("dlist %s vp(%.0f,+%.0f) %s", sProj.type == GX_ORTHOGRAPHIC ? "ortho" : "persp", sVp.left, sVp.wd,
                  guestChain(c).c_str());
    prepareDraw(drawMode(ProjMode::Scaled), "dlist");
    indNoteDraw(c, "dlist");
    interp::snapDisplayList(hostp(c.r[3]), c.r[4]);
    GXCallDisplayList(hostp(c.r[3]), c.r[4]);
    interp::snapAfterDraw();
    if (interp::drawLogActive()) interp::drawLogCaller(guestChain(c));
    if (interp::capturing()) interp::vtxDisplayList(c.r[3], hostp(c.r[3]), c.r[4]);
}
GXHLE(GXBeginDisplayList) {
    sInDisplayList = true;
    interp::suspendForDisplayList();
    GXBeginDisplayList(hostp(c.r[3]), c.r[4]);
}
GXHLE(GXEndDisplayList) {
    sInDisplayList = false;
    c.r[3] = GXEndDisplayList();
    interp::resumeAfterDisplayList();
}

GXHLE(GXDrawSphere1) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawSphere1(u8(c.r[3]));
}
GXHLE(GXDrawCube) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawCube();
}
GXHLE(GXDrawCylinder) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawCylinder(u8(c.r[3]));
}

// --- texgen / transform ---------------------------------------------------------------

GXHLE(GXSetNumTexGens) {
    sInd.numTexGens = u8(c.r[3]);
    GXSetNumTexGens(u8(c.r[3]));
}
GXHLE(GXSetTexCoordGen2) {
    if (c.r[3] < 8) sInd.texGenMtx[c.r[3]] = u8(c.r[6]);
    GXSetTexCoordGen2(GXTexCoordID(c.r[3]), GXTexGenType(c.r[4]), GXTexGenSrc(c.r[5]), c.r[6], GXBool(c.r[7]),
                      c.r[8]);
}
// Sent to Aurora lazily, at the next draw, once the viewport and (for 2D) the
// vertices tell which widescreen treatment it gets.
GXHLE(GXSetProjection) {
    const Mtx<4, 4> m(c.r[3]);
    std::memcpy(sProj.m, m.m, sizeof(sProj.m));
    sProj.type = GXProjectionType(c.r[4]);
    sProjDirty = sProjSet = sRawCheck = true;
    traceNote("projection %s from %08X", c.r[4] ? "ortho" : "persp", c.lr);
    if (!ws::adjusting()) emitProjection(ProjMode::Plain);
}
GXHLE(GXLoadPosMtxImm) {
    const Mtx<3, 4> m(c.r[3]);
    if (const u32 id = c.r[4]; id % 3 == 0 && id / 3 < 10) {
        std::memcpy(sPosMtx[id / 3], m.m, sizeof(m.m));
        sPosMtxValid[id / 3] = true;
    }
    const u32 at = mark3D();
    GXLoadPosMtxImm(m.m, c.r[4]);
    interp::note(at, interp::Kind::PosMtx, c.r[3], c.r[4], 12);
}
GXHLE(GXLoadNrmMtxImm) {
    const Mtx<3, 4> m(c.r[3]);
    const u32 at = mark3D();
    GXLoadNrmMtxImm(m.m, c.r[4]);
    interp::note(at, interp::Kind::NrmMtx, c.r[3], c.r[4], 9);
}
GXHLE(GXLoadNrmMtxImm3x3) {
    const Mtx<3, 3> m3(c.r[3]);
    f32 m[3][4] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = m3.m[i][j];
    const u32 at = mark3D();
    GXLoadNrmMtxImm(m, c.r[4]);
    interp::note(at, interp::Kind::NrmMtx, c.r[3], c.r[4], 9);
}
GXHLE(GXLoadTexMtxImm) {
    const GXTexMtxType type = GXTexMtxType(c.r[5]);
    if (type == GX_MTX2x4) {
        const Mtx<2, 4> m(c.r[3]);
        if (const u32 id = c.r[4]; id >= GX_TEXMTX0 && id <= GX_TEXMTX9 && (id - GX_TEXMTX0) % 3 == 0)
            std::memcpy(sInd.texMtx[(id - GX_TEXMTX0) / 3], m.m, sizeof(sInd.texMtx[0]));
        const u32 at = mark3D();
        GXLoadTexMtxImm(m.m, c.r[4], type);
        interp::note(at, interp::Kind::TexMtx, c.r[3], c.r[4], sizeof(m.m) / 4);
    } else {
        const Mtx<3, 4> m(c.r[3]);
        const u32 at = mark3D();
        GXLoadTexMtxImm(m.m, c.r[4], type);
        interp::note(at, interp::Kind::TexMtx, c.r[3], c.r[4], sizeof(m.m) / 4);
    }
}
GXHLE(GXSetCurrentMtx) {
    sCurPosMtx = c.r[3];
    GXSetCurrentMtx(c.r[3]);
}
// Records the guest viewport; returns the x/width to send (moved and narrowed like the 2D
// around it for a partial viewport set by a HUD element, see sVpFollow).
static std::pair<f32, f32> setViewport(const Ctx& c) {
    sRawCheck = true;
    sVp.left = farg(c, 1);
    sVp.wd = farg(c, 3);
    sVpFollow = !viewportFull() && ws::inHudScope();
    sVpAnchor = sVpFollow ? scopeAnchor() : 0;
    HUD_TRACE("viewport %.1f %.1f %.1f %.1f%s %s", farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4),
              sVpFollow ? " follow" : "", guestChain(c).c_str());
    if (!sVpFollow) return {sVp.left, sVp.wd};
    return {pillarX(sVp.left, sVpAnchor), sVp.wd * ws::factor()};
}
GXHLE(GXSetViewport) {
    const auto [x, w] = setViewport(c);
    GXSetViewport(x, farg(c, 2), w, farg(c, 4), farg(c, 5), farg(c, 6));
}
GXHLE(GXSetViewportJitter) {
    gxAudit("GXSetViewportJitter field=%u", c.r[3]);
    const auto [x, w] = setViewport(c);
    GXSetViewportJitter(x, farg(c, 2), w, farg(c, 4), farg(c, 5), farg(c, 6), c.r[3]);
}
GXHLE(GXSetScissor) {
    HUD_TRACE("scissor %u %u %u %u %s", c.r[3], c.r[4], c.r[5], c.r[6], guestChain(c).c_str());
    sScissor = {c.r[3], c.r[4], c.r[5], c.r[6]};
    emitScissor(sVpFollow);
}
GXHLE(GXSetClipMode) { GXSetClipMode(GXClipMode(c.r[3])); }

// --- geometry state ------------------------------------------------------------------

GXHLE(GXSetCullMode) { GXSetCullMode(GXCullMode(c.r[3])); }
GXHLE(GXSetCoPlanar) {
    gxAudit("GXSetCoPlanar %u", c.r[3]); GXSetCoPlanar(GXBool(c.r[3])); }
GXHLE(GXSetLineWidth) {
    gxAudit("GXSetLineWidth %u off=%u", c.r[3], c.r[4]); GXSetLineWidth(u8(c.r[3]), GXTexOffset(c.r[4])); }
GXHLE(GXSetPointSize) {
    gxAudit("GXSetPointSize %u off=%u", c.r[3], c.r[4]); GXSetPointSize(u8(c.r[3]), GXTexOffset(c.r[4])); }
GXHLE(GXEnableTexOffsets) { GXEnableTexOffsets(GXTexCoordID(c.r[3]), GXBool(c.r[4]), GXBool(c.r[5])); }

// --- lighting ----------------------------------------------------------------------------
// Guest GXLightObj (64 bytes) is the same size as Aurora's, so Aurora fills it in place.

static GXLightObj* light(u32 p) { return reinterpret_cast<GXLightObj*>(hostp(p)); }

GXHLE(GXSetNumChans) { GXSetNumChans(u8(c.r[3])); }
GXHLE(GXSetChanCtrl) {
    GXSetChanCtrl(GXChannelID(c.r[3]), GXBool(c.r[4]), GXColorSrc(c.r[5]), GXColorSrc(c.r[6]), c.r[7],
                  GXDiffuseFn(c.r[8]), GXAttnFn(c.r[9]));
}
GXHLE(GXSetChanMatColor) {
    const u32 at = interp::mark();
    GXSetChanMatColor(GXChannelID(c.r[3]), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE(GXSetChanAmbColor) {
    const u32 at = interp::mark();
    GXSetChanAmbColor(GXChannelID(c.r[3]), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE(GXInitLightPos) { GXInitLightPos(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE(GXInitLightDir) { GXInitLightDir(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE(GXInitSpecularDir) {
    gxAudit("GXInitSpecularDir"); GXInitSpecularDir(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE(GXInitLightColor) { GXInitLightColor(light(c.r[3]), color(c.r[4])); }
GXHLE(GXInitLightSpot) {
    gxAudit("GXInitLightSpot cutoff=%.1f fn=%u", farg(c, 1), c.r[4]); GXInitLightSpot(light(c.r[3]), farg(c, 1), GXSpotFn(c.r[4])); }
GXHLE(GXInitLightDistAttn) {
    gxAudit("GXInitLightDistAttn ref=%.1f bright=%.2f fn=%u", farg(c, 1), farg(c, 2), c.r[4]);
    GXInitLightDistAttn(light(c.r[3]), farg(c, 1), farg(c, 2), GXDistAttnFn(c.r[4]));
}
GXHLE(GXInitLightAttn) {
    gxAudit("GXInitLightAttn a=%.3f %.3f %.3f k=%.4f %.4f %.6f", farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), farg(c, 6));
    GXInitLightAttn(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), farg(c, 6));
}
GXHLE(GXLoadLightObjImm) {
    // Position and direction (view space) are blended by frame interpolation.
    const u32 at = interp::mark();
    GXLoadLightObjImm(light(c.r[3]), GXLightID(c.r[4]));
    if (std::getenv("LM_LIGHT_TRACE")) {
        const auto* lo = reinterpret_cast<const float*>(hostp(c.r[3]) + 4);  // a0 a1 a2 k0 k1 k2 px..nz
        static std::vector<u64> seen;
        const u64 key = u64(c.lr) << 32 | c.r[4] | u64(lo[1] != 0.0f) << 31;
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            log("[light] obj %08X id %X a %.3f %.3f %.3f k %.6f %.6f %.6f col %08X chain %s", c.r[3], c.r[4], lo[0], lo[1],
                lo[2], lo[3], lo[4], lo[5], rd32(c.r[3]), guestChain(c).c_str());
        }
    }
    interp::note(at, interp::Kind::Light, c.r[3], std::countr_zero(c.r[4] | 0x100u), 6, 10);
    interp::noteColors(at, c.r[3]);
}

// --- textures ------------------------------------------------------------------------------

void diagTexInit(const char* kind, u32 obj, u32 image, u32 w, u32 h, u32 fmt, u32 lr);
static void noteTexImage(u32 obj, u32 image) {
    const u32 idx = rd32(obj + 4);
    if (idx >= sTexImage.size()) sTexImage.resize(idx + 1);
    sTexImage[idx] = phys(image);
}
GXHLE(GXInitTexObj) {
    GXInitTexObj(sTex.forInit(c.r[3], kTexTag), ptr(c.r[4]), u16(c.r[5]), u16(c.r[6]), GXTexFmt(c.r[7]),
                 GXTexWrapMode(c.r[8]), GXTexWrapMode(c.r[9]), GXBool(c.r[10]));
    noteTexImage(c.r[3], c.r[4]);
    static const bool sHudTraceSet = std::getenv("LM_HUD_TRACE") != nullptr;  // texture sizes in its held lines
    if (indLogOn() || sHudTraceSet) sImageDesc[phys(c.r[4])] = (c.r[5] & 0xFFF) << 20 | (c.r[6] & 0xFFF) << 8 | (c.r[7] & 0xFF);
    diagTexInit("INIT", c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.lr);
}
GXHLE(GXInitTexObjCI) {
    GXInitTexObjCI(sTex.forInit(c.r[3], kTexTag), ptr(c.r[4]), u16(c.r[5]), u16(c.r[6]), GXCITexFmt(c.r[7]),
                   GXTexWrapMode(c.r[8]), GXTexWrapMode(c.r[9]), GXBool(c.r[10]), stackArg(c, 0));
    noteTexImage(c.r[3], c.r[4]);
    diagTexInit("INITCI", c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.lr);
}
GXHLE(GXInitTexObjLOD) {
    gxAudit("GXInitTexObjLOD min=%u mag=%u lod=%.2f..%.2f bias=%.2f clamp=%u edge=%u aniso=%u", c.r[4], c.r[5], farg(c, 1), farg(c, 2), farg(c, 3), c.r[6], c.r[7], c.r[8]);
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag))
        GXInitTexObjLOD(o, GXTexFilter(c.r[4]), GXTexFilter(c.r[5]), farg(c, 1), farg(c, 2), farg(c, 3),
                        GXBool(c.r[6]), GXBool(c.r[7]), GXAnisotropy(c.r[8]));
}
static bool zeroTexObj(u32 obj) {
    for (u32 i = 0; i < 0x20; i += 4)
        if (rd32(obj + i)) return false;
    return true;
}
GXHLE(GXLoadTexObj) {
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag)) {
        const u32 at = interp::mark();
        GXLoadTexObj(o, GXTexMapID(c.r[4]));
        interp::texLoaded(at);
        const u32 idx = rd32(c.r[3] + 4);
        if (c.r[4] < 8) sMapImage[c.r[4]] = idx < sTexImage.size() ? sTexImage[idx] : 0;
    } else if (zeroTexObj(c.r[3])) {
        // Never initialised (zeroed .bss): the European game draws the file-select Luigi through
        // the shadow receivers' setup (fn_8005DD50) before the shadow object's constructor
        // (fn_8005D41C) has run, which the USA game calls earlier. The hardware loads the zero
        // registers: a 1x1 I4 texture at physical address 0, clamped, nearest filtering.
        static GXTexObj sZero = [] {
            GXTexObj o{};
            GXInitTexObj(&o, hostp(0x80000000), 1, 1, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
            GXInitTexObjLOD(&o, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
            return o;
        }();
        const u32 at = interp::mark();
        GXLoadTexObj(&sZero, GXTexMapID(c.r[4]));
        interp::texLoaded(at);
        if (c.r[4] < 8) sMapImage[c.r[4]] = 0;
    } else {
        static u32 sUnknown = 0;
        if (sUnknown++ < 16) log("[gx] GXLoadTexObj: unknown texture object %08X", c.r[3]);
    }
}
GXHLE(GXGetTexBufferSize) {
    c.r[3] = GXGetTexBufferSize(u16(c.r[3]), u16(c.r[4]), c.r[5], GXBool(c.r[6]), u8(c.r[7]));
}
GXHLE(GXInitTexCacheRegion) {}
GXHLE(GXInitTlutObj) {
    GXInitTlutObj(sTlut.forInit(c.r[3], kTlutTag), ptr(c.r[4]), GXTlutFmt(c.r[5]), u16(c.r[6]));
}
GXHLE(GXLoadTlut) {
    if (GXTlutObj* o = sTlut.get(c.r[3], kTlutTag)) {
        const u32 at = interp::mark();
        GXLoadTlut(o, c.r[4]);
        interp::tlutLoaded(at);
    }
}

// --- TEV -------------------------------------------------------------------------------------

GXHLE(GXSetNumTevStages) {
    sNumTevStages = u8(c.r[3]);
    GXSetNumTevStages(u8(c.r[3]));
}
// Model viewer "Textures off": texture colour reads as white and texture blend modes pass
// the lit vertex colour through, so models show lighting/vertex colours only. Alpha stays
// textured so cut-out edges survive.
static bool sUntextured = false;
void gx::setUntextured(bool on) { sUntextured = on; }
static GXTevColorArg untex(u32 a) {
    return GXTevColorArg(sUntextured && (a == GX_CC_TEXC || a == GX_CC_TEXA) ? GX_CC_ONE : a);
}

GXHLE(GXSetTevOp) {
    u32 mode = c.r[4];
    if (sUntextured && (mode == GX_MODULATE || mode == GX_DECAL || mode == GX_REPLACE)) mode = GX_PASSCLR;
    GXSetTevOp(GXTevStageID(c.r[3]), GXTevMode(mode));
}
GXHLE(GXSetTevOrder) {
    if (c.r[3] < 16) sTevMap[c.r[3]] = c.r[5] & 0x100 ? 0xFF : u8(c.r[5]), sTevCoord[c.r[3]] = u8(c.r[4]);
    GXSetTevOrder(GXTevStageID(c.r[3]), GXTexCoordID(c.r[4]), GXTexMapID(c.r[5]), GXChannelID(c.r[6]));
}
GXHLE(GXSetTevColorIn) {
    GXSetTevColorIn(GXTevStageID(c.r[3]), untex(c.r[4]), untex(c.r[5]), untex(c.r[6]), untex(c.r[7]));
}
GXHLE(GXSetTevAlphaIn) {
    GXSetTevAlphaIn(GXTevStageID(c.r[3]), GXTevAlphaArg(c.r[4]), GXTevAlphaArg(c.r[5]), GXTevAlphaArg(c.r[6]),
                    GXTevAlphaArg(c.r[7]));
}
GXHLE(GXSetTevColorOp) {
    GXSetTevColorOp(GXTevStageID(c.r[3]), GXTevOp(c.r[4]), GXTevBias(c.r[5]), GXTevScale(c.r[6]), GXBool(c.r[7]),
                    GXTevRegID(c.r[8]));
}
GXHLE(GXSetTevAlphaOp) {
    GXSetTevAlphaOp(GXTevStageID(c.r[3]), GXTevOp(c.r[4]), GXTevBias(c.r[5]), GXTevScale(c.r[6]), GXBool(c.r[7]),
                    GXTevRegID(c.r[8]));
}
GXHLE(GXSetTevColor) {
    const u32 at = interp::mark();
    GXSetTevColor(GXTevRegID(c.r[3]), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE(GXSetTevColorS10) {
    gxAudit("GXSetTevColorS10 reg=%u", c.r[3]);
    const u32 p = c.r[4];
    const u32 at = interp::mark();
    GXSetTevColorS10(GXTevRegID(c.r[3]), GXColorS10{s16(rd16(p)), s16(rd16(p + 2)), s16(rd16(p + 4)), s16(rd16(p + 6))});
    interp::noteColors(at);
}
GXHLE(GXSetTevKColor) {
    const u32 at = interp::mark();
    GXSetTevKColor(GXTevKColorID(c.r[3]), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE(GXSetTevKColorSel) { GXSetTevKColorSel(GXTevStageID(c.r[3]), GXTevKColorSel(c.r[4])); }
GXHLE(GXSetTevKAlphaSel) { GXSetTevKAlphaSel(GXTevStageID(c.r[3]), GXTevKAlphaSel(c.r[4])); }
GXHLE(GXSetTevSwapMode) { GXSetTevSwapMode(GXTevStageID(c.r[3]), GXTevSwapSel(c.r[4]), GXTevSwapSel(c.r[5])); }
GXHLE(GXSetTevSwapModeTable) {
    gxAudit("GXSetTevSwapModeTable %u %u%u%u%u", c.r[3], c.r[4], c.r[5], c.r[6], c.r[7]);
    GXSetTevSwapModeTable(GXTevSwapSel(c.r[3]), GXTevColorChan(c.r[4]), GXTevColorChan(c.r[5]),
                          GXTevColorChan(c.r[6]), GXTevColorChan(c.r[7]));
}
GXHLE(GXSetAlphaCompare) {
    GXSetAlphaCompare(GXCompare(c.r[3]), u8(c.r[4]), GXAlphaOp(c.r[5]), GXCompare(c.r[6]), u8(c.r[7]));
}
GXHLE(GXSetZTexture) { GXSetZTexture(GXZTexOp(c.r[3]), GXTexFmt(c.r[4]), c.r[5]); }

// --- indirect -----------------------------------------------------------------------------------

GXHLE(GXSetNumIndStages) {
    sInd.numInd = c.r[3] & 7;
    GXSetNumIndStages(indOff() ? 0 : u8(c.r[3]));
}
GXHLE(GXSetIndTexOrder) {
    if (c.r[3] < 4) sInd.order[c.r[3]][0] = u8(c.r[4]), sInd.order[c.r[3]][1] = u8(c.r[5]);
    GXSetIndTexOrder(GXIndTexStageID(c.r[3]), GXTexCoordID(c.r[4]), GXTexMapID(c.r[5]));
}
GXHLE(GXSetIndTexCoordScale) {
    if (c.r[3] < 4) sInd.scale[c.r[3]][0] = u8(c.r[4]), sInd.scale[c.r[3]][1] = u8(c.r[5]);
    GXSetIndTexCoordScale(GXIndTexStageID(c.r[3]), GXIndTexScale(c.r[4]), GXIndTexScale(c.r[5]));
}
GXHLE(GXSetIndTexMtx) {
    const Mtx<2, 3> m(c.r[4]);
    if (const u32 id = c.r[3] - GX_ITM_0; id < 3) {
        std::memcpy(sInd.mtx[id], m.m, sizeof(sInd.mtx[id]));
        sInd.exp[id] = s8(c.r[5]);
    }
    // LM_IND_EXP=<n> (test aid): adds n to the matrix scale exponent (2^n times the offsets).
    static const int sExpAdd = [] { const char* e = std::getenv("LM_IND_EXP"); return e ? std::atoi(e) : 0; }();
    GXSetIndTexMtx(GXIndTexMtxID(c.r[3]), m.m, s8(s8(c.r[5]) + sExpAdd));
}
GXHLE(GXSetTevDirect) {
    if (c.r[3] < 16) sInd.tev[c.r[3]] = 0;
    GXSetTevDirect(GXTevStageID(c.r[3]));
}
GXHLE(GXSetTevIndWarp) {
    // LM_IND_PATTERN=1 (test aid): replaces the noise in the Mario painting warp's 64x64 IA4
    // indirect texture (US 0x803CFD40, filled with random intensities by fn_8005FD80 at boot) with
    // a known pattern: intensity 15 (offset 0) in the left half, 0 (offset -5 texels in s and t)
    // in the right half.
    static const bool sPattern = [] { const char* e = std::getenv("LM_IND_PATTERN"); return e && e[0] == '1'; }();
    if (const u32 img = sMapImage[sInd.order[c.r[4] & 3][1] & 7]; sPattern && img) {
        u8* tex = hostp(0x80000000u | img);
        for (u32 i = 0; i < 64 * 64; i++) {
            const u32 tile = i / 32, bx = tile % 8, x = bx * 8 + i % 8;
            tex[i] = x < 32 ? 0x0F : 0x00;
        }
    }
    // SDK: GXSetTevIndirect(stage, ind, ITF_8, signed ? ITB_STU : NONE, mtx, replace ? ITW_0 : OFF (s,t), 0, 0, 0)
    if (c.r[3] < 16)
        sInd.tev[c.r[3]] = indWord(c.r[4] & 3, 0, (c.r[5] & 0xFF) ? GX_ITB_STU : 0, c.r[7], (c.r[6] & 0xFF) ? GX_ITW_0 : 0,
                                   (c.r[6] & 0xFF) ? GX_ITW_0 : 0, 0, 0, 0);
    if (indOff()) return GXSetTevDirect(GXTevStageID(c.r[3]));
    GXSetTevIndWarp(GXTevStageID(c.r[3]), GXIndTexStageID(c.r[4]), GXBool(c.r[5]), GXBool(c.r[6]),
                    GXIndTexMtxID(c.r[7]));
}
GXHLE(GXSetTevIndirect) {
    if (c.r[3] < 16)
        sInd.tev[c.r[3]] = indWord(c.r[4] & 3, c.r[5] & 3, c.r[6] & 7, c.r[7] & 15, c.r[8] & 7, c.r[9] & 7,
                                   c.r[10] & 1, stackArg(c, 0) & 1, stackArg(c, 1) & 3);
    if (indOff()) return GXSetTevDirect(GXTevStageID(c.r[3]));
    GXSetTevIndirect(GXTevStageID(c.r[3]), GXIndTexStageID(c.r[4]), GXIndTexFormat(c.r[5]),
                     GXIndTexBiasSel(c.r[6]), GXIndTexMtxID(c.r[7]), GXIndTexWrap(c.r[8]), GXIndTexWrap(c.r[9]),
                     GXBool(c.r[10]), GXBool(stackArg(c, 0)), GXIndTexAlphaSel(stackArg(c, 1)));
}

// --- pixel engine ---------------------------------------------------------------------------------

GXHLE(GXSetColorUpdate) { GXSetColorUpdate(GXBool(c.r[3])); }
GXHLE(GXSetAlphaUpdate) { GXSetAlphaUpdate(GXBool(c.r[3])); }
GXHLE(GXSetZMode) { GXSetZMode(GXBool(c.r[3]), GXCompare(c.r[4]), GXBool(c.r[5])); }
GXHLE(GXSetPixelFmt) {
    gxAudit("GXSetPixelFmt %u z=%u", c.r[3], c.r[4]); GXSetPixelFmt(GXPixelFmt(c.r[3]), GXZFmt16(c.r[4])); }
GXHLE(GXSetDither) {
    gxAudit("GXSetDither %u", c.r[3]); GXSetDither(GXBool(c.r[3])); }
GXHLE(GXSetZCompLoc) {
    gxAudit("GXSetZCompLoc %u", c.r[3]); GXSetZCompLoc(GXBool(c.r[3])); }
GXHLE(GXSetBlendMode) {
    if (c.r[3] == GX_BM_LOGIC)  // logic ops: where the game uses them (Aurora 0026)
        gxAudit("GXSetBlendMode %u %u %u %u from %s", c.r[3], c.r[4], c.r[5], c.r[6], guestChain(c).c_str());
    else
        gxAudit("GXSetBlendMode %u %u %u %u", c.r[3], c.r[4], c.r[5], c.r[6]);
    GXSetBlendMode(GXBlendMode(c.r[3]), GXBlendFactor(c.r[4]), GXBlendFactor(c.r[5]), GXLogicOp(c.r[6]));
}
GXHLE(GXSetDstAlpha) {
    // Ghost pass (fn_80060E70 pass 0x30): r21 is the ghost about to be drawn with this alpha
    // (ghost_lights.cpp: only ghosts actually drawn visibly give light).
    if (c.lr == VA(0x800611C4) || c.lr == VA(0x800611D4)) ghostlights::onGhostDrawn(c.r[21], c.r[4] & 0xFF);
    GXSetDstAlpha(GXBool(c.r[3]), u8(c.r[4]));
}
GXHLE(GXSetFog) {
    gxAudit("GXSetFog type=%u start=%.1f end=%.1f near=%.1f far=%.1f", c.r[3], farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4));
    const u32 at = interp::mark();
    GXSetFog(GXFogType(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE(GXSetFogRangeAdj) {
    gxAudit("GXSetFogRangeAdj en=%u center=%u", c.r[3], c.r[4]);
    GXFogAdjTable t{};
    if (c.r[5])
        for (int i = 0; i < 10; i++) t.r[i] = u16(rd16(c.r[5] + 2 * i));
    GXSetFogRangeAdj(GXBool(c.r[3]), u16(c.r[4]), &t);
}
GXHLE(GXInitFogAdjTable) {
    const u32 table = c.r[3];
    const u16 width = u16(c.r[4]);
    const Mtx<4, 4> p(c.r[5]);
    f32 nearZ, sideX;
    if (p.m[3][3] == 0.0f) {
        nearZ = p.m[2][3] / (p.m[2][2] - 1.0f);
        sideX = nearZ / p.m[0][0];
    } else {
        sideX = 1.0f / p.m[0][0];
        nearZ = 1.73205f * sideX;
    }
    const f32 iw = 2.0f / width;
    for (u32 i = 0; i < 10; i++) {
        const f32 xi = f32((i + 1) << 5) * iw * sideX;
        const f32 range = std::sqrt(1.0f + (xi * xi) / (nearZ * nearZ));
        wr16(table + 2 * i, u32(range * 256.0f) & 0xFFF);
    }
}

// --- copies ---------------------------------------------------------------------------------------

// Current texture-copy setup (LM_COPY_LOG and the LM_FRAME_DUMP restore), copies by frame for the log.
static struct { u32 x, y, w, h, dw, dh, fmt, mip; } sCopyDiag;
static const bool sCopyLog = std::getenv("LM_COPY_LOG") != nullptr;
u32 copyDispCount() { return sDiagFrame; }
struct DiagCopy { u32 dest, size, fmt, w, h, frame; };
static std::vector<DiagCopy> sDiagCopies;
void diagTexInit(const char* kind, u32 obj, u32 image, u32 w, u32 h, u32 fmt, u32 lr) {
    if (!sCopyLog) return;
    const u32 im = phys(image);
    const u32 sz = GXGetTexBufferSize(u16(w), u16(h), fmt, GX_FALSE, 0);
    for (const auto& d : sDiagCopies) {
        if (im < d.dest + d.size && d.dest < im + sz) {
            log("[copydiag] f%u %s obj=%08X img=%08X %ux%u fmt=%u size=%X lr=%08X  overlaps copy %08X %ux%u fmt=%u (f%u)%s",
                sDiagFrame, kind, obj, image, w, h, fmt, sz, lr, d.dest, d.w, d.h, d.fmt, d.frame,
                (im == d.dest && w == d.w && h == d.h && fmt == d.fmt) ? " [same]" : " [DIFFERENT]");
        }
    }
}
GXHLE(GXSetCopyClear) { GXSetCopyClear(color(c.r[3]), c.r[4]); }
GXHLE(GXSetDispCopySrc) {
    gxAudit("GXSetDispCopySrc %u %u %u %u", c.r[3], c.r[4], c.r[5], c.r[6]);
    if (c.r[6]) sDisplayCopy.efbHeight = u16(c.r[6]);
    GXSetDispCopySrc(u16(c.r[3]), u16(c.r[4]), u16(c.r[5]), u16(c.r[6]));
}
GXHLE(GXSetDispCopyDst) {
    gxAudit("GXSetDispCopyDst %u %u", c.r[3], c.r[4]);
    GXSetDispCopyDst(u16(c.r[3]), u16(c.r[4]));
}
GXHLE(GXSetDispCopyYScale) {
    // Returns the number of XFB lines the display copy writes for the copy source height
    // (the SDK's formula for the 9-bit reciprocal scale register).
    gxAudit("GXSetDispCopyYScale %.4f", farg(c, 1));
    GXSetDispCopyYScale(farg(c, 1));
    const u32 iScale = u32(256.0f / farg(c, 1)) & 0x1FF;
    const u32 efbHeight = sDisplayCopy.efbHeight;
    u32 lines = iScale ? (efbHeight - 1) * 256 / iScale + 1 : efbHeight;
    if (iScale > 128 && iScale < 256) {
        u32 s = iScale;
        while ((s & 1) == 0) s >>= 1;
        if (efbHeight % s == 0) lines++;
    }
    c.r[3] = std::min<u32>(lines, 1024);
}
GXHLE(GXSetDispCopyGamma) {
    gxAudit("GXSetDispCopyGamma %u", c.r[3]);
    sDisplayCopy.gamma = u8(std::min<u32>(c.r[3], 2));
    GXSetDispCopyGamma(GXGamma(c.r[3]));
}
GXHLE(GXSetCopyClamp) {
    gxAudit("GXSetCopyClamp %u", c.r[3]);
    sDisplayCopy.clamp = u8(c.r[3] & 3);
    GXSetCopyClamp(GXFBClamp(c.r[3] & 3));  // texture copies: copy filter edge lines (patch 0018)
}
GXHLE(GXSetCopyFilter) {
    if (c.r[6]) gxAudit("GXSetCopyFilter aa=%u vf=%u vfilter=%u,%u,%u,%u,%u,%u,%u", c.r[3], c.r[5], rd8(c.r[6]), rd8(c.r[6] + 1), rd8(c.r[6] + 2), rd8(c.r[6] + 3), rd8(c.r[6] + 4), rd8(c.r[6] + 5), rd8(c.r[6] + 6));
    u8 pattern[12][2] = {};
    u8 vfilter[7] = {};
    if (c.r[4]) std::memcpy(pattern, hostp(c.r[4]), sizeof(pattern));
    if (c.r[6]) std::memcpy(vfilter, hostp(c.r[6]), sizeof(vfilter));
    // With vf off the SDK loads the pass-through taps {0, 0, 21, 22, 21, 0, 0}.
    static constexpr u8 kNoFilter[7] = {0, 0, 21, 22, 21, 0, 0};
    for (int i = 0; i < 7; i++) sDisplayCopy.vfilter[i] = c.r[5] && c.r[6] ? u8(vfilter[i] & 0x3F) : kNoFilter[i];
    GXSetCopyFilter(GXBool(c.r[3]), pattern, GXBool(c.r[5]), vfilter);
}
GXHLE(GXCopyDisp) {
    sDiagFrame++;
    if (void* dump = dropDraw() ? nullptr : efbcopy::dumpTarget(sDiagFrame)) {
        // LM_FRAME_DUMP: copy the finished EFB through the copy-to-RAM path, then restore
        // the game's texture-copy setup. The dump is the raw EFB: no copy filter (texture
        // copies apply it, GX_AURORA_ACCURACY_COPY_FILTER), then the game's taps again.
        u8 pattern[12][2] = {};
        u8 taps[7];
        std::memcpy(taps, sDisplayCopy.vfilter, sizeof(taps));
        GXSetCopyFilter(GX_FALSE, pattern, GX_FALSE, nullptr);
        GXSetTexCopySrc(0, 0, 640, 480);
        GXSetTexCopyDst(640, 480, GX_TF_RGBA8, GX_FALSE);
        const u32 at = interp::mark();
        GXCopyTex(dump, GX_FALSE);
        interp::noteDump(at, dump, sDiagFrame);
        GXSetCopyFilter(GX_FALSE, pattern, GX_TRUE, taps);
        GXSetTexCopySrc(u16(sCopyDiag.x), u16(sCopyDiag.y), u16(sCopyDiag.w), u16(sCopyDiag.h));
        GXSetTexCopyDst(u16(sCopyDiag.dw), u16(sCopyDiag.dh), GXTexFmt(sCopyDiag.fmt), GXBool(sCopyDiag.mip));
    }
    if (!dropDraw()) GXCopyDisp(hostp(c.r[3]), GXBool(c.r[4]));
    if (frame::presentOnCopy()) frame::present(c, c.r[3], frame::viBlack());  // PAL release (frame.h)
}
GXHLE(GXSetTexCopySrc) {
    sCopyDiag.x = c.r[3]; sCopyDiag.y = c.r[4]; sCopyDiag.w = c.r[5]; sCopyDiag.h = c.r[6];
    traceNote("copy src (%u,%u %ux%u)", c.r[3], c.r[4], c.r[5], c.r[6]);
    GXSetTexCopySrc(u16(c.r[3]), u16(c.r[4]), u16(c.r[5]), u16(c.r[6]));
}
GXHLE(GXSetTexCopyDst) {
    sCopyDiag.dw = c.r[3]; sCopyDiag.dh = c.r[4]; sCopyDiag.fmt = c.r[5]; sCopyDiag.mip = c.r[6];
    GXSetTexCopyDst(u16(c.r[3]), u16(c.r[4]), GXTexFmt(c.r[5]), GXBool(c.r[6]));
}
GXHLE(GXCopyTex) {
    HUD_TRACE("copytex src(%u,%u %ux%u) dst %ux%u fmt %u -> %08X %s", sCopyDiag.x, sCopyDiag.y, sCopyDiag.w, sCopyDiag.h,
              sCopyDiag.dw, sCopyDiag.dh, sCopyDiag.fmt, c.r[3], guestChain(c).c_str());
    if (sCopyLog) {
        const u32 sz = GXGetTexBufferSize(u16(sCopyDiag.dw), u16(sCopyDiag.dh), sCopyDiag.fmt, GX_FALSE, 0);
        log("[copydiag] f%u COPY dest=%08X src=(%u,%u %ux%u) dst=%ux%u fmt=%u mip=%u clear=%u size=%X lr=%08X%s",
            sDiagFrame, c.r[3], sCopyDiag.x, sCopyDiag.y, sCopyDiag.w, sCopyDiag.h, sCopyDiag.dw, sCopyDiag.dh,
            sCopyDiag.fmt, sCopyDiag.mip, c.r[4], sz, c.lr, dropDraw() ? " DROPPED" : "");
        bool found = false;
        for (auto& d : sDiagCopies)
            if (d.dest == phys(c.r[3])) { d = {phys(c.r[3]), sz, sCopyDiag.fmt, sCopyDiag.dw, sCopyDiag.dh, sDiagFrame}; found = true; }
        if (!found) sDiagCopies.push_back({phys(c.r[3]), sz, sCopyDiag.fmt, sCopyDiag.dw, sCopyDiag.dh, sDiagFrame});
    }
    if (dropDraw()) return;
    if (!isCopyDest(phys(c.r[3]))) {
        if (sCopyDests.size() >= 64) sCopyDests.erase(sCopyDests.begin());
        sCopyDests.push_back(phys(c.r[3]));
    }
    interp::efbCopied(c.r[3], GXGetTexBufferSize(u16(sCopyDiag.dw), u16(sCopyDiag.dh), sCopyDiag.fmt, GX_FALSE, 0));
    efbcopy::guestCopy(c.r[3], sCopyDiag.dw, sCopyDiag.dh, sCopyDiag.fmt);
    GXCopyTex(hostp(c.r[3]), GXBool(c.r[4]));
}

}  // namespace recomp
