// Per-character headgear mode, through the game's own headgear option (see headgear.h).
//
// Found in CrimsonDesert.exe 1.0.0.3576 (Steam build 26480954): the option byte is read by
//   +A856B0 (inv, ?, u8, u8 event)  the headgear decision; ends in the headgear switch (+A87F90, Outfit Manager's BattleVis)
//   +A85A20 (inv)                   re-runs the decision (the game calls it when the option changes)
//   +A85AF0 (inv, hat item)         a helper both of the above call
//   +A88980 (inv, ...)              the battle-state loop: re-decides when the option is "shown in combat"
//   +A9E3E0 (inv, ...)              another consumer of the option
// Each reads the byte with a rip-relative load; all five point at the same global.
#include "common.h"
#include "headgear.h"
#include "gear.h"
#include "state.h"
#include "config.h"

namespace ghw {

namespace {

// Up to 12 arguments are passed through unchanged (the battle-state loop takes 11; the rest read fewer). Arguments past
// the fourth live in the caller's stack slots and are copied as they are, floats included.
using PassFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);

struct Reader {
    const char* name;
    U64 known;
    const char* pattern;
    U32 read_at;         // offset of its option read in the known build (0: not used to locate the byte)
    const char* read_op; // that read's opcode bytes (the rel32 follows)
    PassFn orig = nullptr;
    U64 rva = 0;
};

Reader g_readers[] = {
    {"decide", 0xa856b0, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 30 41 0F B6 F1 45 0F B6 F0",
     0xac, "0F B6 2D"},
    {"refresh", 0xa85a20, "48 89 5C 24 18 57 48 83 EC 20 48 8B F9 48 8B 41 08 48 8B 50 68 48 8B 42", 0, nullptr},
    {"helper", 0xa85af0, "48 89 6C 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 41 08 48 8B EA 48", 0x85, "0F B6 15"},
    {"battle loop", 0xa88980,
     "48 89 5C 24 08 44 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 60 E8 FF FF B8 A0 18 00 00",
     0xe18, "80 3D"},
    {"consumer", 0xa9e3e0,
     "48 8B C4 4C 89 48 20 66 44 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 D8 FD FF FF 48 81 EC E8 02 00 00",
     0xee7, "0F B6 3D"},
};

U64 g_option = 0;  // address of the game's option byte (0: not found)
bool g_ready = false;
std::recursive_mutex g_swap_mu;
thread_local int t_depth = 0;
U8 g_real = 0; // the real option while a swap is on (read when the first swap of a thread starts)

// A hooked function starts with a jmp out of the image: compare the rest of its prologue.
bool prologue_at(U64 rva, const char* pattern)
{
    U64 a = g_base + rva;
    auto mod = game::main_module();
    if (rva + 64 > mod.size) return false;
    if (*reinterpret_cast<const U8*>(a) == 0xE9) {
        std::int32_t rel;
        std::memcpy(&rel, reinterpret_cast<const void*>(a + 1), 4);
        U64 t = a + 5 + static_cast<std::int64_t>(rel);
        if (t < mod.base || t >= mod.base + mod.size) { // (another plugin's hook) the bytes after its jmp
            std::string p = pattern;
            for (int i = 0; i < 5 && p.size() >= 3; ++i) p.erase(0, 3);
            return game::matches(a + 5, p.c_str());
        }
    }
    return game::matches(a, pattern);
}

U64 locate(const Reader& r)
{
    if (prologue_at(r.known, r.pattern)) return r.known;
    if (std::uintptr_t a = game::find_unique(r.pattern)) return a - g_base;
    return 0;
}

// The option byte a reader loads: its read instruction (opcode, then rel32; "80 3D" compares with an imm8 after).
U64 option_of(const Reader& r)
{
    if (!r.read_op || !r.rva) return 0;
    const std::string op = r.read_op;
    const size_t oplen = (op.size() + 1) / 3;
    const U64 imm = op == "80 3D" ? 1 : 0; // (cmp byte [rip+disp32], imm8: one byte after the displacement)
    const auto mod = game::main_module();
    auto target_at = [&](U64 at) -> U64 {
        std::int32_t rel;
        if (!game::matches(at, r.read_op) || !rd(at + oplen, rel)) return 0;
        U64 t = at + oplen + 4 + imm + static_cast<std::int64_t>(rel);
        return t >= mod.base && t < mod.base + mod.size ? t : 0;
    };
    const U64 start = g_base + r.rva;
    if (U64 t = target_at(start + r.read_at)) return t; // (where it is in the known build)
    for (U32 d = 0; d < r.read_at + 0x400; ++d)       // (a patch may have moved it a little)
        if (U64 t = target_at(start + d)) return t;
    return 0;
}

int mode_for(U64 inv)
{
    U64 actor = rp(inv + 8);
    if (!actor) return -1;
    U16 ch = character_of(actor);
    if (ch == kNoCharacter) return -1;
    if (!g_cfg.companions && actor != g_player_actor.load()) return -1;
    return state_headgear(ch);
}

struct Swap {
    bool on = false;
    U8 prev = 0;
    explicit Swap(U64 inv)
    {
        if (!g_ready || !inv) return;
        const int mode = mode_for(inv);
        if (mode < 0 && !t_depth) return; // (no mode of its own and nothing swapped: the game's value as it is)
        // (another thread swapping: wait a little, never block for good)
        bool locked = false;
        for (int i = 0; i < 2000 && !(locked = g_swap_mu.try_lock()); ++i) YieldProcessor();
        if (!locked) {
            static std::atomic<int> told{0};
            if (told++ < 3) LOG("headgear: the option was busy on another thread; the game's value used once");
            return;
        }
        if (!t_depth) rd(g_option, g_real);
        rd(g_option, prev);
        U8 want = mode >= 0 ? static_cast<U8>(mode) : g_real; // (nested, for a character without a mode: the real one)
        if (prev != want) wr<U8>(g_option, want);
        ++t_depth;
        on = true;
    }
    ~Swap()
    {
        if (!on) return;
        --t_depth;
        U8 now = 0;
        rd(g_option, now);
        if (now != prev) wr<U8>(g_option, prev);
        g_swap_mu.unlock();
    }
};

template <int I>
U64 __fastcall reader_hook(U64 a1, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7, U64 a8, U64 a9, U64 a10, U64 a11,
                           U64 a12)
{
    Swap s(a1);
    return g_readers[I].orig(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
}

void* const kHooks[] = {reinterpret_cast<void*>(&reader_hook<0>), reinterpret_cast<void*>(&reader_hook<1>),
                        reinterpret_cast<void*>(&reader_hook<2>), reinterpret_cast<void*>(&reader_hook<3>),
                        reinterpret_cast<void*>(&reader_hook<4>)};
static_assert(std::size(kHooks) == std::size(g_readers));

} // namespace

int headgear_mode_from_name(const std::string& name)
{
    std::string l;
    for (char c : name) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "shown" || l == "show" || l == "always shown" || l == "visible") return kHeadShown;
    if (l == "hidden" || l == "hide" || l == "always hidden") return kHeadHidden;
    if (l == "combat" || l == "shown in combat" || l == "show in combat") return kHeadCombat;
    if (l == "cutscenes" || l == "cutscene" || l == "hide in cutscenes") return kHeadCutscenes;
    return -1;
}

const char* headgear_mode_text(int mode)
{
    switch (mode) {
    case kHeadShown: return "Always shown";
    case kHeadHidden: return "Always hidden";
    case kHeadCombat: return "Shown in combat";
    case kHeadCutscenes: return "Hidden in cutscenes";
    }
    return "Game setting";
}

bool install_headgear()
{
    int found = 0;
    for (Reader& r : g_readers) {
        r.rva = locate(r);
        found += r.rva != 0;
        if (!r.rva) LOG("headgear: %s (+%llX) not found", r.name, r.known);
    }
    if (found != static_cast<int>(std::size(g_readers))) {
        LOG("headgear: per-character headgear off (the game's code changed)");
        return false;
    }
    // every reader that names the byte must name the same one
    U64 opt = 0;
    int agree = 0;
    for (const Reader& r : g_readers) {
        U64 t = option_of(r);
        if (!t) continue;
        if (opt && t != opt) {
            LOG("headgear: the readers disagree on the option (+%llX / +%llX); feature off", opt - g_base, t - g_base);
            return false;
        }
        opt = t, ++agree;
    }
    if (!opt || agree < 2) {
        LOG("headgear: the option byte was not found (%d reader(s) named it); feature off", agree);
        return false;
    }
    g_option = opt;
    U8 v = 0xFF;
    rd(g_option, v);
    LOG("headgear: option at +%llX (named by %d readers), the game's setting is %u (%s)", g_option - g_base, agree, v,
        headgear_mode_text(v));
    for (size_t i = 0; i < std::size(g_readers); ++i) {
        void* t = reinterpret_cast<void*>(g_base + g_readers[i].rva);
        if (MH_CreateHook(t, kHooks[i], reinterpret_cast<void**>(&g_readers[i].orig)) != MH_OK || MH_EnableHook(t) != MH_OK) {
            LOG("headgear: hooking %s failed; feature off", g_readers[i].name);
            for (size_t k = 0; k <= i; ++k) MH_DisableHook(reinterpret_cast<void*>(g_base + g_readers[k].rva));
            return false;
        }
    }
    g_ready = true;
    return true;
}

bool headgear_ready() { return g_ready; }

int game_headgear_option()
{
    if (!g_option) return -1;
    std::unique_lock<std::recursive_mutex> lock(g_swap_mu, std::try_to_lock);
    if (!lock.owns_lock()) return g_real; // (mid swap: the real one was saved)
    U8 v = 0;
    return rd(g_option, v) && v < kHeadModeCount ? v : -1;
}

void refresh_headgear(U64 inv)
{
    if (!g_ready || !inv) return;
    using RefreshFn = void(__fastcall*)(U64);
    __try {
        reinterpret_cast<RefreshFn>(g_base + g_readers[1].rva)(inv); // (through the hook: the swap applies)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("headgear: the refresh faulted");
    }
}

} // namespace ghw
