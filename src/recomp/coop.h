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

}  // namespace recomp::coop
