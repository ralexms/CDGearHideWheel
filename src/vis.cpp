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
#include "ui.h"

#include <optional>

namespace ghw {

namespace {

VisSetFn g_vis_set = nullptr;
VisReleaseFn g_vis_release = nullptr;
SlotHandleFn g_slot_handle = nullptr;
BattleVisFn g_battle_vis_orig = nullptr;
ModelSwapFn g_model_swap_orig = nullptr;
EquipVisCheckFn g_equip_vis_check_orig = nullptr;

std::mutex g_mu; // everything below (game thread, job threads, UI thread)
// The game calls back into our hooks from inside its setter while vis_sync holds g_mu (the stack builder asks which
// character a model is: vis_model_character). std::mutex throws when a thread locks it twice - that exception went
// up through the setter and cut it short, before it updated the attached objects (the backpacks). So the lock is
// taken only by a thread that doesn't hold it already.
thread_local bool t_own_mu = false;
class MuLock {
public:
    MuLock() : took_(!t_own_mu)
    {
        if (took_) g_mu.lock(), t_own_mu = true;
    }
    ~MuLock() { unlock(); }
    void unlock()
    {
        if (took_) t_own_mu = false, g_mu.unlock(), took_ = false;
    }
    MuLock(const MuLock&) = delete;
    MuLock& operator=(const MuLock&) = delete;

private:
    bool took_;
};
struct InvInfo {
    U64 model = 0;
    U16 ch = kNoCharacter;
    std::set<int> applied; // handles our layer hides now
    std::string last_log;
    int flight = 0;        // a mobility pack shown when in use: the part that says it is (Helm_Flight), watched each tick
    bool flying = false;   // ... what it said at the last sync
};
std::map<U64, InvInfo> g_inv;                                      // inventory -> what we did to its model
std::map<U64, U64> g_model_inv;                                    // model -> inventory
std::vector<U64> g_pending;                                        // inventories to sync on the next UI frame
std::atomic<bool> g_any_pending{false};                            // ... any (the per-tick check is this otherwise)

void queue_locked(U64 inv) // (g_mu held)
{
    if (std::find(g_pending.begin(), g_pending.end(), inv) == g_pending.end()) g_pending.push_back(inv);
    g_any_pending = true;
}
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

// A model's parts by name, read once per sync (each part's name is a string built from game memory: the lookups
// below share one pass over the table instead of one each).
struct PartIndex {
    std::vector<std::pair<int, std::string>> parts; // (handle, name)
    explicit PartIndex(U64 model)
    {
        U64 tab = rp(model + 0x58);
        U32 n = r32(model + 0x60);
        for (U32 i = 0; tab && i < n && i < 1024; ++i)
            if (U64 part = rp(tab + i * 16ull + 8))
                parts.emplace_back(static_cast<int>(r32(tab + i * 16ull)), part_name(r32(part + 0x48)));
    }
    int find(const std::string& want) const // its handle, or 0
    {
        for (const auto& [h, nm] : parts)
            if (nm == want) return h;
        return 0;
    }
};

// A weapon's case (scabbard) is a part of its own, named like the weapon with "IN" before its side
// (MainWeapon_Sword_R -> MainWeapon_Sword_IN_R, TwoHandWeapon_Sword -> TwoHandWeapon_Sword_IN). Its handle, or 0.
int case_handle(const PartIndex& idx, const std::string& weapon)
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
    return idx.find(want);
}

// Parts of their own that belong with a weapon and go with it (the bow's quiver and arrows, a lantern's ring).
std::vector<int> extra_handles(const PartIndex& idx, const std::string& weapon)
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
    for (const auto& [h, nm] : idx.parts)
        for (const char* b : *group)
            if (nm == b) v.push_back(h);
    return v;
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
// (the filter runs before the stack unwinds: the game frames that threw are still on it)
constexpr int kFaultFrames = 24;
void* g_fault_frames[kFaultFrames];
USHORT g_fault_nframes = 0;

int fault_filter(EXCEPTION_POINTERS* e, DWORD& code, U64& addr)
{
    code = e->ExceptionRecord->ExceptionCode;
    addr = reinterpret_cast<U64>(e->ExceptionRecord->ExceptionAddress);
    g_fault_nframes = RtlCaptureStackBackTrace(0, kFaultFrames, g_fault_frames, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

std::string fault_game_frames() // the captured frames inside the game: +RVA, innermost first
{
    std::string s;
    char b[24];
    const U64 lo = g_base, hi = g_base + game::main_module().size;
    for (USHORT i = 0; i < g_fault_nframes; ++i)
        if (U64 a = reinterpret_cast<U64>(g_fault_frames[i]); a >= lo && a < hi)
            s += (std::snprintf(b, sizeof b, " +%llX", a - lo), b);
    return s;
}

bool layer_call_raw(U64 model, int handle, int op, DWORD& code, U64& addr)
{
    struct {
        int* p;
        U32 n;
    } list{&handle, 1};
    __try {
        if (op == 2) g_vis_release(model, reinterpret_cast<U64>(&list), kLayer, 1, 0);
        else g_vis_set(model, reinterpret_cast<U64>(&list), 0, kLayer, 0, 0, 0);
        return true;
    } __except (fault_filter(GetExceptionInformation(), code, addr)) {
        return false;
    }
}

std::atomic<int> g_layer_faults{0};
bool layer_call(U64 model, int handle, int op)
{
    DWORD code = 0;
    U64 addr = 0;
    if (layer_call_raw(model, handle, op, code, addr)) return true;
    if (g_layer_faults.fetch_add(1) < 20) {
        HMODULE m = nullptr;
        wchar_t path[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &m))
            GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* file = wcsrchr(path, L'\\');
        LOG("visibility: the game's layer %s faulted on part %d of model %llX: exception %08lX at %llX (%ls +%llX); "
            "game frames:%s",
            op == 2 ? "release" : "setter", handle, model, code, addr, file ? file + 1 : path,
            m ? addr - reinterpret_cast<U64>(m) : 0, fault_game_frames().c_str());
    }
    return false;
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

// A backpack slot piece the character moves with (Oongka's rocket pack: Bag_Rocket). The utility packs (Disruptor,
// Explorer's Breathing Device) are all the plain Bag part; the slot's other parts (Bag_Belt, Bag_For_Dock...) belong
// to the body, not to the slot.
bool is_mobility_pack(const std::string& name) { return name != "Bag" && name.rfind("Bag_", 0) == 0; }

// A mobility pack is in use while the game shows its gear: the rocket pack's flight goggles (Helm_Flight) go on at
// take-off and off on landing, on the game's base layer 0 (read there alone, so a headgear mode hiding them doesn't
// matter).
bool in_use_part_out(U64 part)
{
    if (!part) return false;
    const U32 m = r32(part + 0x70);
    return (m & 0x10000) && (m & 1);
}

// --- Whether a part carries an attached object -------------------------------------------------------
// The setter (kRvaVisSet) asks +2F88A50 (part) whether the part carries an attached object; for those, a layer change
// is handed on to the model's attached-object manager (model+0x88). (1.0.0.3663; checked by its prologue.)
using PartAttachedFn = U8(__fastcall*)(U64 part);
constexpr U64 kRvaPartAttached = 0x2f88a50;
constexpr const char* kPartAttachedPattern =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 49 28 48 85 C9 74 ?? 48 8B 49 08 48 85 C9 74 ?? 80 79 15 00";

PartAttachedFn part_attached_fn()
{
    static const PartAttachedFn fn = game::matches(g_base + kRvaPartAttached, kPartAttachedPattern)
                                         ? reinterpret_cast<PartAttachedFn>(g_base + kRvaPartAttached)
                                         : nullptr;
    return fn;
}

int part_attached(U64 part) // 1 / 0: the game's answer; -1: the function wasn't found; -2: it faulted
{
    PartAttachedFn fn = part_attached_fn();
    if (!fn) return -1;
    __try {
        return fn(part);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// The game's model rebuild request: +2F91A80 (model, u8, u8, vec*, vec*) queues the job that rebuilds the model's drawn
// parts (its last step is kRvaModelSwap). The setter asks for it itself only when [model+0x208] is set, and a part with
// an attached object (a backpack) is redrawn only by that rebuild - so after hiding or showing one, we ask for it.
// (1.0.0.3663; checked by its prologue. The two vectors are empty ones, as the setter passes them.)
using ModelRebuildFn = void(__fastcall*)(U64 model, U8 a, U8 b, void* v1, void* v2);
constexpr U64 kRvaModelRebuild = 0x2f91a80;
constexpr const char* kModelRebuildPattern = "48 89 5C 24 20 44 88 44 24 18 88 54 24 10 55 56 57 41 54 41 55 41 56 41 57 "
                                             "48 8D AC 24 A0 FD FF FF 48 81 EC 60 03 00 00 49 8B F1 44 0F B6 FA 4C 8B F1";
std::atomic<bool> g_rebuild_off{false}; // (it faulted once: not called again this session)

ModelRebuildFn model_rebuild_fn()
{
    static const ModelRebuildFn fn = game::matches(g_base + kRvaModelRebuild, kModelRebuildPattern)
                                         ? reinterpret_cast<ModelRebuildFn>(g_base + kRvaModelRebuild)
                                         : nullptr;
    return fn;
}

bool request_model_rebuild(U64 model) // (g_mu not held: the job might run at once and reach the swap hook)
{
    ModelRebuildFn fn = model_rebuild_fn();
    if (!fn || g_rebuild_off.load()) return false;
    struct Vec {
        U64 p, n;
    } a{0, 0}, b{0, 0};
    __try {
        fn(model, 0, 0, &a, &b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_rebuild_off = true;
        return false;
    }
    if (a.p || b.p) LOG("visibility: the rebuild request handed vectors back (%llX %llX; left alone)", a.p, b.p);
    return true;
}

// Rebuild requests per model. A toggle asks at once. A hide put back on its own (the game made the part anew: the
// rocket pack after a flight, or after the wheel's preview) is spaced out, and stops for a while if the game keeps
// undoing it - each rebuild must not lead to the next.
// (set with g_mu held whenever a request is due or a part is watched: the per-tick check is this flag otherwise)
std::atomic<bool> g_any_timer{false};

struct Redraw {
    U64 last = 0;          // GetTickCount64 of the last request
    U64 due = 0;           // a request waits for this time (vis_tick)
    bool followup = false; // ... the one more after a request made while the game was rebuilding the model already
    U64 quiet_until = 0; // re-hides ask for none until then
    int strikes = 0;     // re-hide requests in a row, each soon after the one before
};
std::map<U64, Redraw> g_redraw; // model -> (g_mu)
constexpr U64 kRedrawGapMs = 1000, kRedrawStrikeMs = 4000, kRedrawQuietMs = 15000;
constexpr int kRedrawMaxStrikes = 10;

// (g_mu held) whether to ask now; false: not now (a later one may be due)
bool redraw_now(U64 model, bool toggle, U64 now)
{
    Redraw& r = g_redraw[model];
    if (!toggle) {
        if (now < r.quiet_until || r.due) return false; // (resting, or one already waits)
        r.strikes = r.last && now - r.last < kRedrawStrikeMs ? r.strikes + 1 : 0;
        if (r.strikes >= kRedrawMaxStrikes) {
            r.strikes = 0, r.quiet_until = now + kRedrawQuietMs;
            LOG("visibility: the game keeps showing a hidden backpack on model %llX again; no redraws for %llu s",
                model, kRedrawQuietMs / 1000);
            return false;
        }
        if (r.last && now - r.last < kRedrawGapMs) {
            r.due = r.last + kRedrawGapMs, g_any_timer = true;
            return false;
        }
    }
    r.last = now, r.due = 0;
    return true;
}

// Parts hidden or shown before they were ready (a backpack just put on, the rocket pack made anew): the game draws
// them as they finish, whatever their layers, so the redraw is asked for once they are ready.
std::map<std::pair<U64, int>, U64> g_watch; // (model, handle) -> GetTickCount64 to give up (g_mu)
constexpr U64 kWatchMs = 10000, kFollowupMs = 500;

bool part_ready(U64 part) { return part && !rp(part + 0x78) && part_attached(part) == 1; }

// (g_mu not held) followup: a request made while the game is rebuilding the model already (state not 0) may be taken
// into the rebuild under way, laid out before our change: one more is asked for shortly after
void do_redraw(U64 model, const std::string& what, const char* why, bool followup = true)
{
    const U32 big = r32(model + 0x208);
    U8 state = 0;
    rd(model + 0x7a, state);
    const bool ok = request_model_rebuild(model);
    if (g_cfg.verbose || !ok)
        LOG("visibility: %s %s on model %llX: %s (+208 %u, state %u)", what.c_str(), why, model,
        ok ? "asked the game to rebuild it" : g_rebuild_off ? "the rebuild request faulted (off now)" : "rebuild request not found",
        big, state);
    if (ok && followup && state != 0) {
        MuLock lock;
        Redraw& r = g_redraw[model];
        if (!r.due) r.due = GetTickCount64() + kFollowupMs, r.followup = true, g_any_timer = true;
    }
}

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
    MuLock lock;
    if (auto it = g_model_inv.find(model); it != g_model_inv.end()) {
        queue_locked(it->second);
        if (g_cfg.verbose) {
            const auto inf = g_inv.find(it->second);
            const char* nm = inf != g_inv.end() ? character_name(inf->second.ch) : nullptr;
            LOG("visibility: the game rebuilt the model of %s (%llX), phase %d", nm ? nm : "?", model, static_cast<int>(c));
        }
    }
}

// The game's in/out check (drawing / sheathing, per part): a hidden weapon drawn shows in the hand, and goes again
// when it is put away. Synced on the next UI frame (the game is in the middle of laying the parts out here).
float __fastcall equip_vis_check_hook(U64 vc, const U32* part, U8* rec, U8 drawn)
{
    if (!hot(vc)) return g_equip_vis_check_orig(vc, part, rec, drawn); // (not one of ours: no lock, no lookups)
    U32 name = 0;
    const bool named = part && rd(reinterpret_cast<U64>(part), name);
    if (named && rec) {
        MuLock lock;
        U8 b = 0;
        if (!g_stow.empty() && g_stow.count({vc, name}) && rd(reinterpret_cast<U64>(rec) + kOffInOutDisplay, b) && (b & 2))
            wr<U8>(reinterpret_cast<U64>(rec) + kOffInOutDisplay, 0);
    }
    float r = g_equip_vis_check_orig(vc, part, rec, drawn);
    if (!named) return r;
    MuLock lock;
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
    queue_locked(owner.first);
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
    MuLock lock;
    queue_locked(inv);
}

void vis_sync(U64 inv)
{
    if (!g_vis_set || !inv) return;
    if (t_own_mu) { // (called back from inside a sync of ours, through the game: the next UI frame does it)
        queue_locked(inv);
        return;
    }
    U64 actor = rp(inv + 8);
    if (!actor || inventory_of(actor) != inv) { // (gone since)
        MuLock lock;
        forget(inv);
        return;
    }
    U16 ch = character_of(actor);
    bool known;
    {
        MuLock lock;
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
    int flight = 0;      // (a mobility pack shown when in use: see InvInfo)
    bool flying = false;
    std::optional<PartIndex> parts; // (read on first use: most syncs need no part by name)
    auto index = [&]() -> const PartIndex& {
        if (!parts) parts.emplace(model);
        return *parts;
    };
    MuLock lock;
    std::set<U16> slots = s.hidden;
    // Shields: wherever worn. While another weapon takes the off hand the game carries the shield on the back
    // (equipment-23): "shown when equipped" hides it there only.
    if (s.shield != kShieldShown) slots.insert(kBackShieldSlot);
    for (U32 i = 0; s.shield == kShieldHidden && i < kMaxWheelSlots; ++i)
        if (U16 w = wheel_slot(kWeaponRecord, i); w != 0xFFFF) slots.insert(w);
    if (s.pack != kPackShown) slots.insert(kSlotBag); // (mobility packs: their own mode, apart from the slot's entry)
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
        // The backpack slot: a mobility pack follows its own mode, any other pack the slot's entry.
        const bool mobility = slot == kSlotBag && is_mobility_pack(own);
        const bool want = shield     ? s.shield == kShieldHidden || (s.shield == kShieldInHand && slot == kBackShieldSlot)
                          : mobility ? s.pack != kPackShown
                                     : s.hidden.count(slot) != 0;
        if (!want) {
            log += " kept (" + own + ")";
            continue;
        }
        if (vc)
            for (U64 q : look_parts(model, h)) g_weapon_parts[{vc, r32(q + 0x48)}] = {inv, slot};
        bool drawn = false;
        if (auto it = g_drawn.find({inv, slot}); it != g_drawn.end()) drawn = it->second;
        if (mobility) { // (the in/out check never sees these: "in use" is the flight gear the game puts on)
            drawn = false;
            if (s.pack == kPackInUse) {
                flight = index().find("Helm_Flight");
                drawn = flying = flight && in_use_part_out(look_part(model, flight));
            }
        }
        if (!drawn) hide.insert(h);
        if (int c = case_handle(index(), own)) hide.insert(c), log += " (+case)";
        for (int x : extra_handles(index(), own))
            if (!drawn) hide.insert(x);
        if (mobility && s.pack == kPackInUse)
            log += drawn ? " in use, shown (" + own + ")" : flight ? " stowed, hidden (" + own + ")" : " hidden, no Helm_Flight (" + own + ")";
        else log += drawn ? " drawn (" + own + ")" : " hidden (" + own + ")";
    }
    if (s.underwear) {
        int h = index().find("Underwear");
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
    info.model = model, info.ch = ch, info.flight = flight, info.flying = flying;
    g_model_inv[model] = inv;
    // The parts whose layer we just set. (A call that faulted counts too: the game's setter throws now and then while
    // the model is being rebuilt, after the layer is already set - the part must still be redrawn.)
    std::set<int> touched;
    for (int h : hide)
        if (!hidden_by_us(model, h)) layer_call(model, h, 0), touched.insert(h);
    for (int h : info.applied)
        if (!hide.count(h)) layer_call(model, h, 2), touched.insert(h);
    const U64 now = GetTickCount64();
    const bool changed = hide != info.applied;
    // A piece carrying an attached object (the backpacks: Bag, Bag_Rocket) hidden or shown: the game redraws it only
    // when it rebuilds the model, which the setter doesn't ask for here. That goes for a toggle, and for a hide put
    // back because the game made the part anew (the rocket pack after flying). (The cape is one too, but its own
    // rules already make the game rebuild the model.)
    std::string redraw;
    bool toggle = false;
    {
        std::set<int> diff;
        for (int h : hide)
            if (!info.applied.count(h)) diff.insert(h);
        for (int h : info.applied)
            if (!hide.count(h)) diff.insert(h);
        std::set<int> look = diff;
        look.insert(touched.begin(), touched.end());
        for (int h : look) {
            U64 part = look_part(model, h);
            if (!part_ready(part)) { // (not ready yet, or not a piece with an attached object: vis_tick looks again)
                g_watch[{model, h}] = now + kWatchMs, g_any_timer = true;
                continue;
            }
            g_watch.erase({model, h});
            std::string nm = part_name(r32(part + 0x48));
            if (nm == "Cloak") continue;
            const bool t = diff.count(h) != 0;
            toggle |= t;
            redraw += (redraw.empty() ? "" : ", ") + nm + (t ? "" : " (hidden again)");
        }
    }
    const bool ask = !redraw.empty() && redraw_now(model, toggle, now);
    info.applied = std::move(hide);
    publish_hot();
    // (a hidden cape no longer pushes the back weapons out; pieces hidden / shown since: the stack is laid out again)
    stack_note(model, ch, s.hidden.count(kSlotCloak) != 0, changed);
    if (log != info.last_log) {
        info.last_log = log;
        const char* nm = character_name(ch);
        LOG("visibility: %s (character %u):%s", nm ? nm : "?", ch, log.empty() ? " nothing hidden" : log.c_str());
    }
    if (!redraw.empty() && !ask && g_cfg.verbose) LOG("visibility: %s on model %llX: redraw held back", redraw.c_str(), model);
    if (!ask) return;
    lock.unlock();
    do_redraw(model, redraw, "changed");
}

// The UI tick: runs many times a frame (the frame hook is a shared per-element getter), so with nothing to do it is
// a few atomic reads and a clock read - each piece of work below is gated by a flag or a time.
void vis_tick()
{
    std::vector<U64> todo;
    if (g_any_pending.exchange(false)) {
        MuLock lock;
        todo.swap(g_pending);
    }
    U64 now = GetTickCount64();
    static U64 last = 0;
    if (now - last > 750) { // (a periodic re-check: covers anything the hooks didn't report)
        last = now;
        if (U64 inv = player_inventory()) todo.push_back(inv);
        MuLock lock;
        for (const auto& [inv, info] : g_inv) todo.push_back(inv);
    }
    if (!todo.empty()) {
        std::sort(todo.begin(), todo.end());
        todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
        for (U64 inv : todo) vis_sync(inv);
    }
    // A mobility pack shown when in use, on the character played (the one who flies): its flight gear went on or off.
    // The game rebuilds the model at take-off and landing, which syncs it already; this catches one that didn't.
    static U64 last_flight = 0;
    if (now - last_flight >= 250) {
        last_flight = now;
        const U64 player = g_player_actor.load();
        std::vector<U64> flips;
        {
            MuLock lock;
            for (auto& [inv, info] : g_inv)
                if (info.flight && info.model && player && rp(inv + 8) == player)
                    if (const bool out = in_use_part_out(look_part(info.model, info.flight)); out != info.flying)
                        info.flying = out, flips.push_back(inv); // (kept here too: a sync cut short can't make it spin)
        }
        for (U64 inv : flips) {
            if (g_cfg.verbose) LOG("visibility: a mobility pack's flight gear went %s", "on / off (missed by the rebuild)");
            vis_sync(inv);
        }
    }
    // Redraws held back to space them out, follow-ups, and parts that weren't ready.
    if (g_any_timer.load()) {
        std::vector<std::pair<U64, bool>> due; // model, a follow-up
        std::map<U64, std::string> ready;      // model -> the parts ready now
        {
            MuLock lock;
            bool more = false;
            for (auto it = g_redraw.begin(); it != g_redraw.end();) {
                if (!g_model_inv.count(it->first)) { // (a model gone since)
                    it = g_redraw.erase(it);
                    continue;
                }
                if (it->second.due && now >= it->second.due) {
                    due.push_back({it->first, it->second.followup});
                    it->second.last = now, it->second.due = 0, it->second.followup = false;
                }
                more |= it->second.due != 0;
                ++it;
            }
            for (auto it = g_watch.begin(); it != g_watch.end();) {
                const auto [model, h] = it->first;
                if (now > it->second || !g_model_inv.count(model)) {
                    it = g_watch.erase(it);
                    continue;
                }
                U64 part = look_part(model, h);
                if (!part_ready(part)) {
                    ++it;
                    continue;
                }
                std::string nm = part_name(r32(part + 0x48));
                if (nm != "Cloak") ready[model] += (ready[model].empty() ? "" : ", ") + nm;
                it = g_watch.erase(it);
            }
            g_any_timer = more || !g_watch.empty(); // (set again under the lock by whatever adds one)
        }
        for (const auto& [model, followup] : due)
            if (!ready.count(model))
                do_redraw(model, "backpack", followup ? "again (the model was being rebuilt)" : "hidden again (held back)", false);
        for (const auto& [model, what] : ready) do_redraw(model, what, "ready now");
    }
    stack_tick();
}

U16 vis_model_character(U64 model)
{
    MuLock lock;
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
            if (rp(part + 0x78)) s += "(loading)";
            // (its children: name and layers, {..} - a piece whose mesh hangs off a socket part shows here)
            U64 kids = rp(part + 0x18);
            U32 c = r32(part + 0x20);
            if (kids && c) {
                s += "{";
                for (U32 k = 0; k < c && k < 16; ++k)
                    if (U64 kid = rp(kids + k * 8ull)) {
                        std::snprintf(b, sizeof b, "[%08X]", r32(kid + 0x70));
                        s += (k ? " " : "") + part_name(r32(kid + 0x48)) + b;
                    }
                s += "}";
            }
        }
    LOG("model of character %u (%u parts):%s", character_of(actor), n, s.c_str());
    // (the worn pieces, each slot's part: which part a slot leads to, and what is worn)
    U64 list = rp(inv + kOffInvEquipList);
    U64 e = list ? rp(list + 8) : 0;
    std::string w;
    for (U32 i = 0, m = list ? r32(list + 0x10) : 0; e && i < m && i < 64; ++i, e += 0xd0) {
        U16 slot = 0xFFFF, key = 0xFFFF;
        if (!rd(e + kOffEntrySlot, slot) || slot == 0xFFFF || !slot_worn(inv, slot)) continue;
        rd(e + 8, key);
        int h = slot_handle(inv, slot);
        U64 part = h ? look_part(model, h) : 0;
        w += " equipment-" + std::to_string(slot) + "=" + std::to_string(h) + " " +
             (part ? part_name(r32(part + 0x48)) : std::string("(no part)")) + " (" + item_name(key) + ")";
    }
    LOG("worn by character %u:%s", character_of(actor), w.c_str());
}

bool vis_slot_mobility(U64 inv, U16 slot)
{
    if (slot != kSlotBag || !g_slot_handle || !inv) return false;
    U64 actor = rp(inv + 8);
    U64 model = actor ? look_model(actor) : 0;
    int h = model ? slot_handle(inv, slot) : 0;
    U64 part = h ? look_part(model, h) : 0;
    return part && is_mobility_pack(part_name(r32(part + 0x48)));
}

} // namespace ghw
