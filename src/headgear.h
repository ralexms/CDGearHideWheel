// Per-character headgear mode, through the game's own headgear option.
//
// The game keeps its "Headgear Visibility" option in one global byte. Five functions read it while deciding a
// character's headgear (each gets that character's inventory first); the decision itself - combat, cutscenes, the
// hair under the hat - stays the game's. Around those five calls the byte is swapped for the character's own mode,
// and put back right after, so the game's menu, its saved settings and every other character keep the real value.
#pragma once
#include "common.h"

namespace ghw {

// The option's values (in the order of the game's menu; the log names the value read at start-up).
enum HeadMode : int { kHeadShown = 0, kHeadCombat = 1, kHeadCutscenes = 2, kHeadHidden = 3, kHeadModeCount = 4 };

int headgear_mode_from_name(const std::string& name); // "Shown" / "Hidden" / "Combat" / "Cutscenes", or -1
const char* headgear_mode_text(int mode);             // "Always shown", ...

bool install_headgear(); // finds the option and the five readers, hooks them; false: feature off (logged)
bool headgear_ready();
int game_headgear_option(); // the game's own setting now (-1: unknown)
void refresh_headgear(U64 inv); // re-runs the game's headgear decision for this character (UI thread)

} // namespace ghw
