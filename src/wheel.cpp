// The gear wheel: the highlighted piece, the toggle presses, the prompt in the centre tooltip.
//
// The wheel is the game's RootQuickSlot. Its tabs' slot scripts sit in per-tab vectors (+0x1F0) that line up with the
// QuickSlotInfo records (0 = Weapons, 1 = Armour), whose slots name the equipment slot of each position: so the
// highlighted script gives the equipment slot. (Layout and offsets from Outfit Manager.) Its callbacks are hooked in
// its vtable, chained to whatever is there already - Outfit Manager's hooks, when it is installed.
#include "common.h"
#include "wheel.h"
#include "addrs.h"
#include "config.h"
#include "gear.h"
#include "headgear.h"
#include "input.h"
#include "state.h"
#include "ui.h"
#include "vis.h"

namespace ghw {

namespace {

OpenFn g_open_orig = nullptr;
TabFn g_close_orig = nullptr;
TabFn g_focus_orig = nullptr;
FloatFn g_frame_orig = nullptr;
FloatFn g_hud_frame_orig = nullptr;

DWORD g_ui_thread = 0;
U64 g_root = 0;      // the wheel while it is open
bool g_open = false;
int g_caption_frames = 0; // re-assert the prompt for a while after a focus change (the game refreshes late)
bool g_shown_pad = false; // the prompt names the controller's buttons

struct Target {
    bool ok = false;
    U32 tab = 0, index = 0;
    U16 slot = 0xFFFF;
};

bool in_depth(U64 self) // (the lantern-style submenu is open: its entries are not the wheel's slots)
{
    U8 v = 0;
    return kOffRootFocusIdx && rd(self + kOffRootFocusIdx, v) && v;
}

U64 focused_script(U64 self)
{
    U8 mode = 0;
    if (!g_focused_slot || !rd(self + kOffRootFocusIdx, mode)) return 0;
    __try {
        return g_focused_slot(self, mode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The gear wheel proper (the same screen also shows the comrades' and items' wheels): its Weapons header is visible.
bool gear_mode(U64 self)
{
    U64 tb = rp(self + kOffRootTabBar);
    U64 weapons = tb ? rp(rp(tb + kOffTabHeaders)) : 0;
    U64 we = weapons ? rp(weapons + 8) : 0;
    return we && is_element(we) && !has_class(we, "cpp-hide") && !has_class(we, "!cpp-none");
}

Target target_of(U64 self, U64 script)
{
    Target t;
    if (!script || in_depth(self) || !gear_mode(self)) return t;
    U64 tb = rp(self + kOffRootTabBar);
    U32 tab = tb ? r32(tb + kOffTabCurrent) : ~0u;
    if (tab > kArmorRecord) return t; // (Outfit Manager's Outfits tab, or something else)
    U64 sv = rp(self + kOffRootSlotVecs);
    if (!sv || tab >= r32(self + kOffRootSlotVecCount)) return t;
    U64 arr = rp(sv + tab * 0x10ull);
    U32 cnt = r32(sv + tab * 0x10ull + 8);
    for (U32 i = 0; arr && i < cnt && i < kMaxWheelSlots; ++i)
        if (rp(arr + i * 8ull) == script) {
            t.tab = tab, t.index = i, t.slot = wheel_slot(tab, i);
            t.ok = t.slot != 0xFFFF;
            return t;
        }
    return t;
}

const char* slot_name(U16 slot)
{
    switch (slot) {
    case 0: return "Main hand";
    case 1: return "Off hand";
    case 2: return "Bow";
    case 3: return "Headgear";
    case 4: return "Chest";
    case 5: return "Gloves";
    case 6: return "Boots";
    case 7: case 8: return "Earring";
    case 9: return "Necklace";
    case 10: case 11: return "Ring";
    case 16: return "Cloak";
    case 17: return "Circlet";
    case 18: return "Mask";
    case kBackShieldSlot: return "Back shield";
    }
    return nullptr;
}

std::string piece_name(U64 inv, U16 slot)
{
    U64 list = inv ? rp(inv + kOffInvEquipList) : 0;
    U64 e = list ? equipped_item(list, slot) : 0;
    U16 key = 0xFFFF;
    std::string n = e && rd(e + 8, key) ? item_name(key) : std::string();
    if (!n.empty()) return n;
    if (const char* s = slot_name(slot)) return s;
    return "equipment-" + std::to_string(slot);
}

bool headgear_slot(U16 slot) { return slot == kSlotHat && headgear_ready(); }
bool weapon_slot(U16 slot) { return is_weapon_slot(slot) || slot == kBackShieldSlot; }

// A weapon slot that takes a shield or another weapon (Kliff's and Damiane's off hand): the secondary key cycles the
// character's shield mode there, the toggle key covers the rest. Shown for the off hand (equipment-1: sword or
// shield) and any slot holding a shield now.
bool shield_slot(U64 inv, U16 slot)
{
    constexpr U16 kOffHand = 1; // (the slot the game's shields go in: "equipment-1" in the logs)
    return weapon_slot(slot) && (slot == kOffHand || vis_slot_shield(inv, slot));
}

std::string key_hint(const Binding& b)
{
    std::string k = binding_text(b, g_pad_in_use);
    return k.empty() ? std::string() : " [" + k + "]";
}

std::string state_part(const char* label, bool hidden, const Binding& b)
{
    return std::string(label ? label : "") + (label ? ": " : "") + (hidden ? "Hidden" : "Shown") + key_hint(b);
}

// One line on the tooltip's item name: "Leather Cloak - Hidden [H]", "Iron Helm - Shown in combat [H]",
// "Oak Shield - Shield: Hidden [U] | Weapon: Shown [H]", "Leather Armor - Shown [H] | Underwear: Hidden [U]".
std::string caption_for(const Target& t, U16 ch, U64 inv)
{
    std::string s; // (the state / key part; the item's name goes in front)
    if (headgear_slot(t.slot)) {
        int m = state_headgear(ch);
        s += (m >= 0 ? std::string(headgear_mode_text(m)) : std::string("Game setting: ") + headgear_mode_text(game_headgear_option())) +
             key_hint(g_cfg.toggle);
    } else if (t.slot == kSlotBag && !g_cfg.secondary.empty()) { // (the worn kind first)
        std::string mob = std::string("Mobility packs: ") + pack_mode_text(state_pack(ch)) + key_hint(g_cfg.secondary);
        std::string ut = state_part("Utility packs", state_hidden(ch, t.slot), g_cfg.toggle);
        s += vis_slot_mobility(inv, t.slot) ? mob + "  |  " + ut : ut + "  |  " + mob;
    } else if (shield_slot(inv, t.slot) && !g_cfg.secondary.empty()) {
        std::string sh = std::string("Shield: ") + shield_mode_text(state_shield(ch)) + key_hint(g_cfg.secondary);
        std::string we = state_part("Weapon", state_hidden(ch, t.slot), g_cfg.toggle);
        s += vis_slot_shield(inv, t.slot) ? sh + "  |  " + we : we + "  |  " + sh;
    } else {
        s += state_part(nullptr, state_hidden(ch, t.slot), g_cfg.toggle);
    }
    if (t.slot == kSlotChest && !g_cfg.secondary.empty())
        s += "  |  " + state_part("Underwear", state_underwear(ch), g_cfg.secondary);
    return piece_name(inv, t.slot) + "  -  " + s;
}

// The prompt is the tooltip's item name, rewritten (the game sets it again on every focus).
U64 caption_elem(U64 self)
{
    U64 tip = rp(self + kOffRootCenterTip);
    U64 te = tip ? rp(tip + 8) : 0;
    return te && is_element(te) ? find_by_class(te, "quickslot-center-tooltip-name", 12) : 0;
}

void update_caption(U64 self)
{
    if (!g_cfg.show_prompt) return;
    Target t = target_of(self, focused_script(self));
    U16 ch = t.ok ? current_character() : kNoCharacter;
    U64 inv = t.ok ? player_inventory() : 0;
    if (!t.ok || ch == kNoCharacter || !inv) return;
    U64 e = caption_elem(self);
    if (!e) {
        static bool told = false;
        if (!told) told = true, LOG("prompt: the centre tooltip's name line was not found; no prompt");
        return;
    }
    std::string text = caption_for(t, ch, inv);
    if (!set_text(e, text.c_str())) {
        static bool told = false;
        if (!told) told = true, LOG("prompt: setting the tooltip's text failed");
        return;
    }
    static std::string logged;
    if (g_cfg.verbose && text != logged) logged = text, LOG("prompt: %s", text.c_str());
}

void press(U64 self, bool toggle, bool secondary)
{
    Target t = target_of(self, focused_script(self));
    if (!t.ok) {
        if (g_cfg.verbose) LOG("press: no gear slot highlighted");
        return;
    }
    U16 ch = current_character();
    U64 inv = player_inventory();
    if (ch == kNoCharacter || !inv) return;
    const char* who = character_name(ch);
    std::string msg = std::string(who ? who : ("Character " + std::to_string(ch)).c_str()) + " - ";
    const size_t bare = msg.size();
    auto add = [&](const std::string& m) { msg += (msg.size() > bare ? "; " : "") + m; };
    const bool weapon = weapon_slot(t.slot), shield_now = weapon && vis_slot_shield(inv, t.slot);
    if (toggle && headgear_slot(t.slot)) {
        int cur = state_headgear(ch);
        if (cur < 0) cur = game_headgear_option();
        const std::vector<int>& cyc = g_cfg.headgear_cycle;
        auto it = std::find(cyc.begin(), cyc.end(), cur);
        int next = it == cyc.end() || it + 1 == cyc.end() ? cyc.front() : *(it + 1);
        state_set_headgear(ch, next);
        refresh_headgear(inv);
        add(std::string("Headgear: ") + headgear_mode_text(next));
    } else if (toggle) {
        bool hidden = !state_hidden(ch, t.slot);
        state_set_hidden(ch, t.slot, hidden);
        // (on a slot holding a shield, the toggle covers the slot's other weapons; on the backpack slot holding a
        // mobility pack, the utility packs: say so)
        add((shield_now                                                ? std::string("Weapons in this slot (not shields)")
             : t.slot == kSlotBag && vis_slot_mobility(inv, t.slot) ? std::string("Utility packs")
                                                                     : piece_name(inv, t.slot)) +
            (hidden ? ": hidden" : ": shown"));
    }
    if (secondary && t.slot == kSlotChest) {
        bool hidden = !state_underwear(ch);
        state_set_underwear(ch, hidden);
        add(hidden ? "Underwear: hidden" : "Underwear: shown");
    } else if (secondary && t.slot == kSlotBag) { // (mobility packs: Shown -> Hidden -> Shown when in use -> Shown)
        int next = (state_pack(ch) + 1) % kPackModeCount;
        state_set_pack(ch, next);
        add((vis_slot_mobility(inv, t.slot) ? piece_name(inv, t.slot) : std::string("Mobility packs")) + ": " +
            pack_mode_text(next));
    } else if (secondary && weapon) { // (Shown -> Hidden -> Shown when equipped -> Shown)
        int next = (state_shield(ch) + 1) % kShieldModeCount;
        state_set_shield(ch, next);
        add((shield_now ? piece_name(inv, t.slot) : std::string("Shields")) + ": " + shield_mode_text(next));
    }
    if (msg.size() == bare) return;
    vis_sync(inv);
    LOG("press on tab %u position %u (equipment-%u): %s", t.tab, t.index, t.slot, msg.c_str());
    if (g_cfg.verbose) vis_log_model(inv);
    if (g_cfg.notifications || !g_cfg.show_prompt) notify(self, msg);
    g_caption_frames = 30;
    update_caption(self);
}

void tick_common()
{
    static U64 last = 0;
    U64 now = GetTickCount64();
    if (now - last > 250) last = now, current_character(); // (keeps the controlled actor known to the hooks)
    vis_tick();
}

U64 __fastcall open_hook(U64 self, U64 a, U64 b, U64 c)
{
    U64 r = g_open_orig(self, a, b, c);
    g_ui_thread = GetCurrentThreadId();
    g_root = self, g_open = true;
    set_listen(true);
    g_caption_frames = 30;
    collect_wheel_slots();
    static int logged_option = -2;
    if (int o = game_headgear_option(); o != logged_option) {
        logged_option = o;
        LOG("wheel open; the game's headgear setting is %d (%s)", o, headgear_mode_text(o));
    }
    take_toggle(), take_secondary(); // (presses from before the wheel opened)
    return r;
}

U64 __fastcall close_hook(U64 self, U64 a, U64 b, U64 c, U64 d, U64 e)
{
    if (self == g_root) g_open = false, set_listen(false);
    return g_close_orig(self, a, b, c, d, e);
}

U64 __fastcall focus_hook(U64 self, U64 script, U64 a, U64 b, U64 c, U64 d)
{
    U64 r = g_focus_orig(self, script, a, b, c, d);
    if (self == g_root && g_open) {
        g_caption_frames = 30;
        update_caption(self);
    }
    return r;
}

float __fastcall frame_hook(U64 self)
{
    if (self == g_root && g_open && GetCurrentThreadId() == g_ui_thread) {
        bool t = take_toggle(), u = take_secondary();
        if (t || u) press(self, t, u);
        static int n = 0;
        bool pad = g_pad_in_use;
        if (g_caption_frames > 0 || pad != g_shown_pad || ++n % 10 == 0) {
            g_shown_pad = pad;
            update_caption(self);
        }
        if (g_caption_frames > 0) --g_caption_frames;
        tick_common();
    }
    return g_frame_orig(self);
}

float __fastcall hud_frame_hook(U64 self)
{
    if (!g_ui_thread) g_ui_thread = GetCurrentThreadId();
    if (GetCurrentThreadId() == g_ui_thread && !g_open) tick_common();
    return g_hud_frame_orig(self);
}

bool patch(void** vt, int slot, void* fn, void** orig)
{
    void* cur = vt[slot];
    if (!plausible(reinterpret_cast<U64>(cur))) return false;
    *orig = cur; // (before the patch: the game may call it at once)
    return game::patch_vtable(vt, slot, fn) != nullptr;
}

} // namespace

bool install_wheel()
{
    if (!have({kRvaRootQuickSlotVt, kRvaFocusedSlot, kRvaQuickSlotInfoGet})) {
        LOG("wheel: the gear wheel was not found; no toggling");
        return false;
    }
    g_focused_slot = reinterpret_cast<FocusedSlotFn>(g_base + kRvaFocusedSlot);
    g_qsi_get = reinterpret_cast<RecordGetFn>(g_base + kRvaQuickSlotInfoGet);
    void** vt = reinterpret_cast<void**>(g_base + kRvaRootQuickSlotVt);
    struct P {
        int slot;
        void* fn;
        void** orig;
        const char* name;
    } ps[] = {
        {kSlotOpen, reinterpret_cast<void*>(&open_hook), reinterpret_cast<void**>(&g_open_orig), "open"},
        {kSlotClose, reinterpret_cast<void*>(&close_hook), reinterpret_cast<void**>(&g_close_orig), "close"},
        {kSlotFocus, reinterpret_cast<void*>(&focus_hook), reinterpret_cast<void**>(&g_focus_orig), "focus"},
        {kSlotFrame, reinterpret_cast<void*>(&frame_hook), reinterpret_cast<void**>(&g_frame_orig), "frame"},
    };
    for (size_t i = 0; i < std::size(ps); ++i)
        if (!patch(vt, ps[i].slot, ps[i].fn, ps[i].orig)) {
            LOG("wheel: vtable slot %d (%s) could not be hooked", ps[i].slot, ps[i].name);
            for (size_t k = 0; k < i; ++k) game::patch_vtable(vt, ps[k].slot, *ps[k].orig);
            return false;
        }
    LOG("wheel: hooked open %d, close %d, focus %d, frame %d (chained to %p %p %p %p)", kSlotOpen, kSlotClose, kSlotFocus,
        kSlotFrame, reinterpret_cast<void*>(g_open_orig), reinterpret_cast<void*>(g_close_orig),
        reinterpret_cast<void*>(g_focus_orig), reinterpret_cast<void*>(g_frame_orig));
    if (have({kRvaHudVt})) { // (a UI tick with the wheel closed: new pieces, companions)
        void** hvt = reinterpret_cast<void**>(g_base + kRvaHudVt);
        if (!patch(hvt, kSlotFrame, reinterpret_cast<void*>(&hud_frame_hook), reinterpret_cast<void**>(&g_hud_frame_orig)))
            LOG("wheel: the HUD frame could not be hooked (changes made outside the wheel wait for it)");
    }
    return true;
}

} // namespace ghw
