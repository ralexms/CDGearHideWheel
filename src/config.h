// The ini file (GearHideWheel.ini): key bindings and options.
#pragma once
#include "common.h"

namespace ghw {

// One input: a keyboard / mouse key (virtual-key code) or a controller button (XInput bit, see kPad*).
struct Input {
    bool pad = false;
    U32 code = 0;
};
// All inputs of a combo held at once ("Ctrl+H", "Gamepad_LB+Gamepad_Y").
using Combo = std::vector<Input>;
// Any of several combos ("H, Gamepad_RS").
struct Binding {
    std::vector<Combo> combos;
    std::string key_text; // the keyboard combos as shown in game ("H")
    std::string pad_text; // the controller combos as shown in game ("RS")
    bool empty() const { return combos.empty(); }
};

// Controller buttons: XInput's wButtons bits, plus the triggers (and PlayStation pads mapped onto the same bits).
constexpr U32 kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008, kPadStart = 0x0010,
              kPadBack = 0x0020, kPadLS = 0x0040, kPadRS = 0x0080, kPadLB = 0x0100, kPadRB = 0x0200, kPadA = 0x1000,
              kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000, kPadLT = 0x10000, kPadRT = 0x20000;

struct Config {
    Binding toggle;       // cycles the highlighted piece
    Binding secondary;    // the second key: the underwear on the Chest slot, shields on a weapon slot
    bool notifications = true;     // a toast naming the new state on every press
    bool show_prompt = true;       // the key hint in the wheel's centre tooltip
    bool companions = true;        // apply each character's choices to them as companions too
    bool show_stowed_tools = true; // tools show on their stow socket, not only in the hand (unless hidden)
    bool cape_offset_fix = true;   // a hidden cape no longer pushes the weapons on the back outward
    bool cape_offset_refresh = true; // ... applied at once when the cape is toggled (else on the next load / gear change)
    bool verbose = false;
    // Headgear modes in the order the toggle cycles them (the game's option values, see headgear.h).
    std::vector<int> headgear_cycle;
};
extern Config g_cfg;

void load_config(); // reads GearHideWheel.ini beside the .asi (writes a default one if missing)
std::string binding_text(const Binding& b, bool pad); // "H" / "RS" (falls back to the other kind)

} // namespace ghw
