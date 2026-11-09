// The back equipment stack: a hidden cape no longer pushes the weapons on the back outward (see stack.h and
// docs/cape-offset-research.md).
//
// CrimsonDesert.exe 1.0.0.3576:
//   +2F2A410 (model, SocketBoneData**)  builds the model's stacks from the socket data's StackEquipInfo records
//                                       (vector at +0x48, count +0x50; 0x40-byte records, inner part ids +0x08 / +0x10)
//   +E79C110 (model)                    the game's own caller: finds the model's socket data, checks it is loaded,
//                                       builds the stacks, then applies them (+2F2A840)
// The builder only reads the record vector of the socket data it is handed, so for a hidden cape it is handed a copy
// of the records with the Cloak part left out of their inner parts: the stack is then sized by the chest piece alone.
#include "common.h"
#include "stack.h"
#include "addrs.h"
#include "config.h"
#include "gear.h"
#include "state.h"
#include "vis.h"

namespace ghw {

namespace {

using BuildFn = U64(__fastcall*)(U64 model, U64 socket_data_slot);
using RebuildFn = U64(__fastcall*)(U64 model);

constexpr U64 kRvaBuild = 0x2f2a410, kRvaRebuild = 0xe79c110;
constexpr const char* kBuildPattern =
    "48 89 4C 24 08 53 56 41 55 41 56 48 83 EC 78 4C 8B 32 4C 8D 69 20 49 83 C6 48 48 8B F1"; // (add r14, 0x48: the records)
constexpr const char* kRebuildPattern =
    "40 53 48 83 EC 20 48 8B 41 50 48 89 CB 8B 0D ?? ?? ?? ?? 81 E9 98 FF FF FF 48 8B 50 08 48 85 D2";
constexpr U64 kOffRecords = 0x48, kOffRecordCount = 0x50, kRecordSize = 0x40;
constexpr U64 kOffRecInner = 0x08, kOffRecInnerCount = 0x10, kOffRecInnerCap = 0x14;

BuildFn g_build_orig = nullptr;
RebuildFn g_rebuild = nullptr;
U32 g_cloak_id = 0; // the part name id of "Cloak", as the socket data's InnerPartNames hold it

// A filtered copy of a socket data's records, kept alive while the model's stacks may point into it.
struct Filtered {
    std::vector<U8> obj;  // stands in for the SocketBoneData: only +0x48 / +0x50 are read
    std::vector<U8> recs; // the records, inner lists replaced
    std::vector<std::vector<U32>> inner;
};
struct ModelInfo {
    bool built = false;    // the game built this model's stacks through the hook
    bool filtered = false; // ... without the cape
    bool pending = false;  // to be rebuilt on the next UI frame
    bool failed = false;   // wanted without the cape, but the copy failed (not retried until the game builds it again)
    U32 builds = 0;        // builds through the hook (a rebuild that didn't reach the builder doesn't count)
    int stuck = 0;         // rebuilds in a row that didn't reach the builder (given up at 3)
    U16 ch = kNoCharacter;
    Filtered keep;
};
std::mutex g_mu;
std::map<U64, ModelInfo> g_models;     // the protagonists' models only (a handful)
std::atomic<bool> g_any_pending{false}; // a rebuild waits: the per-frame tick is this one check otherwise
constexpr size_t kMaxModels = 16;       // (models of earlier loads are dropped past this)
std::atomic<int> g_logged{0};

bool copy_bytes(void* dst, U64 src, size_t n)
{
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool make_filtered(U64 sbd, Filtered& f, int& removed)
{
    U64 recs = rp(sbd + kOffRecords);
    U32 n = r32(sbd + kOffRecordCount);
    if (!recs || !n || n > 64) return false;
    f.recs.resize(size_t(n) * kRecordSize);
    if (!copy_bytes(f.recs.data(), recs, f.recs.size())) return false;
    f.inner.assign(n, {});
    for (U32 i = 0; i < n; ++i) {
        U8* r = f.recs.data() + size_t(i) * kRecordSize;
        U64 ip = 0;
        U32 ic = 0;
        std::memcpy(&ip, r + kOffRecInner, 8);
        std::memcpy(&ic, r + kOffRecInnerCount, 4);
        if (ic > 32 || (ic && !plausible(ip))) return false;
        for (U32 k = 0; k < ic; ++k) {
            U32 id = r32(ip + 4ull * k);
            if (id == g_cloak_id) ++removed;
            else f.inner[i].push_back(id);
        }
        U64 np = f.inner[i].empty() ? 0 : reinterpret_cast<U64>(f.inner[i].data());
        U32 cnt = static_cast<U32>(f.inner[i].size());
        std::memcpy(r + kOffRecInner, &np, 8);
        std::memcpy(r + kOffRecInnerCount, &cnt, 4);
        std::memcpy(r + kOffRecInnerCap, &cnt, 4);
    }
    f.obj.assign(0x60, 0);
    U64 rp_ = reinterpret_cast<U64>(f.recs.data());
    std::memcpy(f.obj.data() + kOffRecords, &rp_, 8);
    std::memcpy(f.obj.data() + kOffRecordCount, &n, 4);
    std::memcpy(f.obj.data() + kOffRecordCount + 4, &n, 4);
    return true;
}

bool protagonist(U16 ch) { return ch == kKliff || ch == kDamiane || ch == kOongka; }

// The cape of this character is hidden by the mod, and the fix applies to this model.
bool wants_filter(U16 ch, U64 model)
{
    if (!g_cfg.cape_offset_fix || !protagonist(ch) || !state_hidden(ch, kSlotCloak)) return false;
    if (!g_cfg.companions) {
        U64 a = g_player_actor.load();
        if (!a || look_model(a) != model) return false;
    }
    return true;
}

U64 call_build(U64 model, U64 slot)
{
    __try {
        return g_build_orig(model, slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("cape stack: the game's stack builder faulted (model %llX)", model);
        return 0;
    }
}

bool call_rebuild(U64 model)
{
    __try {
        g_rebuild(model); // (the game's caller: builds through the hook, then applies)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

U64 __fastcall build_hook(U64 model, U64 slot)
{
    const U16 ch = vis_model_character(model); // (before our own lock: vis takes its own)
    const bool want = ch != kNoCharacter && wants_filter(ch, model);
    const U64 sbd = rp(slot);
    Filtered f;
    int removed = 0;
    const bool ok = want && sbd && g_cloak_id && make_filtered(sbd, f, removed);
    U64 fake = ok ? reinterpret_cast<U64>(f.obj.data()) : 0;
    U64 r = ok ? call_build(model, reinterpret_cast<U64>(&fake)) : g_build_orig(model, slot);
    if (!protagonist(ch)) { // (NPCs, and ours before the mod knows the model: stack_note takes those up)
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_models.empty()) g_models.erase(model); // (an address reused by another model)
    } else {
        std::lock_guard<std::mutex> lock(g_mu);
        ModelInfo& m = g_models[model];
        m.built = true, m.filtered = ok, m.pending = false, m.ch = ch;
        m.failed = want && !ok, m.stuck = 0, ++m.builds;
        m.keep = std::move(f); // (the previous copy goes only now: the stacks just built replaced what used it)
    }
    if (ok || g_cfg.verbose || g_logged.fetch_add(1) < 12) {
        const char* nm = character_name(ch);
        LOG("cape stack: built for model %llX (%s) on thread %lu: %s", model, nm ? nm : "not ours / not known yet",
            GetCurrentThreadId(),
            ok ? (removed ? "without the cape" : "without the cape (no Cloak in the rules?)")
               : want ? "with the cape (the copy failed)" : "as the game does");
    }
    return r;
}

U64 locate(U64 known, const char* pattern)
{
    if (game::matches(g_base + known, pattern)) return known;
    // (another plugin may have hooked it: its first 5 bytes are a jmp, the rest is still the prologue)
    if (*reinterpret_cast<const U8*>(g_base + known) == 0xE9) {
        std::string p = pattern;
        if (p.size() > 15 && game::matches(g_base + known + 5, p.substr(15).c_str())) return known;
    }
    if (std::uintptr_t a = game::find_unique(pattern)) return a - g_base;
    return 0;
}

} // namespace

bool install_stack()
{
    if (!g_cfg.cape_offset_fix) {
        LOG("cape stack: off in the ini (CapeOffsetFix = false)");
        return false;
    }
    if (!g_intern) {
        LOG("cape stack: the name interning function is missing; off");
        return false;
    }
    U64 build = locate(kRvaBuild, kBuildPattern);
    if (!build) {
        LOG("cape stack: the stack builder was not found (the game's code changed); hidden capes still push weapons out");
        return false;
    }
    U64 id = 0;
    __try {
        g_intern(&id, "Cloak", 1, 0x2ffff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id = 0;
    }
    g_cloak_id = static_cast<U32>(id);
    if (!g_cloak_id) {
        LOG("cape stack: could not name the Cloak part; off");
        return false;
    }
    void* t = reinterpret_cast<void*>(g_base + build);
    if (MH_CreateHook(t, reinterpret_cast<void*>(&build_hook), reinterpret_cast<void**>(&g_build_orig)) != MH_OK ||
        MH_EnableHook(t) != MH_OK) {
        LOG("cape stack: hooking the stack builder failed; off");
        return false;
    }
    if (g_cfg.cape_offset_refresh)
        if (U64 rb = locate(kRvaRebuild, kRebuildPattern)) g_rebuild = reinterpret_cast<RebuildFn>(g_base + rb);
    LOG("cape stack: builder +%llX hooked, Cloak part id %X; rebuild on toggle: %s", build, g_cloak_id,
        g_rebuild ? "on" : g_cfg.cape_offset_refresh ? "not found (applies on the next load / gear change)" : "off in the ini");
    return true;
}

void stack_note(U64 model, U16 ch, bool cape_hidden, bool layout_changed)
{
    if (!g_build_orig || !model || !protagonist(ch)) return;
    const bool want = cape_hidden && g_cfg.cape_offset_fix;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_models.find(model);
    if (it == g_models.end()) {
        // (built before the mod knew whose model it is: the game built it as it does - with the cape - and before the
        // mod's hides were on, so it is laid out again once)
        if (g_models.size() >= kMaxModels) // (models of earlier loads; a filtered one keeps its record copy - the
            for (auto e = g_models.begin(); e != g_models.end();) // game's stacks may point into it - unless far too many)
                e = !e->second.filtered || g_models.size() >= 4 * kMaxModels ? g_models.erase(e) : std::next(e);
        it = g_models.emplace(model, ModelInfo{}).first;
        it->second.built = true, it->second.ch = ch;
        layout_changed = true;
    }
    ModelInfo& m = it->second;
    if (m.pending || m.stuck >= 3) return;
    // (the stack is laid out from what is visible when it is built: a piece we hid or showed since - a bow, a shield,
    // a hidden weapon drawn or put away - leaves the outer layers where they were until it is built again)
    if (layout_changed || (m.filtered != want && !(want && m.failed))) m.pending = true, g_any_pending = true;
}

void stack_tick()
{
    if (!g_rebuild || !g_any_pending.exchange(false)) return;
    struct Todo {
        U64 model;
        bool was;
        U32 builds;
    };
    std::vector<Todo> todo;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& [model, m] : g_models)
            if (m.pending) todo.push_back({model, m.filtered, m.builds}), m.pending = false;
    }
    for (const auto& [model, was, before] : todo) {
        if (g_cfg.verbose || g_logged.fetch_add(1) < 40)
            LOG("cape stack: rebuilding model %llX on thread %lu (built %s the cape before)", model, GetCurrentThreadId(),
                was ? "without" : "with");
        if (!call_rebuild(model)) {
            LOG("cape stack: the rebuild faulted; rebuilds on toggle are off for this session");
            g_rebuild = nullptr;
            return;
        }
        std::lock_guard<std::mutex> lock(g_mu);
        if (auto it = g_models.find(model); it != g_models.end() && it->second.builds == before && ++it->second.stuck >= 3)
            LOG("cape stack: model %llX was not rebuilt (its data isn't ready?); waiting for the game's next build", model);
    }
}

} // namespace ghw
