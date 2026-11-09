// The back equipment stack: a hidden cape no longer pushes the weapons on the back outward.
//
// The game lays out what a character carries on the back (and the shoulders) as a stack: the body's socket data lists
// the inner parts (Cloak, Upperbody), each worn mesh declares a thickness, and the stack's origin bone is pushed out by
// the largest inner thickness. The cape counts while equipped, visible or not. For a character whose cape this mod
// hides, the stack is built as if no cape were worn (see docs/cape-offset-research.md).
#pragma once
#include "common.h"

namespace ghw {

bool install_stack(); // hooks the stack builder; false: feature off (logged)
// From vis_sync: this model belongs to character ch, whose cape is (not) hidden now; layout_changed: the mod just hid
// or showed pieces on it. The game lays the stacks out from what is visible when it builds them, so the model is
// rebuilt on the next UI frame when its pieces changed or its stacks were built with the cape the other way.
void stack_note(U64 model, U16 ch, bool cape_hidden, bool layout_changed);
void stack_tick(); // the UI frame: rebuilds the stacks that wait for it

} // namespace ghw
