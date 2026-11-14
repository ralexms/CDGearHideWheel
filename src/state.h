// What each character hides: kept in GearHideWheel.state beside the .asi (per character, for every save).
#pragma once
#include "common.h"

namespace ghw {

struct CharState {
    std::set<U16> hidden;  // equipment slots hidden on our layer (the hat slot uses the headgear mode instead); for
                           // weapon slots this covers everything but shields
    int shield = 0;        // ShieldMode: the character's shields (wherever worn), the rest of a weapon slot apart
    bool underwear = false; // the body's Underwear part hidden
    int headgear = -1;      // HeadMode, or -1: the game's own setting
    int pack = 0;           // PackMode: mobility packs on the backpack slot (the rocket pack); the slot's entry in
                            // `hidden` covers the other packs (utility packs)
};

void load_state();
CharState state_of(U16 ch); // (a copy: any thread)
bool state_hidden(U16 ch, U16 slot);
bool state_underwear(U16 ch);
int state_headgear(U16 ch); // (any thread, cheap: the headgear hooks ask it on every decision)
// Changes (UI thread): saved to the file at once.
void state_set_hidden(U16 ch, U16 slot, bool hidden);
void state_set_underwear(U16 ch, bool hidden);
void state_set_headgear(U16 ch, int mode);
bool state_any(U16 ch); // anything hidden / a headgear mode for this character
// Shields: shown, hidden, or shown only while in the off hand (hidden on the back, where the game carries the shield
// while another weapon takes the off hand).
enum ShieldMode : int { kShieldShown = 0, kShieldHidden = 1, kShieldInHand = 2, kShieldModeCount = 3 };
const char* shield_mode_text(int mode);
int state_shield(U16 ch);
void state_set_shield(U16 ch, int mode);
// Mobility packs (a backpack slot piece the character moves with: Oongka's rocket pack): shown, hidden, or shown only
// while in use (out of its stowed state: flying).
enum PackMode : int { kPackShown = 0, kPackHidden = 1, kPackInUse = 2, kPackModeCount = 3 };
const char* pack_mode_text(int mode);
int state_pack(U16 ch);
void state_set_pack(U16 ch, int mode);

} // namespace ghw
