// Gameplay / difficulty options as hooks on the game (see gameplay.h,
// docs/GAMEPLAY_OPTIONS.md). Every hook calls the original unchanged when its option is at the
// neutral default, so the original game is untouched unless the player changes something.
#include "recomp/gameplay.h"

#include "recomp/coop.h"
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"
#include "recomp/version.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace recomp {
namespace gameplay {
namespace {

bool inMem1(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }
s16 rds16(u32 a) { return s16(rd16(a)); }

#define GP_FIRST(...)                                       \
    do {                                                    \
        static bool sLogged = false;                        \
        if (!sLogged) {                                     \
            sLogged = true;                                 \
            log("[gameplay] " __VA_ARGS__);                 \
        }                                                   \
    } while (0)

struct Overrides {
    bool any = false;
    port::GameplaySettings v;
    bool has[8] = {};
};

const Overrides& overrides() {
    static const Overrides o = [] {
        Overrides r;
        const char* env = std::getenv("LM_GAMEPLAY");
        if (!env || !*env) return r;
        std::string s = env;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(',', pos);
            if (end == std::string::npos) end = s.size();
            const std::string item = s.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = item.substr(0, eq);
            const float f = float(std::atof(item.c_str() + eq + 1));
            if (k == "dark") r.v.palDarknessEverywhere = f != 0, r.has[0] = true;
            else if (k == "ambient") r.v.ambientScale = f, r.has[1] = true;
            else if (k == "damage") r.v.damageScale = f, r.has[2] = true;
            else if (k == "health") r.v.healthScale = f, r.has[3] = true;
            else if (k == "ghosthp") r.v.ghostHealthScale = f, r.has[4] = true;
            else if (k == "hearts") r.v.noHeartDrops = f == 0, r.has[5] = true;
            else if (k == "vacuum") r.v.vacuumStrength = f, r.has[6] = true;
            else if (k == "ride") r.v.alwaysRide = f != 0, r.has[7] = true;
            else continue;
            r.any = true;
        }
        if (r.any) log("[gameplay] LM_GAMEPLAY overrides: %s", env);
        return r;
    }();
    return o;
}

}  // namespace

port::GameplaySettings current() {
    port::GameplaySettings g = port::settings().gameplay;
    const Overrides& o = overrides();
    if (o.any) {
        if (o.has[0]) g.palDarknessEverywhere = o.v.palDarknessEverywhere;
        if (o.has[1]) g.ambientScale = o.v.ambientScale;
        if (o.has[2]) g.damageScale = o.v.damageScale;
        if (o.has[3]) g.healthScale = o.v.healthScale;
        if (o.has[4]) g.ghostHealthScale = o.v.ghostHealthScale;
        if (o.has[5]) g.noHeartDrops = o.v.noHeartDrops;
        if (o.has[6]) g.vacuumStrength = o.v.vacuumStrength;
        if (o.has[7]) g.alwaysRide = o.v.alwaysRide;
    }
    return g;
}

bool rideAllowed(u32 luigi) {
    // Only in the mansion (map 2) and the boss arenas (9 Bowser, 10 Chauncey, 11 Boolossus):
    // E. Gadd's lab, the training room, the gallery and the ending maps position Luigi by
    // script and are left alone. Off while Luigi has no health (death sequence).
    const u32 map = rd32(VA(0x804D80C8));  // sCurMapNo
    if (map != 2 && map != 9 && map != 10 && map != 11) return false;
    return s16(rd16(luigi + 0xFC)) > 0;
}

float ambientFactor() {
    const port::GameplaySettings g = current();
    float base = (hm::palRulesActive() || g.palDarknessEverywhere) ? 0.2f : 1.0f;
    // The European game applies the PAL hidden-mansion 0.2 itself (fn_800179F8).
    if (ver::pal() && hm::palRulesActive()) base = 1.0f;
    return base * g.ambientScale;
}

}  // namespace gameplay

using gameplay::current;
using gameplay::inMem1;
using gameplay::rds16;

// ---- Damage ------------------------------------------------------------------------------
// fn_800B9488(luigi, newHp) is the one place Luigi's health (+0xFC, s16) is set: it clamps to
// [0, max (+0xFFC)] and plays the hurt/heal reactions. Every damage source (ghost attacks,
// floors, bombs, the hidden mansion's x2) arrives here as a lower value, so the damage taken is
// rescaled here; healing passes through.
HLE(fn_800B9488) {
    // Calls from Luigi's message handler (fn_800AEEA8: event/script commands such as "set
    // health", e.g. the new game's health = 100 on arriving in the mansion) are not damage.
    static const u32 sMsg = guestSymbol("fn_800AEEA8");
    const bool scripted = c.lr >= sMsg && c.lr < sMsg + 0x2884;  // US size of fn_800AEEA8
    const port::GameplaySettings g = current();
    if (scripted) {
        if (g.healthScale != 1.0f && s16(c.r[4]) == 100) {  // "full health" (100) -> scaled full health
            c.r[4] = u32(std::clamp(s32(std::lround(100 * g.healthScale)), 1, 999));
            GP_FIRST("health x%.2f: scripted full health -> %d", g.healthScale, s32(c.r[4]));
        }
        return callOriginal(c, "fn_800B9488");
    }
    const float k = g.damageScale;
    if (k != 1.0f) {
        const u32 luigi = c.r[3];
        const s32 cur = rds16(luigi + 0xFC);
        const s32 next = s16(c.r[4]);
        if (next < cur) {
            const s32 dmg = cur - next;
            s32 scaled = s32(std::lround(float(dmg) * k));
            if (k > 0.0f && scaled == 0) scaled = 1;  // never round a hit away completely
            c.r[4] = u32(s32(cur - scaled)) & 0xFFFF;
            GP_FIRST("damage x%.2f: hit %d -> %d", k, dmg, scaled);
        }
    }
    if (coop::fixes()) coop::onLuigiHealth(c);  // 2-player co-op: a downed player is revived by the partner
    callOriginal(c, "fn_800B9488");
}

// ---- Luigi's health ---------------------------------------------------------------------
// Luigi's constructor sets health and maximum health (+0xFC / +0xFFC) to 100; the game then
// carries both between maps. Scaling them once here scales the starting and maximum health.
HLE(fn_800AD5AC) {
    const u32 luigi = c.r[3];
    callOriginal(c, "fn_800AD5AC");
    const float k = current().healthScale;
    if (k == 1.0f || !inMem1(luigi)) return;
    const s32 maxHp = std::clamp(s32(std::lround(rds16(luigi + 0xFFC) * k)), 1, 999);
    wr16(luigi + 0xFFC, u32(maxHp));
    wr16(luigi + 0xFC, u32(maxHp));
    GP_FIRST("health x%.2f: Luigi starts with %d", k, maxHp);
}

// Restoring Luigi between maps copies health/max from the carried-over state; apply the
// scale to a maximum that is still the original 100 (i.e. a state created without the option).
HLE(fn_800AE1A4) {
    const u32 luigi = c.r[3];
    callOriginal(c, "fn_800AE1A4");
    const float k = current().healthScale;
    if (k == 1.0f || !inMem1(luigi) || rds16(luigi + 0xFFC) != 100) return;
    const s32 maxHp = std::clamp(s32(std::lround(100 * k)), 1, 999);
    const s32 hp = rds16(luigi + 0xFC);
    wr16(luigi + 0xFFC, u32(maxHp));
    if (hp == 100) wr16(luigi + 0xFC, u32(maxHp));
    GP_FIRST("health x%.2f: restored Luigi max %d", k, maxHp);
}

// ---- Ghost HP ---------------------------------------------------------------------------
// EnThought (every ghost's AI) sets its actor's HP (+0xFC) on messages 6 (HP from the spawn
// data) and 7 (HP from the per-type table / EnTypesManager::getEnemyLife), and mirrors it in
// +0x89C. Scale both right after.
HLE(fn_800C0F08) {
    const u32 self = c.r[3];
    const u32 msg = inMem1(c.r[4]) ? rd32(c.r[4]) : 0;
    if (coop::active()) coop::onGhostMessage(c);  // 2-player mode: messages from player 2
    callOriginal(c, "fn_800C0F08");
    const float k = current().ghostHealthScale;
    if (k == 1.0f || (msg != 6 && msg != 7)) return;
    const u32 actor = rd32(self + 0x808);
    if (!inMem1(actor)) return;
    const s32 hp = rds16(actor + 0xFC);
    if (hp <= 0) return;
    const s32 scaled = std::clamp(s32(std::lround(hp * k)), 1, 32767);
    wr16(actor + 0xFC, u32(scaled));
    wr32(self + 0x89C, u32(scaled));
    GP_FIRST("ghost HP x%.2f: %d -> %d", k, hp, scaled);
}

// ---- Vacuum strength --------------------------------------------------------------------
// The Poltergust capture update drains the captured ghost's HP by one on each drain tick
// (capture +0x98 = ghost HP - 1, ghost at capture +0x18). The strength scales the ticks: each
// tick removes `strength` HP, with fractions carried over.
HLE(fn_80081BCC) {
    const float k = current().vacuumStrength;
    const u32 cap = c.r[3];
    const u32 ghost = inMem1(cap) ? rd32(cap + 0x18) : 0;
    if (k == 1.0f || !inMem1(ghost)) return hm::callWithFlag(c, "fn_80081BCC");
    const s32 before = rds16(ghost + 0xFC);
    hm::callWithFlag(c, "fn_80081BCC");
    const s32 after = rds16(cap + 0x98);
    if (before <= 0 || after != before - 1) return;  // no drain tick this frame
    static float sCarry = 0.0f;
    sCarry += k;
    const s32 n = s32(sCarry);
    sCarry -= float(n);
    wr16(cap + 0x98, u32(std::max(0, before - n)));
    GP_FIRST("vacuum x%.2f: drain tick removes %d (ghost HP %d)", k, n, before);
}

// ---- Recovery hearts --------------------------------------------------------------------
namespace {
bool isHeartName(const char* s) {
    static const char* kHearts[] = {"sheart", "mheart", "lheart", "move_sheart", "move_mheart", "move_lheart"};
    for (const char* h : kHearts)
        if (std::strcmp(s, h) == 0) return true;
    return false;
}
u32 guestNothing() {
    static u32 a = [] {
        const u32 p = hleAlloc(8, 4);
        std::memcpy(hostp(p), "nothing", 8);
        return p;
    }();
    return a;
}
}  // namespace

// Item drops are named in the map's JMP tables (furniture/ghost item tables, itemappeartable,
// iteminfotable) and read through ToolData::getValue(row, field, const char**). With hearts
// disabled, any heart item name read from a table is turned into "nothing".
HLE(getValue__Q24Koga8ToolDataCFiPCcPPCc) {
    const u32 out = c.r[6];
    callOriginal(c, "getValue__Q24Koga8ToolDataCFiPCcPPCc");
    if (!current().noHeartDrops || !inMem1(out)) return;
    const u32 str = rd32(out);
    if (!inMem1(str) || !isHeartName(gstr(str))) return;
    GP_FIRST("hearts disabled: table item %s -> nothing", gstr(str));
    wr32(out, guestNothing());
}

// EnHeartItem setup (sets how much the heart heals, +0x238): with hearts disabled, a heart
// that still appears from a source not driven by the tables heals nothing.
HLE(fn_800D1458) {
    const u32 self = c.r[3];
    callOriginal(c, "fn_800D1458");
    if (!current().noHeartDrops || !inMem1(self)) return;
    wr32(self + 0x238, 0);
    GP_FIRST("hearts disabled: heart item heals 0");
}

}  // namespace recomp
