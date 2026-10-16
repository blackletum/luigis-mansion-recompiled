// Co-op improvements on top of the unused 2-player mode (coop.cpp): F1 > Enhancements >
// Unused content "Co-op improvements", off by default. Docs: docs/UNUSED_CONTENT.md.
#include "recomp/coop.h"
#include "recomp/coop_test.h"

#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/skip.h"
#include "recomp/version.h"
#include "recomp/widescreen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace recomp::coop {
namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
// LM_COOP_BISECT=<hex mask> (test aid): turns single improvements off. 1 event hold, 2 follow,
// 10 event hold without the hide bits, 20 without keeping player 2 at player 1, 40 shared progress,
// 4 player 2 camera, 8 view layout, 80 the player an object deals with, 100 player 2's suction of
// objects pulled to the nozzle.
bool off(u32 bit) {
    static const u32 sMask = std::getenv("LM_COOP_BISECT") ? u32(std::strtoul(std::getenv("LM_COOP_BISECT"), nullptr, 16)) : 0;
    return (sMask & bit) != 0;
}

u32 sLightOwner = 0;  // Luigi whose flashlight update (fn_800850B4) is running
u32 sMsg2[2] = {};
u32 sCached[2] = {};  // the players as of the last game-scene frame (cachedPlayer)
void mergeProgress(u32 p1, u32 p2);  // shared progress, below
bool sGbhView = false;  // the Game Boy Horror search view is up (below)

// Player records of the view setup (lbl_803A3CA0, 0x28 per player): +0 position, +0xC the
// room number in its top byte (from the player's room, fn_800E9750(i) +0xB4).
#define kViewPlayers VA(0x803A3CA0)
u32 roomOf(int idx) { return rd8(kViewPlayers + 0x28 * idx + 0xC); }
u32 sLastRoom[2] = {~0u, ~0u};

// Ghost AI objects (EnThought, +0x808 = actor of a ghost type) that got a message this map;
// message 3 (capture end) removes them.
std::vector<u32> sGhosts;
std::vector<u32> sObjects;  // AI objects of non-ghost characters that got a message this map
int sAsPlayer2 = 0;  // > 0: game code runs as player 2 (player index 0 reads as 1; below)
std::vector<std::pair<u32, u32>> sPulledByP2;  // objects player 2's suction pulls: zako, game frame
void retargetObjects(Ctx& c);  // below

// Character type flags (type table lbl_8033B590, 0x1C per type, u16 at +8; fn_8006A3BC): 8 = ghost.
#define kTypeTable VA(0x8033B590)
bool isGhost(u32 actor) { return gu::inMem1(actor) && (rd16(kTypeTable + 0x1C * rd32(actor + 0x3C) + 8) & 8); }

// LM_COOP_GHOST_TEST=<from>,<to> (guest frames, test aid): keeps the first live ghost 220
// units in front of player 2 (Luigi +0xF48 = facing), so scripted input on port 2 can flash
// and vacuum it.
struct GhostTest {
    u32 from = 0, to = 0;
};
const GhostTest& ghostTest() {
    static const GhostTest t = [] {
        GhostTest r;
        if (const char* v = std::getenv("LM_COOP_GHOST_TEST")) {
            char* e = nullptr;
            r.from = u32(std::strtoul(v, &e, 10));
            if (e && *e == ',') r.to = u32(std::strtoul(e + 1, nullptr, 10));
        }
        return r;
    }();
    return t;
}

}  // namespace

namespace {
bool sEventHoldReset = false;
}
void onMapSetupFixes(Ctx&) {
    sGhosts.clear();
    cooptest::onMapSetup();
    sObjects.clear();
    sPulledByP2.clear();
    sLastRoom[0] = sLastRoom[1] = ~0u;
    // Player 2 enters every map with player 1's health: player 2's record (lbl_803D5CD0 + 0x16C,
    // +0 health, +2 maximum; read by the player creation) is never reset by the game, so after a
    // game over player 2 started the next game with the 1 point they had left.
    const u32 rec0 = VA(0x803D5CD0), rec1 = rec0 + 0x16C;
    if (fixes()) wr32(rec1, rd32(rec0));
    sEventHoldReset = true;  // the players are new: nothing is held
    sGbhView = false;         // and camera 1 is player 2's (fn_800204B4)
    sCached[0] = sCached[1] = 0;
}

namespace {
// Puts player 2 where player 1 stands (facing the same way), e.g. after player 1 went through a
// door (the game only follows player 1 into the next room) or after an event; the players'
// collision then pushes them apart. A spot beside or behind player 1 is not safe: behind put
// player 2 back in the doorway player 1 had come through (the room before), and beside player 1
// at the foyer's front door is off the floor (player 2 fell into the basement), although the
// room boxes (fn_80017ADC) say it is in the foyer.
void bringP2(Ctx&, u32 p1, u32 p2) {
    const float fx = gu::rdf(p1 + 0xF48), fz = gu::rdf(p1 + 0xF50);
    gu::wrf(p2 + 0x44, gu::rdf(p1 + 0x44) - fz * 2.0f);  // 2 units to the side: a direction to part
    gu::wrf(p2 + 0x48, gu::rdf(p1 + 0x48));
    gu::wrf(p2 + 0x4C, gu::rdf(p1 + 0x4C) + fx * 2.0f);
    // Facing the same way (actor yaw +0x88/+0x8A as <LOOK> sets it, Luigi's facing vector
    // +0xF48..+0xF50): both flashlights light the same side of the room.
    wr32(p2 + 0x88, rd32(p1 + 0x88));
    for (u32 o = 0xF48; o < 0xF54; o += 4) wr32(p2 + o, rd32(p1 + o));
}
}  // namespace

u32 player(Ctx& c, int idx) {
    if (!active() && idx > 0) return 0;
    const int as2 = sAsPlayer2;
    sAsPlayer2 = 0;  // the real player idx, also while game code runs as player 2
    const u32 p = gu::call(c, GU_SYM("fn_800E9750__Q24Koga9EnManagerFl"), {u32(idx)});
    sAsPlayer2 = as2;
    return gu::inMem1(p) ? p : 0;
}

// ---- Views ------------------------------------------------------------------------------
// fn_80011150 picks the layout each frame: one view while both players are in the same room
// and close together, else two. The game's own layout is a 640x240 view letterboxed in the
// middle of the screen (shared) and two 640x240 halves on top of each other (split). Every
// one of those is twice as wide as the 4:3 picture the rooms and the camera were made for, so
// the cameras showed the cut walls and the black void beside the room geometry. With the
// improvements the shared view is the whole screen (exactly the one-player picture) and the
// split is side by side: two 320x480 halves, narrower than 4:3 (8:9 on a 16:9 display, drawn
// Hor+ like the full view: ws::splitHalf), so a half never looks past what a single view
// shows. The views' viewports are the camera fields +0xD0..+0xDC (x0, y0, x1, y1), set by
// fn_80011150 before it lets each camera update (fn_80020654: aspect from them, then the
// camera's own update). The view cameras: lbl_803A4C80[0] player 1, [1] player 2, [2] the
// Game Boy Horror's.
#define kCameras VA(0x803A4C80)
#define kViewCount VA(0x804CFF90)  // lbl_804CFF90 (fn_8000ADFC/fn_8000AE04): views drawn
#define kCurScene VA(0x804CFF98)
#define kSceneMainGame VA(0x8021DB68)
#define kEvent VA(0x803D2D40)   // event interpreter (skip.cpp): +0x0C script cursor, 0 when none
#define kMenuOff VA(0x804D8088) // <MENUOFF>: the HUD hidden by an event
#define kCurMapNo VA(0x804D80C8)  // sCurMapNo
#define kCurCamera VA(0x804D8240)  // r13+0x2E0 on the USA game: the camera being updated (fn_8002064C)
#define kGbhMode VA(0x804CFFF0)    // r13-0x7F70: 2 = Game Boy Horror search view (fn_80010F20)
#define kGbhHolder VA(0x804D8180)  // r13+0x220: +0x24 the GBH screen object
namespace {
bool sInViewSetup = false;
bool sEventHold = false;  // an event has taken control: player 2 hidden and following player 1

void setViewport(u32 cam, u32 x0, u32 y0, u32 x1, u32 y1) {
    wr32(cam + 0xD0, x0);
    wr32(cam + 0xD4, y0);
    wr32(cam + 0xD8, x1);
    wr32(cam + 0xDC, y1);
}
}  // namespace

bool player2SetAside() { return fixes() && sEventHold; }

// ---- Game Boy Horror search view ----------------------------------------------------------
// X (search: the Game Boy Horror's first-person view, how hidden Boos are found) shows view 1
// through the GBH camera inside the GBH frame. In one-player mode the per-frame view setup
// (fn_80010F20) does that: two views while the mode r13-0x7F70 is 2, view 0 framed while the GBH
// screen object ([[r13+0x220] +0x24] +0x89) is up, and camera 1 is the GBH camera (fn_800204B4:
// update fn_80021DDC, set up by fn_80021AF4, following player 1). In 2-player mode camera 1 is
// player 2's follow camera and fn_80011150 lays out the players' views, so the search showed a
// third-person picture with the HUD gone. While it is up the one-player layout runs instead and
// camera 1 is turned into the GBH camera, then back into player 2's (fn_80021844 / fn_80021618).
namespace {

u32 sCam1Ac = 0;

bool gbhViewWanted(Ctx& c) {
    const u32 mode = rd32(kGbhMode);
    const u32 g = rd32(kGbhHolder);
    const u32 scr = gu::inMem1(g) ? rd32(g + 0x24) : 0;
    const u8 framed = gu::inMem1(scr) ? rd8(scr + 0x89) : 0;
    static u32 sLastMode = ~0u, sLastFramed = ~0u;
    if (mode != sLastMode || framed != sLastFramed) {
        log("[coop] GBH view mode %u framed %u (guest frame %u)", mode, framed, skip::guestFrames());
        sLastMode = mode;
        sLastFramed = framed;
    }
    return mode == 2 || framed != 0;
}

void setCamera1(Ctx& c, bool gbh) {
    const u32 cam = rd32(kCameras + 4);
    if (!gu::inMem1(cam)) return;
    if (gbh) sCam1Ac = rd32(cam + 0xAC);
    wr32(cam + 0xE8, kViewPlayers + (gbh ? 0 : 0x28));
    wr32(cam + 4, gbh ? VA(0x80021DDC) : VA(0x80021844));
    wr32(cam + 0xC, 0);
    const u32 saved = rd32(kCurCamera);
    wr32(kCurCamera, cam);
    gu::call(c, gbh ? VA(0x80021AF4) : VA(0x80021618));
    wr32(kCurCamera, saved);
    if (!gbh) wr32(cam + 0xAC, sCam1Ac);
}
}  // namespace

bool gbhView() { return fixes() && sGbhView; }

HLE(fn_80011150) {
    if (!fixes() || off(8)) return callOriginal(c, "fn_80011150");
    // The GBH camera finishes its own exit after the frame closes: camera 1 goes back to player 2
    // a few frames later (switched at once, the GBH picture stayed on screen).
    static u32 sGrace = 0;
    bool gbh = gbhViewWanted(c);
    if (gbh) sGrace = 10;
    else if (sGbhView && sGrace > 0) sGrace--, gbh = true;
    if (gbh != sGbhView) {
        setCamera1(c, gbh);
        sGbhView = gbh;
        log("[coop] Game Boy Horror view %s: camera 1 is %s", gbh ? "up" : "down", gbh ? "the GBH camera" : "player 2's again");
    }
    if (gbh) {
        gu::call(c, VA(0x80010F20));  // the one-player layout
        return;
    }
    // During an event the scene is shown in one full view (the event's camera is view 0's):
    // player 2's record is made player 1's so the layout test sees them together. The same in
    // Chauncey's arena (map 10): its camera frames the boss from the arena's side, and view 1's
    // plain follow camera there looked into the void around the floating stage.
    if (sEventHold || rd32(kCurMapNo) == 10) {
        for (u32 o = 0; o < 0x10; o += 4) wr32(kViewPlayers + 0x28 + o, rd32(kViewPlayers + o));
    }
    sInViewSetup = true;
    callOriginal(c, "fn_80011150");
    sInViewSetup = false;
}

HLE(fn_80020654) {
    if (sInViewSetup && fixes()) {
        const u32 cam = rd32(kCurCamera);  // the camera being updated (fn_8002064C)
        const bool split = rd32(kViewCount) >= 2;
        if (cam == rd32(kCameras)) setViewport(cam, 0, 0, split ? 320 : 640, 480);
        else if (split && cam == rd32(kCameras + 4)) setViewport(cam, 320, 0, 640, 480);
    }
    callOriginal(c, "fn_80020654");
}

// ---- Events -----------------------------------------------------------------------------
// The event scripts (doors' close-ups, keys, room clears and chests, E. Gadd's calls, Toads,
// the blackout, ...) only know player 1: <LUIGISTOP>, <WARP>, <POS>, camera paths act on
// player index 0 (fn_800AD54C(0, msg), fn_800E9750(0)). While an event has taken control
// (a script runs with the HUD hidden or player 1 stopped, as skip.cpp tells cutscenes),
// player 2 is set aside the way the scripts set aside other actors (<ACTOR> commands on +0x40,
// fn_80062EE8): BODYOFF (0x4000, not drawn: fn_80060E70 skips 0x6000), COLOFF (0x80), STOP
// (0x10) and SHADOWOFF (0x800), its Poltergust, nozzle and Game Boy Horror actors (+0x7A4,
// +0x7A8, +0x7AC) hidden too, and kept at player 1's position (so it is never left behind when
// the script moves or warps player 1, and the view stays the single full view). When the event
// ends the bits it did not have are cleared again and player 2 stands behind player 1.
namespace {
constexpr u32 kHideBits = 0x4000 | 0x80 | 0x10 | 0x800;
u32 sSavedBits = 0;
u32 sSavedEquip[3] = {};

bool eventControl(u32 p1) {
    if (rd32(kCurScene) != kSceneMainGame || rd32(kEvent + 0x0C) == 0) return false;
    return rd32(kMenuOff) != 0 || (rd32(p1 + 0x40) & 0x10) != 0;
}

void holdP2(Ctx& c, u32 p1, u32 p2, bool on) {
    if (off(0x10)) return;
    if (on) {
        sSavedBits = rd32(p2 + 0x40) & kHideBits;
        wr32(p2 + 0x40, rd32(p2 + 0x40) | kHideBits);
        for (int i = 0; i < 3; i++) {
            const u32 e = rd32(p2 + 0x7A4 + 4 * i);
            sSavedEquip[i] = gu::inMem1(e) ? rd32(e + 0x40) & 0x4000 : 0;
            if (gu::inMem1(e)) wr32(e + 0x40, rd32(e + 0x40) | 0x4000);
        }
    } else {
        wr32(p2 + 0x40, rd32(p2 + 0x40) & ~(kHideBits & ~sSavedBits));
        for (int i = 0; i < 3; i++) {
            const u32 e = rd32(p2 + 0x7A4 + 4 * i);
            if (gu::inMem1(e) && !sSavedEquip[i]) wr32(e + 0x40, rd32(e + 0x40) & ~0x4000u);
        }
        bringP2(c, p1, p2);
    }
}
}  // namespace

// Game-scene view/camera update (fn_800112F8, once per game frame).
u32 cachedPlayer(int idx) { return idx >= 0 && idx < 2 && active() ? sCached[idx] : 0; }

HLE(fn_800112F8) {
    if (sEventHoldReset) {
        sEventHoldReset = false;
        sEventHold = false;
    }
    if (active()) {
        sCached[0] = player(c, 0);
        sCached[1] = player(c, 1);
    }
    if (fixes()) {
        const u32 p1 = player(c, 0), p2 = player(c, 1);
        const bool hold = p1 && p2 && !off(1) && eventControl(p1);
        if (hold != sEventHold) {
            holdP2(c, p1, p2, hold);
            log("[coop] event %s: player 2 %s (guest frame %u)", hold ? "took control" : "ended",
                hold ? "set aside" : "back with player 1", skip::guestFrames());
        }
        sEventHold = hold;
        if (hold && !off(0x20)) {  // with player 1 wherever the script puts them
            for (u32 o = 0x44; o < 0x50; o += 4) wr32(p2 + o, rd32(p1 + o));
        }
        if (p1 && p2 && !off(0x40)) mergeProgress(p1, p2);
        if (p1 && p2) retargetObjects(c);  // the player an object deals with (below)
        // Player 2 downed while player 1 had nothing to share (1 health left): player 2 stays
        // down, and is revived with half of player 1's health once player 1 has more than 1 again
        // (a heart picked up). Player 1 being downed is the game's own game over.
        if (p1 && p2 && !hold && s16(rd16(p2 + 0xFC)) <= 0) {
            const s32 hp1 = s16(rd16(p1 + 0xFC));
            if (hp1 > 1) {
                const s32 give = hp1 / 2;
                wr16(p1 + 0xFC, u16(hp1 - give));
                gu::call(c, GU_SYM("fn_800B9488"), {p2, u32(give)});
                log("[coop] player 2 revived with %d health from player 1 (%d left)", give, hp1 - give);
            }
        }
        // Equipment and medals: Luigi +0x792 event bits (4 Poltergust, 8 Game Boy Horror, the
        // element medals, ...; fn_800B9430 tests them). Player 2 has player 1's, except bit 0
        // (player 1's controller, from the creation flags).
        if (p1 && p2) wr16(p2 + 0x792, u16((rd16(p1 + 0x792) & ~1u) | (rd16(p2 + 0x792) & 1u)));
    }
    callOriginal(c, "fn_800112F8");
    cooptest::frame(c);
    if (!active()) return;
    static const bool sDump = envFlag("LM_COOP_DUMP");
    // LM_COOP_DIFF=<hex addr>,<hex len> (test aid): every 300 game frames, the words of that block
    // that changed since the last report (finding where progress is kept).
    static const auto sDiff = [] {
        std::pair<u32, u32> r{0, 0};
        if (const char* v = std::getenv("LM_COOP_DIFF")) std::sscanf(v, "%x,%x", &r.first, &r.second);
        return r;
    }();
    if (sDiff.second) {
        static std::vector<u32> sPrev;
        static u32 sDiffFrame = 0;
        if (sDiffFrame++ % 300 == 0) {
            const u32 n = sDiff.second / 4;
            if (sPrev.size() == n) {
                int shown = 0;
                for (u32 k = 0; k < n && shown < 40; k++) {
                    const u32 v = rd32(sDiff.first + 4 * k);
                    if (v != sPrev[k]) {
                        log("[coop] diff %08X: %08X -> %08X", sDiff.first + 4 * k, sPrev[k], v);
                        shown++;
                    }
                }
            }
            sPrev.resize(n);
            for (u32 k = 0; k < n; k++) sPrev[k] = rd32(sDiff.first + 4 * k);
        }
    }
    if (sDump) {
        static u32 sFrame = 0;
        if (sFrame++ % 300 == 0) {
            for (int i = 0; i < 2; i++) {
                const u32 p = player(c, i);
                if (!p) continue;
                if (i == 0)
                    log("[coop] guest frame %u, %zu ghosts known, rooms %u/%u, views %u", skip::guestFrames(), sGhosts.size(),
                        roomOf(0), roomOf(1), rd32(kViewCount));
                if (i == 0)
                    for (u32 t : sGhosts) {  // AI state (strategy +0xE) and the ghost's place, health, alpha
                        const u32 st = rd32(t + 0x800), a = rd32(t + 0x808);
                        if (!gu::inMem1(st) || !gu::inMem1(a)) continue;
                        log("[coop]   ghost %08X ai %X pos %.0f,%.0f,%.0f hp %d alpha %u room %u", t, rd16(st + 0xE),
                            gu::rdf(a + 0x44), gu::rdf(a + 0x48), gu::rdf(a + 0x4C), s16(rd16(a + 0xFC)), rd8(a + 0xE3),
                            rd8(a + 0xB4));

                    }
                log("[coop] P%d %08X pos %.0f,%.0f,%.0f hp %d flags %04X mode %u elem %u 40 %08X 7D4 %08X 7D8 %08X 7E0 %08X 7E4 %08X 7A4 %08X 7A8 %08X 7AC %08X",
                    i + 1, p, gu::rdf(p + 0x44), gu::rdf(p + 0x48), gu::rdf(p + 0x4C), s16(rd16(p + 0xFC)), rd16(p + 0x792), rd32(p + 0x1180), rd32(p + 0x1184), rd32(p + 0x40),
                    rd32(p + 0x7D4), rd32(p + 0x7D8), rd32(p + 0x7E0), rd32(p + 0x7E4), rd32(p + 0x7A4), rd32(p + 0x7A8),
                    rd32(p + 0x7AC));

            }
        }
    }
    if (fixes()) {
        const u32 p1 = player(c, 0), p2 = player(c, 1);
        // Player 2's stick moves relative to the camera of the view player 2 looks at (Luigi
        // +0x798, from the creation params, which give every player camera 0): view 1's in the
        // split screen, view 0's while both share it.
        if (p2 && !off(4)) wr32(p2 + 0x798, rd32(kCameras + (rd32(kViewCount) >= 2 ? 4 : 0)));
        const u32 r1 = roomOf(0), r2 = roomOf(1);
        if (p1 && p2 && !sEventHold && !off(2) && r1 != sLastRoom[0] && sLastRoom[0] != ~0u && r1 != r2) {
            bringP2(c, p1, p2);
            log("[coop] player 1 entered room %u: player 2 (room %u) follows", r1, r2);
        }
        sLastRoom[0] = r1;
        sLastRoom[1] = r2;
    }
    // LM_COOP_SETHP=<guest frame>:<player 1|2>:<health>[,...] (test aid): sets a player's health at
    // that frame through Luigi's own health setter (fn_800B9488, the call every damage source makes).
    static const auto sSetHp = [] {
        std::vector<std::array<u32, 3>> v;
        if (const char* e = std::getenv("LM_COOP_SETHP"))
            for (const char* q = e; *q;) {
                std::array<u32, 3> t{};
                if (std::sscanf(q, "%u:%u:%u", &t[0], &t[1], &t[2]) == 3) v.push_back(t);
                const char* comma = std::strchr(q, ',');
                if (!comma) break;
                q = comma + 1;
            }
        return v;
    }();
    for (const auto& t : sSetHp)
        if (t[0] == skip::guestFrames())
            if (const u32 p = player(c, t[1] == 2 ? 1 : 0)) {
                log("[coop] LM_COOP_SETHP: player %u health %d -> %u", t[1], s16(rd16(p + 0xFC)), t[2]);
                gu::call(c, GU_SYM("fn_800B9488"), {p, t[2] & 0xFFFF});
            }
    const GhostTest& gt = ghostTest();
    const u32 now = skip::guestFrames();
    if (gt.to && now >= gt.from && now < gt.to && !sGhosts.empty()) {
        static const int sTarget = [] {
            const char* v = std::getenv("LM_COOP_GHOST_TARGET");  // 1 or 2 (default)
            return v && *v == '1' ? 0 : 1;
        }();
        const u32 p2 = player(c, sTarget);
        u32 g = 0;
        for (u32 t : sGhosts) {  // first ghost that is visible (alpha +0xE3)
            const u32 a = rd32(t + 0x808);
            if (isGhost(a) && rd8(a + 0xE3)) {
                g = a;
                break;
            }
        }
        if (p2 && gu::inMem1(g)) {
            const float fx = gu::rdf(p2 + 0xF48), fz = gu::rdf(p2 + 0xF50);
            gu::wrf(g + 0x44, gu::rdf(p2 + 0x44) + fx * 220.0f);
            gu::wrf(g + 0x48, gu::rdf(p2 + 0x48) + 100.0f);
            gu::wrf(g + 0x4C, gu::rdf(p2 + 0x4C) + fz * 220.0f);
            // LM_COOP_GHOST_HP0=1: and with no health left, so the vacuum's capture check
            // (fn_800849EC) starts the capture right away (no flash needed).
            static const bool sHp0 = envFlag("LM_COOP_GHOST_HP0");
            if (sHp0) wr16(g + 0xFC, 0);
            static u32 sN = 0;
            if (sN++ % 60 == 0) log("[coop] ghost test: ghost %08X in front of P2 (facing %.2f,%.2f)", g, fx, fz);
        }
    }
}

// ---- Shared progress ---------------------------------------------------------------------
// What a Luigi collects is kept in that Luigi (fn_800AEEA8's messages): the captured-ghost list
// (+0x11EC count, +0x11F0 up to 64 entries), money and treasure counters (+0x12F4..+0x1320:
// coins and bills, the seven gem kinds, three more), and the Poltergust's captured-type bits
// (Poltergust +0x7D4, +0x1AC). The "store to record" message copies them into the Luigi's own
// persistent record (+0x79C: lbl_803D5CD0 + 0x16C * player), and only player 1's record is
// shown (Game Boy Horror tally, result screens) and saved, so whatever player 2 caught or picked
// up was lost. Each game frame player 2's gains are moved into player 1 (added, appended,
// ORed) and cleared in player 2: everything counts for the shared progress.
namespace {
void mergeProgress(u32 p1, u32 p2) {
    const u32 n2 = rd32(p2 + 0x11EC);
    if (n2 > 0 && n2 <= 64) {
        u32 n1 = rd32(p1 + 0x11EC);
        for (u32 k = 0; k < n2 && n1 < 64; k++, n1++) wr32(p1 + 0x11F0 + 4 * n1, rd32(p2 + 0x11F0 + 4 * k));
        wr32(p1 + 0x11EC, n1);
        wr32(p2 + 0x11EC, 0);
        log("[coop] player 2's %u captured ghost(s) added to player 1's (%u)", n2, n1);
    }
    for (u32 o = 0x12F4; o <= 0x1320; o += 4) {
        const s32 v = s32(rd32(p2 + o));
        if (v <= 0) continue;
        wr32(p1 + o, u32(s32(rd32(p1 + o)) + v));
        wr32(p2 + o, 0);
        log("[coop] player 2's treasure +%X (%d) added to player 1's (%d)", o, v, s32(rd32(p1 + o)));
    }
    // The records' bit sets (+0xD4..+0x168, set up by fn_80072B84: captured ghost IDs (fn_800AEEA8
    // sets bits through [[Luigi +0x800] +4] = record +0x134) and the other per-ghost/per-object sets
    // next to it) are kept the same in both records (their union): player 1's is the one saved,
    // and player 2's own checks see what player 1 already has. +0x16A (equipment) is left alone.
    const u32 rec1 = rd32(p1 + 0x79C), rec2 = rd32(p2 + 0x79C);
    if (gu::inMem1(rec1) && gu::inMem1(rec2) && rec1 != rec2) {
        for (u32 k = 0xD4; k < 0x168; k++) {
            const u8 a = rd8(rec1 + k), b = rd8(rec2 + k);
            if (a == b) continue;
            if (b & ~a) log("[coop] player 2's record bits %02X at +%X added to player 1's", u8(b & ~a), k);
            wr8(rec1 + k, a | b);
            wr8(rec2 + k, a | b);
        }
    }
    const u32 v1 = rd32(p1 + 0x7D4), v2 = rd32(p2 + 0x7D4);
    if (gu::inMem1(v1) && gu::inMem1(v2) && v1 != v2) {
        const u32 a = rd32(v1 + 0x1AC), b = rd32(v2 + 0x1AC);
        if (a != b) {
            if (b & ~a) log("[coop] player 2's captured ghost types %08X added to player 1's (%08X)", b, a | b);
            wr32(v1 + 0x1AC, a | b);
            wr32(v2 + 0x1AC, a | b);
        }
    }
}
}  // namespace

// ---- Health ------------------------------------------------------------------------------
// fn_800B9488(luigi, hp) sets a Luigi's health. The game is over when player 1's health runs
// out (and player 2 was never meant to have a health of their own). Co-op rule: a player who
// would drop to 0 while the partner still has more than 1 takes half of the partner's health
// instead; the game only ends when both are out.
void onLuigiHealth(Ctx& c) {
    const u32 me = c.r[3];
    const s32 next = s16(c.r[4]);
    if (next > 0 || s16(rd16(me + 0xFC)) <= 0) return;
    const u32 p1 = player(c, 0), p2 = player(c, 1);
    if (!p1 || !p2 || (me != p1 && me != p2)) return;
    const u32 partner = me == p1 ? p2 : p1;
    const s32 php = s16(rd16(partner + 0xFC));
    if (php <= 1) return;
    const s32 give = php / 2;
    wr16(partner + 0xFC, u16(php - give));
    c.r[4] = u32(give);
    log("[coop] player %d was downed: revived with %d health from player %d (%d left)", me == p1 ? 1 : 2, give,
        me == p1 ? 2 : 1, php - give);
}

// ---- HUD --------------------------------------------------------------------------------
// The HUD is drawn by fn_8003E7B0(view) after each view of the frame loop (fn_8000BCE8): view 0
// draws the in-game HUD (fn_8003E3D0), view 1 the GBH screen noise. The 2-player mode skips
// it all (AR patch 46 restores it), so in split screen view 1, drawn after the HUD, covers the
// bottom half where the health meter and the Game Boy Horror are. With the improvements the
// HUD is drawn once after the last view, and player 2 gets a health meter of their own.
namespace {
#define kViewCount VA(0x804CFF90)   // lbl_804CFF90, fn_8000ADFC
#define kHpShown VA(0x804D8490)     // s16 health shown by the meter (eased toward Luigi's)
#define kHpShown2 VA(0x804D8492)    // s16 second meter value (damage flash)
#define kHeartPhase VA(0x804D84B0)  // heartbeat animation phase
#define kTextX VA(0x804D8496)
#define kTextY VA(0x804D8498)  // s16 digits position
// Heart sprites (+0/+4: s32 x, y; 0x18 apart): lbl_803CE698 +0x30 on the USA game; the European
// object (lbl_803BBA08) starts with them.
#define kHeartSprites (VA(0x803CE698) + VOFF(0x30, 0))
#define kMeterArg VA(0x804D84CC)                    // fn_800417F8's argument
int sGbhShift = 0;

struct MeterOffset {
    s32 dx, dy;
    u16 text[2];
    u32 sprite[3][2];
    MeterOffset(s32 x, s32 y) : dx(x), dy(y) {
        text[0] = rd16(kTextX);
        text[1] = rd16(kTextY);
        wr16(kTextX, u16(s16(text[0]) + dx));
        wr16(kTextY, u16(s16(text[1]) + dy));
        for (int i = 0; i < 3; i++) {
            const u32 sp = kHeartSprites + 0x18 * i;
            sprite[i][0] = rd32(sp);
            sprite[i][1] = rd32(sp + 4);
            wr32(sp, u32(s32(sprite[i][0]) + dx));
            wr32(sp + 4, u32(s32(sprite[i][1]) + dy));
        }
    }
    ~MeterOffset() {
        wr16(kTextX, text[0]);
        wr16(kTextY, text[1]);
        for (int i = 0; i < 3; i++) {
            wr32(kHeartSprites + 0x18 * i, sprite[i][0]);
            wr32(kHeartSprites + 4 + 0x18 * i, sprite[i][1]);
        }
    }
};

// Player 2's meter: the game's own health meter drawn again with player 2's health, moved
// next to player 1's (shared view) or into player 2's (right) half of the split screen.
void drawP2Meter(Ctx& c, bool split) {
    const u32 p2 = player(c, 1);
    const u32 arg = rd32(kMeterArg);
    if (!p2 || !gu::inMem1(arg)) return;
    const s16 hp = s16(rd16(p2 + 0xFC));
    const u16 shown = rd16(kHpShown), shown2 = rd16(kHpShown2);
    const u32 phase = rd32(kHeartPhase);
    wr16(kHpShown, u16(hp < 0 ? 0 : hp));
    wr16(kHpShown2, u16(hp < 0 ? 0 : hp));
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        log("[coop] player 2 health meter (hp %d, %s)", hp, split ? "split" : "shared");
    }
    gu::call(c, GU_SYM("LMDefaultOrthoView__Fv"));
    if (split) {
        ws::ShiftScope half(320);  // bottom left of the right half, at any aspect
        gu::call(c, GU_SYM("fn_800417F8"), {arg});
    } else {
        MeterOffset off(150, 0);
        gu::call(c, GU_SYM("fn_800417F8"), {arg});
    }
    wr16(kHpShown, shown);
    wr16(kHpShown2, shown2);
    wr32(kHeartPhase, phase);
}
}  // namespace

HLE(fn_8003E7B0) {
    if (!fixes() || sGbhView) return callOriginal(c, "fn_8003E7B0");
    const u32 view = c.r[3];
    const u32 views = rd32(kViewCount);
    if (views < 2) {
        callOriginal(c, "fn_8003E7B0");
        if (view == 0) drawP2Meter(c, false);
        return;
    }
    if (view == 0) return;  // drawn after the last view
    callOriginal(c, "fn_8003E7B0");  // view 1's own overlay (GBH noise; nothing in 2-player)
    if (view + 1 != views) return;
    // Player 1's HUD over the whole screen: the health meter (bottom left) lies in player 1's
    // (left) half; the Game Boy Horror (bottom right) is moved into that half too, to its
    // bottom right corner (gbhShift, widescreen.cpp's GBH hooks).
    sGbhShift = -320;
    c.r[3] = 0;
    callOriginal(c, "fn_8003E7B0");
    sGbhShift = 0;
    drawP2Meter(c, true);
}

int gbhShift() { return sGbhShift; }

// Flashlight update fn_800850B4(light, params): params +4 is the Luigi it belongs to; the
// beam's ghost test (fn_800859EC) runs inside.
HLE(fn_800850B4) {
    const u32 prev = sLightOwner;
    sLightOwner = gu::inMem1(c.r[4]) ? rd32(c.r[4] + 4) : 0;
    callOriginal(c, "fn_800850B4");
    sLightOwner = prev;
}

// ---- The player an object deals with ----------------------------------------------------------
// Every AI object (EnThought) keeps the index of the player it deals with at +0x950; the game
// reaches that player's Poltergust part (Luigi +0x80C) through fn_800AD39C(index). Objects that
// are caught on the nozzle test that player's suction (Chauncey's balls: fn_800A30B8, state 4,
// [part +0xA8] -> vfunc +0x40), hold on to and are fired from that player's nozzle (part +0x700,
// fn_800A3460), and a hooked ghost's tug of war (the tsuri strategies, e.g. fn_800CDCF4) pulls
// against that player. Most objects set it to 0 (player 1) and never change it, so player 2 could
// not catch the balls and a ghost player 2 hooked was pulled about by player 1's nozzle. With the
// improvements: a ghost being hooked (message 9) deals with the player whose suction hooked it, and
// any object (ghosts included) deals with the player whose running Poltergust points at it while
// the player it deals with does not.
namespace {

// How straight player idx's running Poltergust points at actor a (cosine in the ground plane;
// -1 when not vacuuming or too far).
float suctionAim(Ctx& c, int idx, u32 a, float maxDist) {
    const u32 p = player(c, idx);
    if (!p || !gu::inMem1(rd32(p + 0x80C + 0xA8))) return -1.0f;  // no suction running
    const float dx = gu::rdf(a + 0x44) - gu::rdf(p + 0x44), dz = gu::rdf(a + 0x4C) - gu::rdf(p + 0x4C);
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d > maxDist) return -1.0f;
    if (d < 1.0f) return 1.0f;
    // The better of Luigi's facing (+0xF48/+0xF50) and the nozzle direction (part +0x700, turned ahead
    // of the body by the C stick, but only kept up to date while something is on the nozzle).
    auto cosTo = [&](float nx, float nz) {
        const float nl = std::sqrt(nx * nx + nz * nz);
        return nl > 1e-3f ? (nx * dx + nz * dz) / (nl * d) : -1.0f;
    };
    float aim = cosTo(gu::rdf(p + 0xF48), gu::rdf(p + 0xF50));
    const u32 dir = rd32(p + 0x80C + 0x700);
    if (gu::inMem1(dir)) aim = std::max(aim, cosTo(gu::rdf(dir), gu::rdf(dir + 8)));
    return aim;
}

int suctionPlayer(Ctx& c, u32 a, float maxDist, float minAim) {
    int best = -1;
    float bestAim = minAim;
    for (int i = 0; i < 2; i++)
        if (const float aim = suctionAim(c, i, a, maxDist); aim > bestAim) best = i, bestAim = aim;
    return best;
}

void retargetObjects(Ctx& c) {
    if (off(0x80)) return;
    // Ghosts too: e.g. Boolossus is drawn toward the nozzle of the player he deals with (his state 7,
    // fn_80137E24 -> fn_8014573C(.., +0x950)) and pops on the balcony's spikes; with +0x950 left at
    // 0 only player 1's Poltergust moved him.
    static u32 sLogged = 0;
    for (const std::vector<u32>* list : {&sObjects, &sGhosts})
        for (u32 t : *list) {
            const u32 a = rd32(t + 0x808), cur = rd32(t + 0x950);
            if (!gu::inMem1(a) || cur > 1) continue;
            if (suctionAim(c, int(cur), a, 800.0f) > 0.7f) continue;  // its player is vacuuming at it
            const int want = suctionPlayer(c, a, 800.0f, 0.7f);
            if (want < 0 || u32(want) == cur) continue;
            wr32(t + 0x950, u32(want));
            if (sLogged++ < 200)
                log("[coop] %08X (type %u) now deals with player %d, whose Poltergust points at it", t, rd32(a + 0x3C), want + 1);
        }
}
}  // namespace

// ---- Suction of objects pulled to the nozzle -------------------------------------------------
// fn_80143174(strategy, zako, f1, f2) tells whether an object is in the suction of a running
// Poltergust (and where to move it): hearts and other items (fn_800D1704, ...), Boolossus' frozen
// Boos (fn_8013B714). Underneath, fn_80144664 asks for player 0's Poltergust part literally
// (fn_800AD39C(0)) and fn_8014405C for the object's room being visible in view 0, so only player
// 1 could vacuum them. With the improvements, while player 2's running Poltergust points at the
// object and player 1's does not, the test runs as player 2: player index 0 reads as 1 (fn_800AC164)
// and, in the split screen, view 0 as view 1 (fn_8001CEAC).
namespace {
#define kSuction VA(0x803D8B40)  // suction state (fn_8014588C sets +0xE0 -> +0x00, +0xE4 -> +0x60)
u32 sP2Nozzle = 0;               // player 2's copy of its +0x00 nozzle record (HLE memory)
bool pulledByP2(u32 zako) {
    const u32 now = skip::guestFrames();
    for (const auto& [z, f] : sPulledByP2)
        if (z == zako) return now - f <= 10;
    return false;
}
void markPulledByP2(u32 zako) {
    for (auto& [z, f] : sPulledByP2)
        if (z == zako) {
            f = skip::guestFrames();
            return;
        }
    if (sPulledByP2.size() > 64) sPulledByP2.erase(sPulledByP2.begin());
    sPulledByP2.push_back({zako, skip::guestFrames()});
    static u32 sLogged = 0;
    if (sLogged++ < 20)
        log("[coop] player 2's Poltergust pulls %08X (type %u)", zako, gu::inMem1(zako) ? rd32(rd32(zako + 0x808) + 0x3C) : 0);
}
}

// The sucked-object update (fn_800C60B0(strategy)): the movement to the nozzle (the strategy's
// +0x78 part, its +0x10 update) and the capture once it arrives (fn_800F4374, message 4) follow
// player 1's nozzle. For an object player 2's suction is pulling it runs as player 2.
HLE(fn_800C60B0) {
    const u32 zako = gu::inMem1(c.r[3]) ? rd32(c.r[3] + 4) : 0;
    const bool p2 = fixes() && !off(0x100) && zako && pulledByP2(zako);
    if (p2) sAsPlayer2++;
    callOriginal(c, "fn_800C60B0");
    if (p2) sAsPlayer2--;
}

HLE(fn_80143174) {
    // The test has side effects (a failed test lets go of the object: fn_80143EDC), so it runs once,
    // for the player whose suction is on the object: player 2 while player 2's running Poltergust
    // points at it and player 1's does not (or player 2 has been pulling it).
    const u32 zako = c.r[4];
    const u32 a = gu::inMem1(zako) ? rd32(zako + 0x808) : 0;
    bool p2 = false;
    if (fixes() && !off(0x100) && sAsPlayer2 == 0 && gu::inMem1(a) && player(c, 1)) {
        const float aim1 = suctionAim(c, 0, a, 800.0f), aim2 = suctionAim(c, 1, a, 800.0f);
        p2 = (aim2 > 0.7f && aim1 < aim2) || (pulledByP2(zako) && aim2 > 0.0f && aim1 <= 0.7f);
    }
    // The nozzle the object is pulled into (and captured at) is the suction state's +0xE0 record,
    // filled for player 1 every frame (fn_80145534 -> fn_8012BF4C): player 2's record for the call.
    const u32 rec = kSuction + 0xE0, saved = rd32(rec);
    const bool swap = (p2 || sAsPlayer2 > 0) && sP2Nozzle;
    if (swap) wr32(rec, sP2Nozzle);
    if (p2) sAsPlayer2++;
    callOriginal(c, "fn_80143174");
    if (p2) sAsPlayer2--;
    if (swap) wr32(rec, saved);
    if ((p2 || sAsPlayer2 > 0) && (c.r[3] & 0xFF) != 0) markPulledByP2(zako);
}

// The suction state's per-frame update (fn_80145534): its nozzle record (+0xE0 -> +0x00, 0x60
// bytes: nozzle position and direction pointers, room, reach) is filled from player 1's Poltergust
// (fn_8012BF4C, fn_800AD39C(0)); a copy is filled the same way for player 2.
HLE(fn_80145534) {
    callOriginal(c, "fn_80145534");
    if (!fixes() || off(0x100) || !player(c, 1)) return;
    if (!sP2Nozzle) {
        sP2Nozzle = hleAlloc(0x60, 8);
        for (u32 i = 0; i < 0x60; i += 4) wr32(sP2Nozzle + i, rd32(kSuction + i));
    }
    sAsPlayer2++;
    gu::call(c, VA(0x8012BF4C), {sP2Nozzle}, {double(gu::rdf(c.r[2] - 0x51F0))});
    sAsPlayer2--;
}

// An AI object's per-frame update (EnZako::vt_0C, 0x800C1EBC(thought): its strategy's update, then its light).
// Much object code asks for player 0 literally (fn_800AD39C(0), fn_800E9750(0), fn_800AD54C(0, ..)):
// Bogmire's shadows are caught on player 1's nozzle only (fn_8006E47C, fn_8006EA10, ...). An object
// that deals with player 2 (+0x950 = 1: player 2's Poltergust caught or hooked it, above) runs its
// update as player 2. Not Luigi and his own parts (types up to 10).
HLE(vt_0C__6EnZakoFv) {
    const u32 t = c.r[3];
    const u32 a = gu::inMem1(t) ? rd32(t + 0x808) : 0;
    const bool p2 = fixes() && !off(0x100) && gu::inMem1(a) && rd32(a + 0x3C) > 10 && rd32(t + 0x950) == 1;
    if (p2) sAsPlayer2++;
    callOriginal(c, "vt_0C__6EnZakoFv");
    if (p2) sAsPlayer2--;
}

// EnManager player by index (fn_800E9750): index 0 reads as 1 while running as player 2.
HLE(fn_800E9750__Q24Koga9EnManagerFl) {
    if (sAsPlayer2 > 0 && c.r[3] == 0) c.r[3] = 1;
    callOriginal(c, "fn_800E9750__Q24Koga9EnManagerFl");
}

// Player by index (index < 256: fn_800E9750; else the special object r13+0xCC8).
HLE(fn_800AC164) {
    if (sAsPlayer2 > 0 && c.r[3] == 0) c.r[3] = 1;
    callOriginal(c, "fn_800AC164");
}

// Room visibility record for a view (fn_8001CEAC(room, view)).
HLE(fn_8001CEAC) {
    if (sAsPlayer2 > 0 && c.r[4] == 0 && rd32(kViewCount) >= 2) c.r[4] = 1;
    callOriginal(c, "fn_8001CEAC");
}

// EnThought::receiveMessage (fn_800C0F08), before the original.
void onGhostMessage(Ctx& c) {
    const u32 msg = gu::inMem1(c.r[4]) ? rd32(c.r[4]) : ~0u;
    const u32 self = c.r[3];
    if (fixes() && !off(0x80) && gu::inMem1(self)) {
        const u32 a = rd32(self + 0x808);
        if (msg == 9 && gu::inMem1(a)) {  // fishing begins: the player whose suction hooked it
            const int who = suctionPlayer(c, a, 1500.0f, -1.0f);
            if (who >= 0 && rd32(self + 0x950) <= 1 && rd32(self + 0x950) != u32(who)) {
                log("[coop] %08X (type %u) hooked by player %d's Poltergust (was dealing with player %u)", self, rd32(a + 0x3C),
                    who + 1, rd32(self + 0x950) + 1);
                wr32(self + 0x950, u32(who));
            }
        }
        if (gu::inMem1(a) && !isGhost(a) && rd32(a + 0x3C) > 10 && msg != 3 && msg != 4 &&
            std::find(sObjects.begin(), sObjects.end(), self) == sObjects.end())
            sObjects.push_back(self);
    }
    if (msg != 3 && isGhost(rd32(self + 0x808))) {
        bool known = false;
        for (u32 g : sGhosts) known |= g == self;
        if (!known) sGhosts.push_back(self);
    } else if (msg == 3) {
        std::erase(sGhosts, self);
        log("[coop] ghost %08X captured (lr %08X)", self, c.lr);
    }
    static const bool sLog = envFlag("LM_COOP_MSGLOG");
    if (!sLog) return;
    if (msg == 2 && sLightOwner) {
        const int who = sLightOwner == player(c, 1) ? 1 : 0;
        if (sMsg2[who]++ % 30 == 0) log("[coop] flashlight of P%d lights %08X (x%u)", who + 1, c.r[3], sMsg2[who]);
    }
    static u32 sCount[64] = {};
    if (msg < 64 && sCount[msg]++ < 6)
        log("[coop] msg %u to %08X from lr %08X ev %08X %08X %08X %08X %08X %08X", msg, c.r[3], c.lr, rd32(c.r[4] + 4),
            rd32(c.r[4] + 8), rd32(c.r[4] + 0xC), rd32(c.r[4] + 0x10), rd32(c.r[4] + 0x14), rd32(c.r[4] + 0x18));
}

}  // namespace recomp::coop
