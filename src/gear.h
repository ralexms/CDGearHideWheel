// The player, the characters and their worn gear (paths from Outfit Manager).
#pragma once
#include "common.h"

namespace ghw {

U64 acquire_player(U64* ctx); // the controlled actor (ctx[0] set: release_player(ctx) after use)
void release_player(U64* ctx);
U16 character_of(U64 actor);
U64 inventory_of(U64 actor);   // its inventory / equipment component (0: none)
U64 player_inventory();        // the controlled character's
U16 current_character();       // the controlled character's key (kNoCharacter: none)
U64 equipped_item(U64 list, U16 slot); // the equipment list's entry for a slot (200-byte item + slot id), or 0
bool slot_worn(U64 inv, U16 slot);     // something real is worn there
U64 look_model(U64 actor);     // the character's model (part table +0x58, count +0x60)

// The Weapons wheel's equipment slots (read from its QuickSlotInfo record), and the Armour wheel's.
void collect_wheel_slots();
bool is_weapon_slot(U16 slot);
U16 wheel_slot(U32 record, U32 index); // the equipment slot of a wheel position (0xFFFF: none)

extern std::atomic<U64> g_player_actor; // the controlled actor, as the UI thread last saw it (cheap checks elsewhere)
extern RecordGetFn g_qsi_get;

} // namespace ghw
