// The visibility engine: hides each character's chosen pieces on our own layer (8) of the model's part visibility.
#pragma once
#include "common.h"

namespace ghw {

bool install_vis();     // hooks the headgear switch, the model rebuild and the drawn-weapon check
void vis_sync(U64 inv); // applies that character's choices to its model now (UI / game thread)
void vis_queue(U64 inv); // ... on the next UI frame (any thread)
void vis_tick();        // the UI frame: queued syncs, and a periodic re-check of the known characters
void vis_log_model(U64 inv); // names every part of the character's model in the log (diagnostics)
bool vis_slot_shield(U64 inv, U16 slot); // the piece worn in this slot is a shield
U16 vis_model_character(U64 model);      // whose model this is (as the last sync saw it), or kNoCharacter

} // namespace ghw
