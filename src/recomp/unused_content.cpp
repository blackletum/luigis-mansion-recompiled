// Unused content still present in the retail code, re-enabled as F1 > Enhancements options.
// Both are hooks (tools/recomp/hooks.txt) that run the original function and only steer which
// of the game's own code paths it takes. Findings and addresses: docs/UNUSED_CONTENT.md.
#include "recomp/enhanced_lighting.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <dolphin/pad.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp: guest frame number (LM_FRAME_DUMP numbering)
u16 padButtons();  // hle_pad.cpp: port 1 buttons as last handed to the game
void unusedAssetsFrame(Ctx& c);  // unused_assets.cpp: per-frame work (nozzles)
void testStateFrame(Ctx& c);     // test_state.cpp: LM_TEST_STATE presets
void unusedModelsFrame();        // unused_models.cpp: green Toad

namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

// Game Boy Horror HUD screen mode (lbl_804D846C). Retail only ever sets 4 (normal) and 3
// (E. Gadd's call, fn_8003EA78/fn_8003EA84). Mode 2 is the leftover live camera: the HUD
// update reports the screen rectangle to the renderer (fn_80010EBC), which then renders the
// second view (first-person camera fn_80021DDC) into it; static noise covers the screen
// while it is off-screen.
#define kGbhScreenMode VA(0x804D846C)
constexpr u32 kGbhModeCamera = 2;
constexpr u32 kGbhModeNormal = 4;
constexpr u32 kGbhModeClock = 1;  // cut E3 clock + radar

bool sGbhCamOn = envFlag("LM_GBH_CAM");  // test aid: start with the camera on
u16 sPrevButtons = 0;
bool sGbhClockOn = envFlag("LM_GBH_CLOCK");  // test aid: start with the clock on

}  // namespace

// Game Boy Horror HUD update (once per game frame while the HUD is up).
HLE(fn_8003CFC0) {
    static const bool sForce = envFlag("LM_GBH_CAM");
    const bool enabled = sForce || port::settings().enhancements.gbhCamera;
    const u16 buttons = padButtons();
    if (!enabled) {
        sGbhCamOn = false;
    } else if ((buttons & PAD_BUTTON_UP) && !(sPrevButtons & PAD_BUTTON_UP)) {
        sGbhCamOn = !sGbhCamOn;
        if (sGbhCamOn) sGbhClockOn = false;
        log("[unused] Game Boy Horror camera %s", sGbhCamOn ? "on" : "off");
    }
    // Cut E3 clock/radar screen (GBH mode 1): D-pad Left; exclusive with the camera.
    if (!port::settings().enhancements.gbhClock && !envFlag("LM_GBH_CLOCK")) {
        sGbhClockOn = false;
    } else if ((buttons & PAD_BUTTON_LEFT) && !(sPrevButtons & PAD_BUTTON_LEFT)) {
        sGbhClockOn = !sGbhClockOn;
        if (sGbhClockOn) sGbhCamOn = false;
        log("[unused] Game Boy Horror clock %s", sGbhClockOn ? "on" : "off");
    }
    setPatch(3, sGbhClockOn);
    // Unused ghost tracker: D-pad Down cycles off -> far mode -> close mode (patch switches 1/2).
    static u32 sTracker = 0;
    if (!port::settings().enhancements.gbhTracker) {
        sTracker = 0;
    } else if ((buttons & PAD_BUTTON_DOWN) && !(sPrevButtons & PAD_BUTTON_DOWN)) {
        sTracker = (sTracker + 1) % 3;
        log("[unused] Game Boy Horror ghost tracker: %s", sTracker == 0 ? "off" : sTracker == 1 ? "far mode" : "close mode");
    }
    setPatch(1, sTracker == 1);
    setPatch(2, sTracker == 2);
    sPrevButtons = buttons;
    static const bool sShowHud = [] {
        const char* v = std::getenv("LM_GBH_CAM");
        return v && *v == '2';
    }();
    if (sShowHud) {  // test aid (LM_GBH_CAM=2): GBH HUD up before Luigi has the Game Boy Horror
        static const u32 player = guestSymbol("fn_800AD39C");
        const Ctx saved = c;
        const u32 p = guestCall(c, player, 0);
        c = saved;
        if (p >= 0x80000000u && p < 0x81800000u) wr8(p + 0x55, 1);
    }
    // Screen mode: camera (2) / clock (1) / normal (4); E. Gadd's call screen (3) is left alone.
    const u32 mode = rd32(kGbhScreenMode);
    if (mode == kGbhModeNormal || mode == kGbhModeCamera || mode == kGbhModeClock) {
        const u32 want = sGbhCamOn ? kGbhModeCamera : sGbhClockOn ? kGbhModeClock : kGbhModeNormal;
        if (mode != want) wr32(kGbhScreenMode, want);
    }
    callOriginal(c, "fn_8003CFC0");
    testStateFrame(c);
    unusedAssetsFrame(c);
    unusedModelsFrame();
    // Test aid: LM_TELEPORT=x,y,z,frames moves Luigi (actor slot 0, position +0x44) there once,
    // `frames` HUD updates into gameplay (used to reach the 2F doors for screenshots).
    static const char* sTp = std::getenv("LM_TELEPORT");
    if (sTp && *sTp) {
        static int sFrames = 0;
        float x = 0, y = 0, z = 0;
        int at = 300;
        std::sscanf(sTp, "%f,%f,%f,%d", &x, &y, &z, &at);
        if (++sFrames == at) {
            const u32 actor = rd32(VA(0x803D48A0));
            if (actor >= 0x80000000u && actor < 0x81800000u) {
                wr32(actor + 0x44, std::bit_cast<u32>(x));
                wr32(actor + 0x48, std::bit_cast<u32>(y));
                wr32(actor + 0x4C, std::bit_cast<u32>(z));
                log("[unused] teleported Luigi to %.0f,%.0f,%.0f", x, y, z);
            }
        }
    }
    // Test aid (judder sweep, tools/interp/judder_sweep.py): LM_ROOM_TOUR=<first frame>,<frames per
    // room>,<room>[:<room>...] puts Luigi in each listed room in turn (the room table's centre,
    // `<room>@x,z` for a spot of its own), guest frame numbers as in LM_FRAME_DUMP.
    static const char* sTour = std::getenv("LM_ROOM_TOUR");
    if (sTour && *sTour) {
        static u32 sFirst = 0, sPer = 0;
        static std::vector<std::string> sStops;
        if (!sPer) {
            const std::string s = sTour;
            std::sscanf(s.c_str(), "%u,%u", &sFirst, &sPer);
            const size_t c2 = s.find(',', s.find(',') + 1);
            for (size_t p = c2 == std::string::npos ? s.size() : c2 + 1; p < s.size();) {
                const size_t e = std::min(s.find(':', p), s.size());
                sStops.push_back(s.substr(p, e - p));
                p = e + 1;
            }
            if (!sPer) sPer = 1;
        }
        const u32 f = copyDispCount();
        static u32 sLastStop = ~0u;
        if (f >= sFirst && (f - sFirst) / sPer < sStops.size() && (f - sFirst) / sPer != sLastStop) {
            const u32 i = (f - sFirst) / sPer;
            const u32 actor = rd32(VA(0x803D48A0));
            const u32 rooms = rd32(VA(0x804D8170));
            int room = -1;
            float px = 0, pz = 0;
            const int n = std::sscanf(sStops[i].c_str(), "%d@%f,%f", &room, &px, &pz);
            if (actor >= 0x80000000u && actor < 0x81800000u && (rooms >> 28) == 8 && room >= 0 && room < 80) {
                sLastStop = i;
                const u32 b = rooms + u32(room) * 0x30;
                float lo[3], hi[3];
                for (int k = 0; k < 3; k++) lo[k] = float(s32(rd32(b + 8 + 4 * k))), hi[k] = float(s32(rd32(b + 0x14 + 4 * k)));
                if (!(hi[0] > lo[0] && hi[2] > lo[2] && hi[0] - lo[0] < 20000.0f && hi[2] - lo[2] < 20000.0f)) {
                    log("[tour] f%u stop %u room %d: no bounds", f, i, room);
                } else {
                    const float x = n == 3 ? px : (lo[0] + hi[0]) * 0.5f, z = n == 3 ? pz : (lo[2] + hi[2]) * 0.5f, y = lo[1] + 60.0f;
                    wr32(actor + 0x44, std::bit_cast<u32>(x));
                    wr32(actor + 0x48, std::bit_cast<u32>(y));
                    wr32(actor + 0x4C, std::bit_cast<u32>(z));
                    log("[tour] f%u stop %u room %d at %.0f,%.0f,%.0f (bounds %.0f %.0f %.0f .. %.0f %.0f %.0f)", f, i, room, x, y,
                        z, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
                }
            }
        }
        // Where Luigi ended up, half-way through each stop.
        if (f >= sFirst && (f - sFirst) % sPer == sPer / 2 && (f - sFirst) / sPer < sStops.size()) {
            const u32 actor = rd32(VA(0x803D48A0));
            if (actor >= 0x80000000u && actor < 0x81800000u)
                log("[tour] f%u luigi %.0f %.0f %.0f room %d", f, std::bit_cast<f32>(rd32(actor + 0x44)),
                    std::bit_cast<f32>(rd32(actor + 0x48)), std::bit_cast<f32>(rd32(actor + 0x4C)),
                    s32(rd32(VA(0x803A3CA0) + 0xC) >> 24));
        }
    }
}

// Actor draw pass. r3 selects the pass: (r3 & 0x30) == 0x10 draws normal actors into the
// scene, 0x30 draws ghosts (type flag 8) into the separate ghost layer that is later added
// back at half resolution weighted by the ghost's alpha (fn_8000C2D0, fn_8005F280). Pass 0
// (used by the never-shipped Game Boy Horror view) draws every actor, ghosts included,
// straight into the scene with their ghost lighting and alpha: the early, more solid look.
// The ghost pass is also where enhanced lighting tags draws as ghosts (enhanced_lighting.h).
HLE(fn_80060E70) {
    static const bool sForce = envFlag("LM_EARLY_GHOSTS");
    const u32 pass = c.r[3] & 0x30;
    struct GhostScope {
        bool prev = enh::ghostPass();
        explicit GhostScope(bool on) { enh::setGhostPass(on); }
        ~GhostScope() { enh::setGhostPass(prev); }
    } ghostScope(pass == 0x30);
    struct CharScope {
        bool prev = enh::characterPass();
        explicit CharScope(bool on) { enh::setCharacterPass(on); }
        ~CharScope() { enh::setCharacterPass(prev); }
    } charScope(pass != 0x30);
    // Characters (Luigi, ghosts, actors) appear and vanish at the game frame with frame
    // interpolation, never faded: Luigi's damage blink stays an on/off blink (interp.h).
    struct SnapScope {
        SnapScope() { interp::pushSnapScope(); }
        ~SnapScope() { interp::popSnapScope(); }
    } snapScope;
    if (!sForce) return  // dev-only experiment (LM_EARLY_GHOSTS=1); the menu option is the beta lighting below
        callOriginal(c, "fn_80060E70");
    if (pass == 0x10) {
        c.r[3] &= ~0x30u;  // normal actors + ghosts, drawn directly
    } else if (pass == 0x30) {
        c.r[3] = (c.r[3] & ~0x30u) | 0x20;  // no case for 0x20: the ghost layer stays empty
    }
    callOriginal(c, "fn_80060E70");
}

// Early ghost lighting: the known cheat for the pre-release ghost look patches the channel
// setup call at 0x80058290 in fn_800581E8 (the ghost lighting function): light mask 0x80
// (light 7) -> 0x40 (light 6) and attenuation function 0 -> 2. fn_8005A5D4 takes the
// GXSetChanCtrl-style arguments (r7 = light mask, r9 = attenuation), so adjust them only
// for that call site (the European fn_800581E8 is identical; VA() maps the call site).
HLE(fn_8005A5D4) {
    if (c.lr == VA(0x80058290) + 4 && port::settings().enhancements.earlyGhostLighting) {
        c.r[7] = 0x40;
        c.r[9] = 2;
    }
    callOriginal(c, "fn_8005A5D4");
}

}  // namespace recomp
