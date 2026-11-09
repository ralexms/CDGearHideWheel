// The visibility engine: hides each character's chosen pieces on our own layer (8) of the model's part visibility.
//
// Built on Outfit Manager's appearance engine (MIT licence): the model's part table, the part names, weapon cases and
// the drawn-weapon check are its findings. This version only ever hides (or lets go), on a layer above every other, so
// it neither fights the game's own layers nor Outfit Manager's (layer 6).
#include "common.h"
#include "vis.h"
#include "addrs.h"
#include "gear.h"
#include "state.h"
#include "config.h"
#include "headgear.h"
#include "stack.h"

namespace ghw {

namespace {

VisSetFn g_vis_set = nullptr;
VisReleaseFn g_vis_release = nullptr;
SlotHandleFn g_slot_handle = nullptr;
BattleVisFn g_battle_vis_orig = nullptr;
ModelSwapFn g_model_swap_orig = nullptr;
EquipVisCheckFn g_equip_vis_check_orig = nullptr;

std::mutex g_mu; // everything below (game thread, job threads, UI thread)
struct InvInfo {
    U64 model = 0;
    U16 ch = kNoCharacter;
    std::set<int> applied; // handles our layer hides now
    std::string last_log;
};
std::map<U64, InvInfo> g_inv;                                      // inventory -> what we did to its model
std::map<U64, U64> g_model_inv;                                    // model -> inventory
std::vector<U64> g_pending;                                        // inventories to sync on the next UI frame
std::map<std::pair<U64, U32>, std::pair<U64, U16>> g_weapon_parts; // (vis ctrl, part name) -> (inventory, slot)
std::map<std::pair<U64, U32>, bool> g_part_drawn;                  // (vis ctrl, part name) -> drawn
std::map<std::pair<U64, U16>, bool> g_drawn;                       // (inventory, slot) -> the weapon is drawn
std::map<std::pair<U64, U32>, U64> g_stow;                         // (vis ctrl, part name) -> inventory: a tool shown when stowed too
// The in/out record's display byte (PartInOut +0x20, the in/out check's first read: "movzx r11d, byte [r8+0x20]"):
// bit 2 hides the part while it is stowed ("In") - the game sets it for tools, which then only show in the hand.
// Clearing the byte shows them on their stow socket as well (as Gear Hider's "restore" does).
constexpr U64 kOffInOutDisplay = 0x20;

// --- The model's parts ------------------------------------------------------------------------------
// The model's table (+0x58: 16-byte {u32 handle, .., part*}, count +0x60). A part: +0x48 its name (part-name string
// index), children +0x18 (count +0x20), visibility layers +0x70, +0x78 non-null while still loading.

U64 look_part(U64 model, int handle)
{
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (static_cast<int>(r32(tab + i * 16ull)) == handle) return rp(tab + i * 16ull + 8);
    return 0;
}

std::vector<U64> look_parts(U64 model, int handle)
{
    std::vector<U64> v;
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (static_cast<int>(r32(tab + i * 16ull)) == handle)
            if (U64 part = rp(tab + i * 16ull + 8)) {
                v.push_back(part);
                U64 kids = rp(part + 0x18);
                for (U32 k = 0, c = r32(part + 0x20); kids && k < c && k < 16; ++k)
                    if (U64 kid = rp(kids + k * 8ull)) v.push_back(kid);
            }
    return v;
}

U64 g_part_names = 0;
std::string part_name(U32 index)
{
    if (!g_part_names && kRvaPartNameMap) {
        const auto* code = reinterpret_cast<const U8*>(g_base + kRvaPartNameMap);
        for (int i = 0; i < 0x40 - 7 && !g_part_names; ++i)
            if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x05) {
                std::int32_t disp;
                std::memcpy(&disp, code + i + 3, 4);
                if (U64 g = rp(g_base + kRvaPartNameMap + i + 7 + disp)) g_part_names = rp(g + 0x58);
            }
    }
    std::string s;
    U64 p = g_part_names ? rp(g_part_names + U64(index) * 16) : 0;
    for (int i = 0; p && i < 96; ++i) {
        char c = 0;
        if (!rd(p + i, c) || c < 0x20 || c > 0x7E) break;
        s += c;
    }
    if (s.rfind("CD_", 0) == 0) s.erase(0, 3); // (2.04 dropped the prefix from part names)
    return s;
}

// A weapon's case (scabbard) is a part of its own, named like the weapon with "IN" before its side
// (MainWeapon_Sword_R -> MainWeapon_Sword_IN_R, TwoHandWeapon_Sword -> TwoHandWeapon_Sword_IN). Its handle, or 0.
int case_handle(U64 model, const std::string& weapon)
{
    if (weapon.empty()) return 0;
    std::vector<std::string> t;
    for (size_t a = 0;;) {
        size_t b = weapon.find('_', a);
        t.push_back(weapon.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    size_t side = t.size();
    for (size_t i = t.size(); i-- > 1;)
        if (t[i] == "R" || t[i] == "L") {
            side = i;
            break;
        }
    t.insert(t.begin() + static_cast<std::ptrdiff_t>(side), "IN");
    std::string want;
    for (size_t i = 0; i < t.size(); ++i) want += (i ? "_" : "") + t[i];
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8); part && part_name(r32(part + 0x48)) == want)
            return static_cast<int>(r32(tab + i * 16ull));
    return 0;
}

// Parts of their own that belong with a weapon and go with it (the bow's quiver and arrows, a lantern's ring).
std::vector<int> extra_handles(U64 model, const std::string& weapon)
{
    static const std::vector<const char*> kBow = {"MainWeapon_Quiver",        "MainWeapon_Quiver_Arw",
                                                  "MainWeapon_Quiver_Arw_01", "MainWeapon_Quiver_Arw_02",
                                                  "MainWeapon_Quiver_Arw_03", "MainWeapon_Arw_IN",
                                                  "MainWeapon_Arw"};
    static const std::vector<const char*> kBag = {"Bag_Belt"};
    static const std::vector<const char*> kLantern = {"Lantern_Ring"};
    std::vector<int> v;
    const std::vector<const char*>* group =
        weapon == "MainWeapon_Bow" ? &kBow : weapon == "Bag" ? &kBag : weapon == "Lantern" ? &kLantern : nullptr;
    if (!group) return v;
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8)) {
            std::string nm = part_name(r32(part + 0x48));
            for (const char* b : *group)
                if (nm == b) v.push_back(static_cast<int>(r32(tab + i * 16ull)));
        }
    return v;
}

// A model's part by name: its handle, or 0 (the underwear: no slot leads to it).
int named_handle(U64 model, const char* want)
{
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8); part && part_name(r32(part + 0x48)) == want)
            return static_cast<int>(r32(tab + i * 16ull));
    return 0;
}

int slot_handle(U64 inv, U16 slot)
{
    int h = 0;
    __try {
        g_slot_handle(inv, &h, static_cast<short>(slot));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 0;
    }
    return h;
}

// Our layer on one part: 0 hide, 2 let go.
bool layer_call(U64 model, int handle, int op)
{
    struct {
        int* p;
        U32 n;
    } list{&handle, 1};
    __try {
        if (op == 2) g_vis_release(model, reinterpret_cast<U64>(&list), kLayer, 1, 0);
        else g_vis_set(model, reinterpret_cast<U64>(&list), 0, kLayer, 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Our layer already hides this part (so nothing is called: our calls stay rare, and other mods that watch the game's
// layer calls - Outfit Manager does - don't get woken every frame).
bool hidden_by_us(U64 model, int handle)
{
    U64 part = look_part(model, handle);
    if (!part) return false;
    U32 mask = r32(part + 0x70);
    return (mask >> (16 + kLayer) & 1) && !(mask >> kLayer & 1);
}

bool is_shield_part(const std::string& name) { return name.find("Shield") != std::string::npos; }

// The vis ctrls the in/out hook has anything to do for (a hidden weapon, a stowed tool), published lock-free: the
// game runs that check for every managed part of every character, nearly all of them none of ours.
constexpr int kHotMax = 8;
std::atomic<U64> g_hot[kHotMax] = {};
std::atomic<bool> g_hot_overflow{false}; // (more than kHotMax: every call takes the slow path)

bool hot(U64 vc)
{
    if (g_hot_overflow.load(std::memory_order_relaxed)) return true;
    for (const auto& h : g_hot)
        if (h.load(std::memory_order_relaxed) == vc) return true;
    return false;
}

// (g_mu held) after g_weapon_parts / g_stow changed
void publish_hot()
{
    std::set<U64> vcs;
    for (const auto& [key, own] : g_weapon_parts) vcs.insert(key.first);
    for (const auto& [key, inv] : g_stow) vcs.insert(key.first);
    g_hot_overflow = vcs.size() > kHotMax;
    int i = 0;
    for (U64 v : vcs)
        if (i < kHotMax) g_hot[i++] = v;
    for (; i < kHotMax; ++i) g_hot[i] = 0;
}

// (g_mu held)
void forget(U64 inv)
{
    g_inv.erase(inv);
    for (auto it = g_model_inv.begin(); it != g_model_inv.end();) it = it->second == inv ? g_model_inv.erase(it) : std::next(it);
    for (auto it = g_weapon_parts.begin(); it != g_weapon_parts.end();)
        it = it->second.first == inv ? g_weapon_parts.erase(it) : std::next(it);
    for (auto it = g_drawn.begin(); it != g_drawn.end();) it = it->first.first == inv ? g_drawn.erase(it) : std::next(it);
    for (auto it = g_stow.begin(); it != g_stow.end();) it = it->second == inv ? g_stow.erase(it) : std::next(it);
    publish_hot();
}

void __fastcall battle_vis_hook(U64 inv, U8 a, U8 b)
{
    g_battle_vis_orig(inv, a, b); // (the game's headgear switch: loading, battle changes, model rebuilds)
    vis_sync(inv);
}

void __fastcall model_swap_hook(U64 job, U64 a, U64 b, char c, U8* st)
{
    g_model_swap_orig(job, a, b, c, st);
    U64 model = rp(rp(rp(job + 8) + 8) + kOffModelRoot);
    std::lock_guard<std::mutex> lock(g_mu);
    if (auto it = g_model_inv.find(model); it != g_model_inv.end())
        if (std::find(g_pending.begin(), g_pending.end(), it->second) == g_pending.end()) g_pending.push_back(it->second);
}

// The game's in/out check (drawing / sheathing, per part): a hidden weapon drawn shows in the hand, and goes again
// when it is put away. Synced on the next UI frame (the game is in the middle of laying the parts out here).
float __fastcall equip_vis_check_hook(U64 vc, const U32* part, U8* rec, U8 drawn)
{
    if (!hot(vc)) return g_equip_vis_check_orig(vc, part, rec, drawn); // (not one of ours: no lock, no lookups)
    U32 name = 0;
    const bool named = part && rd(reinterpret_cast<U64>(part), name);
    if (named && rec) {
        std::lock_guard<std::mutex> lock(g_mu);
        U8 b = 0;
        if (!g_stow.empty() && g_stow.count({vc, name}) && rd(reinterpret_cast<U64>(rec) + kOffInOutDisplay, b) && (b & 2))
            wr<U8>(reinterpret_cast<U64>(rec) + kOffInOutDisplay, 0);
    }
    float r = g_equip_vis_check_orig(vc, part, rec, drawn);
    if (!named) return r;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_weapon_parts.empty()) return r;
    auto it = g_weapon_parts.find({vc, name});
    if (it == g_weapon_parts.end()) return r;
    const auto owner = it->second;
    g_part_drawn[{vc, name}] = drawn == 1;
    bool now = false;
    for (const auto& [key, own] : g_weapon_parts)
        if (key.first == vc && own == owner) now |= g_part_drawn[key];
    bool& d = g_drawn[owner];
    if (d == now) return r;
    d = now;
    if (std::find(g_pending.begin(), g_pending.end(), owner.first) == g_pending.end()) g_pending.push_back(owner.first);
    return r;
}

} // namespace

bool install_vis()
{
    if (!have({kRvaVisSet, kRvaVisRelease, kRvaSlotHandle, kRvaBattleVis})) {
        LOG("visibility: game functions not found; hiding is off");
        return false;
    }
    g_vis_set = reinterpret_cast<VisSetFn>(g_base + kRvaVisSet); // (called through whatever hooks them: Outfit Manager's)
    g_vis_release = reinterpret_cast<VisReleaseFn>(g_base + kRvaVisRelease);
    g_slot_handle = reinterpret_cast<SlotHandleFn>(g_base + kRvaSlotHandle);
    void* t = reinterpret_cast<void*>(g_base + kRvaBattleVis);
    if (MH_CreateHook(t, reinterpret_cast<void*>(&battle_vis_hook), reinterpret_cast<void**>(&g_battle_vis_orig)) != MH_OK ||
        MH_EnableHook(t) != MH_OK) {
        LOG("visibility: headgear switch hook failed; hiding is off");
        return false;
    }
    if (have({kRvaModelSwap})) {
        void* s = reinterpret_cast<void*>(g_base + kRvaModelSwap);
        if (MH_CreateHook(s, reinterpret_cast<void*>(&model_swap_hook), reinterpret_cast<void**>(&g_model_swap_orig)) != MH_OK ||
            MH_EnableHook(s) != MH_OK)
            LOG("visibility: model rebuild hook failed (new pieces are hidden by the periodic check)");
    }
    if (have({kRvaEquipVisCheck})) {
        void* c = reinterpret_cast<void*>(g_base + kRvaEquipVisCheck);
        if (MH_CreateHook(c, reinterpret_cast<void*>(&equip_vis_check_hook), reinterpret_cast<void**>(&g_equip_vis_check_orig)) !=
                MH_OK ||
            MH_EnableHook(c) != MH_OK)
            LOG("visibility: drawn-weapon hook failed (hidden weapons stay hidden when drawn)");
    }
    LOG("visibility: ready (layer %u)", kLayer);
    return true;
}

void vis_queue(U64 inv)
{
    if (!inv) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (std::find(g_pending.begin(), g_pending.end(), inv) == g_pending.end()) g_pending.push_back(inv);
}

void vis_sync(U64 inv)
{
    if (!g_vis_set || !inv) return;
    U64 actor = rp(inv + 8);
    if (!actor || inventory_of(actor) != inv) { // (gone since)
        std::lock_guard<std::mutex> lock(g_mu);
        forget(inv);
        return;
    }
    U16 ch = character_of(actor);
    bool known;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        known = g_inv.count(inv) != 0;
    }
    // (every character the game decides headgear for comes here, NPCs too: only ours are looked at)
    const bool protagonist = ch == kKliff || ch == kDamiane || ch == kOongka;
    if (ch == kNoCharacter || (!known && !state_any(ch) && !(g_cfg.show_stowed_tools && protagonist))) return;
    CharState s = state_of(ch);
    if (!g_cfg.companions && actor != g_player_actor.load()) s = CharState{};
    U64 model = look_model(actor);
    if (!model) return;
    U64 list = rp(inv + kOffInvEquipList);
    U64 vc = rp(rp(rp(actor + kOffActorComp) + 0x40) + 0xE8); // (its in/out vis ctrl: the drawn check's first argument)
    std::set<int> hide;
    std::string log;
    std::lock_guard<std::mutex> lock(g_mu);
    std::set<U16> slots = s.hidden;
    // Shields: wherever worn. While another weapon takes the off hand the game carries the shield on the back
    // (equipment-23): "shown when equipped" hides it there only.
    if (s.shield != kShieldShown) slots.insert(kBackShieldSlot);
    for (U32 i = 0; s.shield == kShieldHidden && i < kMaxWheelSlots; ++i)
        if (U16 w = wheel_slot(kWeaponRecord, i); w != 0xFFFF) slots.insert(w);
    for (U16 slot : slots) {
        if (slot == kSlotHat && headgear_ready()) continue; // (the hat follows the headgear mode: the game's own switch)
        const bool weapon = is_weapon_slot(slot) || slot == kBackShieldSlot;
        if (!weapon && !s.hidden.count(slot)) continue; // (shields: weapon slots only)
        log += " equipment-" + std::to_string(slot);
        if (!list || !slot_worn(inv, slot)) {
            log += " empty";
            continue;
        }
        int h = slot_handle(inv, slot);
        if (!h) {
            log += " no part";
            continue;
        }
        if (!weapon) {
            hide.insert(h);
            log += " hidden";
            continue;
        }
        // A weapon: hidden while sheathed (drawn, it shows in the hand), its case (the scabbard on the back) always,
        // its extras (quiver and arrows...) with it. A slot that takes a sword or a shield hides each on its own.
        U64 part = look_part(model, h);
        std::string own = part ? part_name(r32(part + 0x48)) : std::string();
        const bool shield = is_shield_part(own);
        const bool want = shield ? s.shield == kShieldHidden || (s.shield == kShieldInHand && slot == kBackShieldSlot)
                                 : s.hidden.count(slot) != 0;
        if (!want) {
            log += " kept (" + own + ")";
            continue;
        }
        if (vc)
            for (U64 q : look_parts(model, h)) g_weapon_parts[{vc, r32(q + 0x48)}] = {inv, slot};
        bool drawn = false;
        if (auto it = g_drawn.find({inv, slot}); it != g_drawn.end()) drawn = it->second;
        if (!drawn) hide.insert(h);
        if (int c = case_handle(model, own)) hide.insert(c), log += " (+case)";
        for (int x : extra_handles(model, own))
            if (!drawn) hide.insert(x);
        log += drawn ? " drawn (" + own + ")" : " hidden (" + own + ")";
    }
    if (s.underwear) {
        int h = named_handle(model, "Underwear");
        if (h) hide.insert(h);
        log += h ? " underwear hidden" : " underwear (no part)";
    }
    // Tools the game only shows in the hand (Visible="Out"): shown on their stow socket too, unless hidden.
    if (vc) {
        for (auto it = g_stow.begin(); it != g_stow.end();) it = it->first.first == vc ? g_stow.erase(it) : std::next(it);
        for (U32 i = 0; g_cfg.show_stowed_tools && list && protagonist && i < kMaxWheelSlots; ++i) {
            const U16 slot = wheel_slot(kWeaponRecord, i);
            if (slot == 0xFFFF || s.hidden.count(slot) || !slot_worn(inv, slot)) continue;
            int h = slot_handle(inv, slot);
            U64 part = h ? look_part(model, h) : 0;
            std::string own = part ? part_name(r32(part + 0x48)) : std::string();
            if (own.rfind("Tool_", 0) != 0) continue;
            for (U64 q : look_parts(model, h)) g_stow[{vc, r32(q + 0x48)}] = inv;
            log += " equipment-" + std::to_string(slot) + " stowed shown (" + own + ")";
        }
    }
    InvInfo& info = g_inv[inv];
    if (info.model && info.model != model) g_model_inv.erase(info.model);
    info.model = model, info.ch = ch;
    g_model_inv[model] = inv;
    for (int h : hide)
        if (!hidden_by_us(model, h)) layer_call(model, h, 0);
    for (int h : info.applied)
        if (!hide.count(h)) layer_call(model, h, 2);
    const bool changed = hide != info.applied;
    info.applied = std::move(hide);
    publish_hot();
    // (a hidden cape no longer pushes the back weapons out; pieces hidden / shown since: the stack is laid out again)
    stack_note(model, ch, s.hidden.count(kSlotCloak) != 0, changed);
    if (log != info.last_log) {
        info.last_log = log;
        const char* nm = character_name(ch);
        LOG("visibility: %s (character %u):%s", nm ? nm : "?", ch, log.empty() ? " nothing hidden" : log.c_str());
    }
}

void vis_tick()
{
    std::vector<U64> todo;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        todo.swap(g_pending);
    }
    static U64 last = 0;
    U64 now = GetTickCount64();
    if (now - last > 750) { // (a periodic re-check: cheap, and covers anything the hooks didn't report)
        last = now;
        if (U64 inv = player_inventory()) todo.push_back(inv);
        std::lock_guard<std::mutex> lock(g_mu);
        for (const auto& [inv, info] : g_inv) todo.push_back(inv);
    }
    std::sort(todo.begin(), todo.end());
    todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
    for (U64 inv : todo) vis_sync(inv);
    stack_tick();
}

U16 vis_model_character(U64 model)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto m = g_model_inv.find(model);
    if (m == g_model_inv.end()) return kNoCharacter;
    auto i = g_inv.find(m->second);
    return i == g_inv.end() ? kNoCharacter : i->second.ch;
}

bool vis_slot_shield(U64 inv, U16 slot)
{
    if (!g_slot_handle || !inv) return false;
    U64 actor = rp(inv + 8);
    U64 model = actor ? look_model(actor) : 0;
    int h = model ? slot_handle(inv, slot) : 0;
    U64 part = h ? look_part(model, h) : 0;
    return part && is_shield_part(part_name(r32(part + 0x48)));
}

void vis_log_model(U64 inv)
{
    U64 actor = inv ? rp(inv + 8) : 0;
    U64 model = actor ? look_model(actor) : 0;
    if (!model) return;
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    std::string s;
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8)) {
            char b[24];
            std::snprintf(b, sizeof b, " %d=", static_cast<int>(r32(tab + i * 16ull)));
            s += b + part_name(r32(part + 0x48));
            std::snprintf(b, sizeof b, "[%08X]", r32(part + 0x70));
            s += b;
        }
    LOG("model of character %u (%u parts):%s", character_of(actor), n, s.c_str());
}

} // namespace ghw
