// Unused 2-player mode (F1 > Enhancements > Unused content, off by default; US game code).
//
// The retail game still contains an unfinished two-player mode behind one variable that
// nothing writes: Koga::EnManager::sAppearPointSlotIndex (0x804D80B0) is the index of the
// last player slot. With 1, map setup builds two players (fn_80072554 loop), the camera
// setup (fn_800204B4) gives view 1 a third-person camera following player record 2
// (lbl_803A3CA0 + 0x28), and the per-frame view setup (fn_800112F8 -> fn_80011150) uses one
// 640x240 letterboxed view while both players are in the same room and close together, and
// a top/bottom split screen otherwise.
//
// The known Action Replay code turns it on with the flag, a boot-code cave that builds a
// second JUTGamePad for controller port 2 and stores it in the unused second slot of
// sGamePad (r13+0x11C = 0x804D807C), and eight instruction patches. Here:
// - the flag is latched at every map setup (fn_8000C188 hook in dev_tools.cpp calls
//   onMapSetup), so it only changes while no player exists;
// - the cave becomes host code: the game's own JUTGamePad constructor for port 2, run once
//   in HLE memory (no game heap is touched, so retail heap layout is unchanged);
// - the eight patches are hooks.txt "patch 40..47" entries, switched with the flag.
// The co-op improvements (second toggle) are in coop_fixes.cpp. Docs: docs/UNUSED_CONTENT.md.
#include "recomp/coop.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <cstdlib>

namespace recomp::coop {
namespace {

constexpr u32 kSlotIndex = 0x804D80B0;  // Koga::EnManager::sAppearPointSlotIndex
constexpr u32 kGamePad2 = 0x804D807C;   // sGamePad[1] (r13 + 0x11C)
constexpr u32 kPatchFirst = 40, kPatchCount = 8;

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

bool sActive = false;
bool sFixes = false;

}  // namespace

bool wanted() {
    static const bool sEnv = envFlag("LM_COOP");
    return (sEnv || port::settings().enhancements.twoPlayer) && port::usCodeFeature() == port::Avail::Yes;
}

bool fixesWanted() {
    static const bool sEnv = envFlag("LM_COOP_FIXES");
    return wanted() && (sEnv || port::settings().enhancements.coopImprovements);
}

bool active() { return sActive; }
bool fixes() { return sActive && sFixes; }

void onMapSetup(Ctx& c) {
    const bool on = wanted();
    if (on && rd32(kGamePad2) == 0) {
        // The AR cave: new JUTGamePad(EPadPort 1) stored in sGamePad[1]. The constructor links
        // it into JUTGamePad::mPadList, so the game's pad read updates it every frame.
        // JKRDisposer finds no heap for HLE memory, so nothing ever frees it.
        const u32 pad = hleAlloc(0x98);
        gu::call(c, GU_SYM("__ct__10JUTGamePadFQ210JUTGamePad8EPadPort"), {pad, 1});
        wr32(kGamePad2, pad);
        log("[coop] controller port 2 pad created at %08X", pad);
    }
    if (on != sActive) log("[coop] 2-player mode %s", on ? "on" : "off");
    sActive = on;
    sFixes = on && fixesWanted();
    wr32(kSlotIndex, on ? 1 : 0);
    // LM_COOP_PATCHES=<hex mask>: test aid, only the AR patches whose bit is set (bit 0 = patch 40).
    static const u32 sMask = [] {
        const char* v = std::getenv("LM_COOP_PATCHES");
        return v && *v ? u32(std::strtoul(v, nullptr, 16)) : 0xFFu;
    }();
    for (u32 i = 0; i < kPatchCount; i++) setPatch(kPatchFirst + i, on && (sMask >> i & 1));
    onMapSetupFixes(c);
}

}  // namespace recomp::coop
