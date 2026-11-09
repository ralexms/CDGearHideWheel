// Keyboard, mouse buttons and controllers (XInput; PlayStation pads over HID), read by a thread of our own.
// The PlayStation reader follows Outfit Manager's (MIT licence): the game reads those pads natively, so XInput never
// sees them; their HID input reports are read on a shared handle.
#include "common.h"
#include "input.h"
#include "config.h"

#include <hidsdi.h>
#include <setupapi.h>

namespace ghw {

std::atomic<bool> g_listen{false};
std::atomic<bool> g_pad_in_use{false};

namespace {

std::atomic<U64> g_toggle_at{0}, g_under_at{0};
std::atomic<U32> g_hid_buttons{0}; // the PlayStation pad's buttons, as XInput bits
std::atomic<bool> g_hid_touched{false};
HANDLE g_key_wake = nullptr, g_hid_wake = nullptr; // set when the wheel opens: the reader threads sleep on them

bool game_focused()
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

// --- XInput -------------------------------------------------------------------------------------------
struct XPad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    DWORD dwPacketNumber;
    XPad Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);

U32 xinput_buttons(bool& touched)
{
    static XInputGetStateFn get = [] {
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryW(dll))
                if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) {
                    LOG("input: Xbox pads through %ls", dll);
                    return f;
                }
        LOG("input: no XInput found (Xbox pads won't work)");
        return static_cast<XInputGetStateFn>(nullptr);
    }();
    static U64 skip_until[4] = {};
    if (!get) return 0;
    U64 now = GetTickCount64();
    U32 b = 0;
    for (DWORD i = 0; i < 4; ++i) {
        if (now < skip_until[i]) continue;
        XState s{};
        if (get(i, &s) != ERROR_SUCCESS) {
            skip_until[i] = now + 2000; // (polling an empty port is slow)
            continue;
        }
        b |= s.Gamepad.wButtons;
        if (s.Gamepad.bLeftTrigger > 100) b |= kPadLT;
        if (s.Gamepad.bRightTrigger > 100) b |= kPadRT;
        if (s.Gamepad.wButtons || s.Gamepad.bLeftTrigger > 64 || s.Gamepad.bRightTrigger > 64 ||
            std::abs(s.Gamepad.sThumbLX) > 12000 || std::abs(s.Gamepad.sThumbLY) > 12000 ||
            std::abs(s.Gamepad.sThumbRX) > 12000 || std::abs(s.Gamepad.sThumbRY) > 12000)
            touched = true;
    }
    return b;
}

// --- PlayStation pads (HID) -----------------------------------------------------------------------------
// The face byte (d-pad low nibble, 8 = none; Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80) and the byte after
// it (L1 0x01, R1 0x02, L2 0x04, R2 0x08, Share/Create 0x10, Options 0x20, L3 0x40, R3 0x80), by report: DualSense USB
// 0x01 [8], Bluetooth 0x31 [9], Bluetooth simple 0x01 [5]; DualShock 4 USB 0x01 [5], Bluetooth 0x11 [7].
struct PadKind {
    USHORT pid;
    bool dualsense;
};
constexpr USHORT kSonyVid = 0x054C;
constexpr PadKind kPads[] = {{0x0CE6, true}, {0x0DF2, true}, {0x05C4, false}, {0x09CC, false}, {0x0BA0, false}};

U32 hid_parse(const unsigned char* r, DWORD n, bool dualsense, bool& touched)
{
    int face = -1, sticks = 1;
    if (dualsense) {
        if (r[0] == 0x01 && n >= 64) face = 8;
        else if (r[0] == 0x31 && n >= 11) face = 9, sticks = 2;
        else if (r[0] == 0x01 && n >= 7) face = 5;
    } else {
        if (r[0] == 0x01 && n >= 7) face = 5;
        else if (r[0] == 0x11 && n >= 9) face = 7, sticks = 3;
    }
    if (face < 0 || DWORD(face + 2) >= n) return 0;
    const U8 f = r[face], s = r[face + 1];
    U32 b = 0;
    if (f & 0x10) b |= kPadX;
    if (f & 0x20) b |= kPadA;
    if (f & 0x40) b |= kPadB;
    if (f & 0x80) b |= kPadY;
    const U8 d = f & 0x0F; // (0 up, clockwise to 7 up-left)
    if (d == 0 || d == 1 || d == 7) b |= kPadUp;
    if (d >= 1 && d <= 3) b |= kPadRight;
    if (d >= 3 && d <= 5) b |= kPadDown;
    if (d >= 5 && d <= 7) b |= kPadLeft;
    if (s & 0x01) b |= kPadLB;
    if (s & 0x02) b |= kPadRB;
    if (s & 0x04) b |= kPadLT;
    if (s & 0x08) b |= kPadRT;
    if (s & 0x10) b |= kPadBack;
    if (s & 0x20) b |= kPadStart;
    if (s & 0x40) b |= kPadLS;
    if (s & 0x80) b |= kPadRS;
    touched = b || (r[face + 2] & 0x03);
    for (int k = 0; k < 4 && !touched; ++k) touched = std::abs(int(r[sticks + k]) - 0x80) > 48;
    return b;
}

HANDLE hid_open(bool& dualsense, DWORD& report_len)
{
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA itf{sizeof itf};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &itf); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &itf, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<char> buf(need);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &itf, detail, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{sizeof at};
        bool ours = false;
        if (HidD_GetAttributes(h, &at) && at.VendorID == kSonyVid)
            for (const PadKind& k : kPads)
                if (at.ProductID == k.pid) ours = true, dualsense = k.dualsense;
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (ours && HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength) {
                report_len = caps.InputReportByteLength;
                found = h;
                LOG("input: PlayStation pad (%s, product %04X)", dualsense ? "DualSense" : "DualShock 4", at.ProductID);
            }
            HidD_FreePreparsedData(pp);
        }
        if (found != h) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

DWORD WINAPI hid_thread(LPVOID)
{
    for (;;) {
        bool dualsense = false;
        DWORD len = 0;
        HANDLE h = hid_open(dualsense, len);
        if (h == INVALID_HANDLE_VALUE) {
            g_hid_buttons = 0;
            WaitForSingleObject(g_hid_wake, INFINITE); // (none connected: look again when the wheel next opens)
            continue;
        }
        std::vector<unsigned char> r(len);
        DWORD got = 0;
        while (ReadFile(h, r.data(), len, &got, nullptr)) { // (blocks between reports: no polling)
            if (!g_listen) { // (the wheel is closed: nothing reads the buttons)
                g_hid_buttons = 0;
                continue;
            }
            bool touched = false;
            g_hid_buttons = hid_parse(r.data(), got, dualsense, touched);
            if (touched) g_hid_touched = true;
        }
        LOG("input: the PlayStation pad stopped answering (disconnected?)");
        CloseHandle(h);
        g_hid_buttons = 0;
    }
}

// --- Bindings -----------------------------------------------------------------------------------------
bool combo_held(const Combo& c, U32 pad)
{
    for (const Input& in : c) {
        bool down = in.pad ? (pad & in.code) == in.code : (GetAsyncKeyState(static_cast<int>(in.code)) & 0x8000) != 0;
        if (!down) return false;
    }
    return !c.empty();
}

bool binding_held(const Binding& b, U32 pad)
{
    for (const Combo& c : b.combos)
        if (combo_held(c, pad)) return true;
    return false;
}

DWORD WINAPI key_thread(LPVOID)
{
    bool was_toggle = true, was_under = true; // (held at start: not a press)
    POINT last{};
    for (;;) {
        if (!g_listen) { // (the wheel is closed: sleep until it opens - presses only count in it)
            was_toggle = was_under = true;
            WaitForSingleObject(g_key_wake, INFINITE);
            GetCursorPos(&last);
            continue;
        }
        Sleep(10);
        if (!game_focused()) {
            was_toggle = was_under = true;
            continue;
        }
        bool touched = false;
        U32 pad = xinput_buttons(touched) | g_hid_buttons.load();
        touched |= g_hid_touched.exchange(false);
        POINT p{};
        bool mouse = GetCursorPos(&p) && (std::abs(p.x - last.x) > 2 || std::abs(p.y - last.y) > 2);
        last = p;
        if (touched) g_pad_in_use = true;
        else if (mouse || (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) g_pad_in_use = false;
        bool t = binding_held(g_cfg.toggle, pad), u = binding_held(g_cfg.secondary, pad);
        if (t && !was_toggle) g_toggle_at = GetTickCount64();
        if (u && !was_under) g_under_at = GetTickCount64();
        was_toggle = t, was_under = u;
    }
}

bool take(std::atomic<U64>& at)
{
    U64 t = at.exchange(0);
    return t && GetTickCount64() - t < 500;
}

} // namespace

void set_listen(bool on)
{
    g_listen = on;
    if (!on) return;
    if (g_key_wake) SetEvent(g_key_wake);
    if (g_hid_wake) SetEvent(g_hid_wake);
}

void start_input()
{
    g_key_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); // (auto-reset: one wake per wheel open)
    g_hid_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_key_wake || !g_hid_wake) LOG("input: events could not be created");
    if (HANDLE t = CreateThread(nullptr, 0, key_thread, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, hid_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

bool take_toggle() { return take(g_toggle_at); }
bool take_secondary() { return take(g_under_at); }

} // namespace ghw
