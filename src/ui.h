// UI element helpers (from Outfit Manager): classes, text, the game's toast; item names.
#pragma once
#include "common.h"

namespace ghw {

bool install_ui(); // the UI functions (class names, interning, restyle, text, toast); false: no caption / toasts
bool is_element(U64 e);
U64 find_by_class(U64 elem, const char* cls, int max_depth = 8, int depth = 0);
bool has_class(U64 elem, const char* cls);
bool set_class(U64 elem, const char* cls, bool on); // false: the element is mid restyle (try again next frame)
bool set_text(U64 elem, const char* utf8);
void notify(U64 script, const std::string& text); // the game's toast (any UI script of the screen), and the log
std::string item_name(U16 key);                     // localized ("" if unknown)

} // namespace ghw
