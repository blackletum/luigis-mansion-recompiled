#pragma once
// Unused 2-player mode and the co-op improvements (coop.cpp, coop_fixes.cpp,
// docs/UNUSED_CONTENT.md).
#include "recomp/ctx.h"

namespace recomp::coop {

bool wanted();        // setting (or LM_COOP) and US game code
bool fixesWanted();   // co-op improvements setting (or LM_COOP_FIXES), needs wanted()
bool active();        // 2-player mode latched for the current map
bool fixes();         // co-op improvements latched for the current map

void onMapSetup(Ctx& c);       // fn_8000C188 hook (dev_tools.cpp), before the original
void onMapSetupFixes(Ctx& c);  // coop_fixes.cpp
void onGhostMessage(Ctx& c);   // EnThought::receiveMessage (fn_800C0F08) hook, before the original
u32 player(Ctx& c, int idx);
void onLuigiHealth(Ctx& c);    // fn_800B9488(luigi, hp) hook (gameplay_hooks.cpp): may change r4  // player actor (Luigi) idx, 0 when missing

}  // namespace recomp::coop
