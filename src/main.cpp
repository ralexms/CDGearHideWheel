// Gear Hide Wheel for Crimson Desert: hide / show each worn piece from the gear wheel, per character.
// DllMain and start-up.
#include "common.h"
#include "addrs.h"
#include "config.h"
#include "headgear.h"
#include "input.h"
#include "stack.h"
#include "state.h"
#include "ui.h"
#include "vis.h"
#include "wheel.h"

namespace ghw {

U64 g_base = 0;
HMODULE g_self = nullptr;
std::wstring g_dir;

constexpr const char* kVersion = "0.5.0";

namespace {

bool in_game_process()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe = buf;
    exe = exe.substr(exe.find_last_of(L"/\\") + 1);
    return _wcsicmp(exe.c_str(), L"CrimsonDesert.exe") == 0;
}

std::string read_text(const std::wstring& path)
{
    std::string text;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return text;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got);
    CloseHandle(h);
    return text;
}

// Outfit Manager checks the game's code before it hooks it, and hooks the same gear wheel. Our hooks go in after it
// is done, so it finds the game untouched and ours chain onto its own.
void wait_for_outfit_manager()
{
    HMODULE om = nullptr;
    for (int i = 0; i < 30 && !(om = GetModuleHandleW(L"OutfitManager.asi")); ++i) Sleep(100);
    if (!om) {
        LOG("Outfit Manager: not loaded");
        return;
    }
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(om, buf, MAX_PATH);
    std::wstring log = buf;
    log = log.substr(0, log.find_last_of(L"/\\") + 1) + L"OutfitManager.log";
    // (its log is rewritten on every launch: until it has been written since the game started, it is last session's)
    FILETIME created{}, x1{}, x2{}, x3{};
    GetProcessTimes(GetCurrentProcess(), &created, &x1, &x2, &x3);
    const U64 since = (U64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    auto written_since_start = [&] {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &fa)) return false;
        return ((U64(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime) >= since;
    };
    U64 start = GetTickCount64();
    for (;;) {
        std::string t = written_since_start() ? read_text(log) : std::string();
        bool done = t.find("stash ready") != std::string::npos || t.find("stash unavailable") != std::string::npos ||
                    t.find("setup failed") != std::string::npos;
        if (done) {
            LOG("Outfit Manager: loaded and set up (%llu ms); hooking after it", GetTickCount64() - start);
            Sleep(300);
            return;
        }
        if (GetTickCount64() - start > 60000) {
            LOG("Outfit Manager: loaded, but its log never said it was set up; going ahead");
            return;
        }
        Sleep(100);
    }
}

DWORD WINAPI init_thread(LPVOID)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    std::wstring path = buf;
    g_dir = path.substr(0, path.find_last_of(L"/\\") + 1);
    logx::open((g_dir + L"GearHideWheel.log").c_str());
    LOG("Gear Hide Wheel %s started", kVersion);
    load_config();
    load_state();
    if (GetModuleHandleW(L"CrimsonDesertEquipHide.asi"))
        LOG("note: Gear Hider (CrimsonDesertEquipHide.asi) is loaded too; pieces either hides stay hidden");
    wait_for_outfit_manager();
    g_base = game::main_module().base;
    g_cache_path = g_dir + L"GearHideWheel.cache"; // (game addresses for this exe; rebuilt when the exe changes)
    resolve_addresses();
    if (!have({kRvaPlayerHolder, kRvaActorCtx, kRvaCtxRelease, kRvaCtxVt})) {
        LOG("the player was not found (unsupported game version); mod inactive");
        return 0;
    }
    g_actor_ctx = reinterpret_cast<CtxFn>(g_base + kRvaActorCtx);
    g_ctx_release = reinterpret_cast<CtxReleaseFn>(g_base + kRvaCtxRelease);
    if (MH_STATUS s = MH_Initialize(); s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("MinHook failed to start (%d); mod inactive", static_cast<int>(s));
        return 0;
    }
    install_ui();
    bool vis = install_vis();
    bool head = install_headgear();
    bool stack = vis && install_stack();
    bool wheel = install_wheel();
    if (!wheel) {
        LOG("the gear wheel could not be hooked; choices already saved still apply, nothing can be toggled");
    }
    start_input();
    LOG("ready: hiding %s, per-character headgear %s, cape offset fix %s, gear wheel %s", vis ? "on" : "OFF",
        head ? "on" : "OFF", stack ? "on" : "OFF", wheel ? "on" : "OFF");
    return 0;
}

} // namespace
} // namespace ghw

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (!ghw::in_game_process()) return TRUE;
        ghw::g_self = inst;
        if (HANDLE t = CreateThread(nullptr, 0, ghw::init_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
