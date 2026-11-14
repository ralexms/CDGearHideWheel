// The player, the characters and their worn gear (paths from Outfit Manager, MIT licence).
#include "common.h"
#include "gear.h"
#include "addrs.h"

namespace ghw {

std::atomic<U64> g_player_actor{0};
RecordGetFn g_qsi_get = nullptr;

const char* character_name(U16 key)
{
    switch (key) {
    case kKliff: return "Kliff";
    case kDamiane: return "Damiane";
    case kOongka: return "Oongka";
    }
    return nullptr;
}

U64 acquire_player(U64* ctx)
{
    U64 holder = rp(rp(g_base + kRvaPlayerHolder));
    if (!holder || !g_actor_ctx) return 0;
    __try {
        g_actor_ctx(holder, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx[0] = 0;
        return 0;
    }
    return (ctx[2] & 0xFF) && plausible(ctx[1]) ? ctx[1] : 0;
}

void release_player(U64* ctx)
{
    ctx[0] = g_base + kRvaCtxVt;
    __try {
        g_ctx_release(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

U16 character_of(U64 actor)
{
    U64 p = rp(actor + kOffActorComp);
    p = p ? rp(p + 0x20) : 0;
    U16 k = kNoCharacter;
    if (p) rd(p + 0x30, k);
    return k;
}

U64 inventory_of(U64 actor)
{
    U64 c = actor ? rp(actor + kOffActorComp) : 0;
    return c ? rp(c + kOffCompInventory) : 0;
}

U64 player_inventory()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 inv = inventory_of(actor);
    if (ctx[0]) release_player(ctx);
    return inv;
}

U16 current_character()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U16 k = actor ? character_of(actor) : kNoCharacter;
    g_player_actor = actor;
    if (ctx[0]) release_player(ctx);
    return k;
}

U64 equipped_item(U64 list, U16 slot)
{
    U64 e = rp(list + 8);
    U32 n = r32(list + 0x10);
    for (U32 i = 0; e && i < n && i < 64; ++i, e += 0xd0) {
        U16 s = 0xFFFF;
        if (!rd(e + kOffEntrySlot, s)) return 0;
        if (s == slot) return e;
    }
    return 0;
}

bool slot_worn(U64 inv, U16 slot)
{
    U64 list = inv ? rp(inv + kOffInvEquipList) : 0;
    U64 e = list ? equipped_item(list, slot) : 0;
    U64 uid = 0;
    U16 key = 0xFFFF;
    return e && rd(e, uid) && rd(e + 8, key) && uid && key != 0xFFFF;
}

// The path the headgear switch takes from the inventory's actor to the character model.
U64 look_model(U64 actor) { return rp(rp(rp(rp(rp(rp(actor + kOffActorComp) + 0x40) + 0x130)) + 8) + kOffModelRoot); }

namespace {
std::mutex g_slots_mu;
std::vector<U16> g_wheel[2]; // per record (Weapons, Armour): the equipment slot of each position
std::atomic<bool> g_collected{false};

U64 record(U32 key)
{
    __try {
        const U16 k = static_cast<U16>(key);
        return g_qsi_get(&k);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
} // namespace

void collect_wheel_slots()
{
    if (!g_qsi_get) return;
    std::vector<U16> got[2];
    for (U32 r = 0; r < 2; ++r) {
        U64 rec = record(r);
        U64 slots = rec ? rp(rec + kRecSlots) : 0;
        U32 n = rec ? r32(rec + kRecSlotCount) : 0;
        for (U32 i = 0; slots && i < n && i < kMaxWheelSlots; ++i) {
            U16 s = 0xFFFF;
            if (!rd(slots + i * kSlotSize + kSlotEquipIdx, s)) break;
            got[r].push_back(s);
        }
    }
    if (got[0].empty() && got[1].empty()) return;
    std::string w, a;
    for (U16 s : got[0]) w += " " + std::to_string(s);
    for (U16 s : got[1]) a += " " + std::to_string(s);
    {
        std::lock_guard<std::mutex> lock(g_slots_mu);
        if (g_collected && got[0] == g_wheel[0] && got[1] == g_wheel[1]) return;
        g_wheel[0] = std::move(got[0]), g_wheel[1] = std::move(got[1]);
    }
    g_collected = true;
    LOG("wheel slots: weapons%s; armour%s", w.c_str(), a.c_str());
}

bool is_weapon_slot(U16 slot)
{
    if (!g_collected) collect_wheel_slots();
    std::lock_guard<std::mutex> lock(g_slots_mu);
    return std::find(g_wheel[0].begin(), g_wheel[0].end(), slot) != g_wheel[0].end();
}

U16 wheel_slot(U32 record, U32 index)
{
    if (record > 1) return 0xFFFF;
    if (!g_collected) collect_wheel_slots();
    std::lock_guard<std::mutex> lock(g_slots_mu);
    return index < g_wheel[record].size() ? g_wheel[record][index] : 0xFFFF;
}

} // namespace ghw
