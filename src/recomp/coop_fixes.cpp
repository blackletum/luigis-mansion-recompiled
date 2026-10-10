// Co-op improvements on top of the unused 2-player mode (coop.cpp): F1 > Enhancements >
// Unused content "Co-op improvements", off by default. Docs: docs/UNUSED_CONTENT.md.
#include "recomp/coop.h"

#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/skip.h"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace recomp::coop {
namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

u32 sLightOwner = 0;  // Luigi whose flashlight update (fn_800850B4) is running
u32 sMsg2[2] = {};

// Player records of the view setup (lbl_803A3CA0, 0x28 per player): +0 position, +0xC the
// room number in its top byte (from the player's room, fn_800E9750(i) +0xB4).
constexpr u32 kViewPlayers = 0x803A3CA0;
u32 roomOf(int idx) { return rd8(kViewPlayers + 0x28 * idx + 0xC); }
u32 sLastRoom[2] = {~0u, ~0u};

// Ghost AI objects (EnThought, +0x808 = actor of a ghost type) that got a message this map;
// message 3 (capture end) removes them.
std::vector<u32> sGhosts;

// Character type flags (type table lbl_8033B590, 0x1C per type, u16 at +8; fn_8006A3BC): 8 = ghost.
constexpr u32 kTypeTable = 0x8033B590;
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

void onMapSetupFixes(Ctx&) {
    sGhosts.clear();
    sLastRoom[0] = sLastRoom[1] = ~0u;
}

namespace {
// Puts player 2 right behind player 1 (facing the same way), e.g. after player 1 went
// through a door: the game only follows player 1 into the next room.
void bringP2(u32 p1, u32 p2) {
    const float fx = gu::rdf(p1 + 0xF48), fz = gu::rdf(p1 + 0xF50);
    gu::wrf(p2 + 0x44, gu::rdf(p1 + 0x44) - fx * 120.0f);
    gu::wrf(p2 + 0x48, gu::rdf(p1 + 0x48));
    gu::wrf(p2 + 0x4C, gu::rdf(p1 + 0x4C) - fz * 120.0f);
}
}  // namespace

u32 player(Ctx& c, int idx) {
    if (!active() && idx > 0) return 0;
    const u32 p = gu::call(c, GU_SYM("fn_800E9750__Q24Koga9EnManagerFl"), {u32(idx)});
    return gu::inMem1(p) ? p : 0;
}

// Game-scene view/camera update (fn_800112F8, once per game frame).
HLE(fn_800112F8) {
    callOriginal(c, "fn_800112F8");
    if (!active()) return;
    static const bool sDump = envFlag("LM_COOP_DUMP");
    if (sDump) {
        static u32 sFrame = 0;
        if (sFrame++ % 300 == 0) {
            for (int i = 0; i < 2; i++) {
                const u32 p = player(c, i);
                if (!p) continue;
                if (i == 0) log("[coop] guest frame %u, %zu ghosts known", skip::guestFrames(), sGhosts.size());
                log("[coop] P%d %08X pos %.0f,%.0f,%.0f hp %d flags %04X 7D4 %08X 7D8 %08X 7E0 %08X 7E4 %08X 7A4 %08X 7A8 %08X 7AC %08X",
                    i + 1, p, gu::rdf(p + 0x44), gu::rdf(p + 0x48), gu::rdf(p + 0x4C), s16(rd16(p + 0xFC)), rd16(p + 0x792),
                    rd32(p + 0x7D4), rd32(p + 0x7D8), rd32(p + 0x7E0), rd32(p + 0x7E4), rd32(p + 0x7A4), rd32(p + 0x7A8),
                    rd32(p + 0x7AC));
            }
        }
    }
    if (fixes()) {
        const u32 p1 = player(c, 0), p2 = player(c, 1);
        const u32 r1 = roomOf(0), r2 = roomOf(1);
        if (p1 && p2 && r1 != sLastRoom[0] && sLastRoom[0] != ~0u && r1 != r2) {
            bringP2(p1, p2);
            log("[coop] player 1 entered room %u: player 2 (room %u) follows", r1, r2);
        }
        sLastRoom[0] = r1;
        sLastRoom[1] = r2;
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
constexpr u32 kViewCount = 0x804CFF90;   // lbl_804CFF90, fn_8000ADFC
constexpr u32 kHpShown = 0x804D8490;     // s16 health shown by the meter (eased toward Luigi's)
constexpr u32 kHpShown2 = 0x804D8492;    // s16 second meter value (damage flash)
constexpr u32 kHeartPhase = 0x804D84B0;  // heartbeat animation phase
constexpr u32 kTextX = 0x804D8496, kTextY = 0x804D8498;  // s16 digits position
constexpr u32 kHeartSprites = 0x803CE698;                // +0x30/+0x48/+0x60: s32 x, y
constexpr u32 kMeterArg = 0x804D84CC;                    // fn_800417F8's argument

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
            const u32 sp = kHeartSprites + 0x30 + 0x18 * i;
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
            wr32(kHeartSprites + 0x30 + 0x18 * i, sprite[i][0]);
            wr32(kHeartSprites + 0x34 + 0x18 * i, sprite[i][1]);
        }
    }
};

// Player 2's meter: the game's own health meter drawn again with player 2's health, moved
// next to player 1's (shared view) or into player 2's half (split screen).
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
    {
        MeterOffset off(split ? 0 : 150, 0);
        gu::call(c, GU_SYM("fn_800417F8"), {arg});
    }
    wr16(kHpShown, shown);
    wr16(kHpShown2, shown2);
    wr32(kHeartPhase, phase);
}
}  // namespace

HLE(fn_8003E7B0) {
    if (!fixes()) return callOriginal(c, "fn_8003E7B0");
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
    {
        MeterOffset up(0, -240);  // player 1's meter at the bottom of the top half
        c.r[3] = 0;
        callOriginal(c, "fn_8003E7B0");
    }
    drawP2Meter(c, true);
}

// Flashlight update fn_800850B4(light, params): params +4 is the Luigi it belongs to; the
// beam's ghost test (fn_800859EC) runs inside.
HLE(fn_800850B4) {
    const u32 prev = sLightOwner;
    sLightOwner = gu::inMem1(c.r[4]) ? rd32(c.r[4] + 4) : 0;
    callOriginal(c, "fn_800850B4");
    sLightOwner = prev;
}

// EnThought::receiveMessage (fn_800C0F08), before the original.
void onGhostMessage(Ctx& c) {
    const u32 msg = gu::inMem1(c.r[4]) ? rd32(c.r[4]) : ~0u;
    const u32 self = c.r[3];
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
