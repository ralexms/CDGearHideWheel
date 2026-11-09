// Keyboard, mouse buttons and controllers (XInput; PlayStation pads over HID), read by a thread of our own.
#pragma once
#include "common.h"

namespace ghw {

void start_input();                // the reader threads
bool take_toggle();                // a press of the toggle binding since the last call (UI thread)
bool take_secondary();             // ... of the secondary binding (underwear / shields)
extern std::atomic<bool> g_listen; // presses are latched only while this is on (the gear wheel is open)
void set_listen(bool on);          // the gear wheel opened / closed: wakes the reader threads, or lets them sleep
extern std::atomic<bool> g_pad_in_use; // the last input touched was a controller (prompts show its buttons)

} // namespace ghw
