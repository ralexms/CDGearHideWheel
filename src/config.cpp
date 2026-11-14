// The ini file (GearHideWheel.ini): key bindings and options.
#include "common.h"
#include "config.h"
#include "headgear.h"

namespace ghw {

Config g_cfg;

namespace {

const char kDefaultIni[] =
    "; Gear Hide Wheel - settings. Changes apply on the next game start.\n"
    ";\n"
    "; Open the gear wheel, highlight a piece (Weapons or Armour tab) and press the toggle key to hide / show it.\n"
    "; The highlighted Headgear slot cycles the headgear modes instead. The secondary key hides / shows the\n"
    "; underwear (Chest slot), the shield (weapon slots: the toggle key then covers the other weapons there) or the\n"
    "; mobility packs (Backpack slot: the toggle key then covers the other packs).\n"
    "; Every choice is kept per character (Kliff, Damiane, Oongka).\n"
    ";\n"
    "; Key names: A-Z, 0-9, F1-F24, Numpad0-Numpad9, Insert, Delete, Home, End, PageUp, PageDown, Tab, Backspace,\n"
    ";   Enter, Space, Left, Right, Up, Down, Mouse3 (middle), Mouse4, Mouse5, and the modifiers Ctrl, Shift, Alt.\n"
    "; Controller names (Xbox layout; PlayStation pads map Cross=A, Circle=B, Square=X, Triangle=Y, L1=LB, ...):\n"
    ";   Gamepad_A, Gamepad_B, Gamepad_X, Gamepad_Y, Gamepad_LB, Gamepad_RB, Gamepad_LT, Gamepad_RT,\n"
    ";   Gamepad_LS (left stick click), Gamepad_RS (right stick click), Gamepad_Back, Gamepad_Start,\n"
    ";   Gamepad_DpadUp, Gamepad_DpadDown, Gamepad_DpadLeft, Gamepad_DpadRight.\n"
    "; Join inputs held together with '+' (Ctrl+H, Gamepad_LB+Gamepad_Y); list alternatives with ',' (H, Gamepad_Y).\n"
    "; An empty value turns a binding off.\n"
    "\n"
    "[Keys]\n"
    "; Hides / shows the highlighted piece (cycles the headgear mode on the Headgear slot).\n"
    "Toggle = H, Gamepad_Y\n"
    "; Chest slot: hides / shows the underwear. Weapon slot: cycles the shield mode (shown, hidden, shown when\n"
    "; equipped); the toggle key then hides / shows only the other weapons that slot takes, e.g. a sword in the off hand.\n"
    "; Backpack slot: cycles the mobility pack mode (the rocket pack: shown, hidden, shown when in use); the toggle key\n"
    "; then hides / shows the other (utility) packs.\n"
    "Secondary = U, Gamepad_X\n"
    "\n"
    "[Options]\n"
    "; Show the current state and the keys to press on the item's name in the gear wheel's centre panel (true / false).\n"
    "ShowPrompt = true\n"
    "; A notification naming the new state on every press (true / false).\n"
    "Notifications = true\n"
    "; Tools (broom, pickaxe, shovel...) show on the character while put away, not only in the hand, as Gear Hider\n"
    "; showed them. Hidden tools stay hidden (true / false).\n"
    "ShowStowedTools = true\n"
    "; Some capes make the game hang the weapons on the back further out, so they don't clip through the cape. With\n"
    "; the cape hidden, the weapons hang as if no cape were worn (true / false).\n"
    "CapeOffsetFix = true\n"
    "; Apply that at once when you hide / show a cape in the wheel. If toggling a cape ever crashes the game, set this\n"
    "; to false: the weapons then move on the next load or gear change instead (true / false).\n"
    "CapeOffsetRefresh = true\n"
    "; Apply each character's choices while they follow you as a companion, too (true / false).\n"
    "Companions = true\n"
    "; The headgear modes the toggle cycles through, in order. Any of: Shown, Hidden, Combat, Cutscenes\n"
    ";   Shown = always shown, Hidden = always hidden, Combat = shown only in combat, Cutscenes = hidden in cutscenes\n"
    "HeadgearCycle = Shown, Hidden, Combat, Cutscenes\n"
    "; Extra detail in GearHideWheel.log (true / false).\n"
    "Verbose = false\n";

std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        v.push_back(trim(s.substr(a, b == std::string::npos ? std::string::npos : b - a)));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return v;
}

struct Name {
    const char* name;
    bool pad;
    U32 code;
    const char* shown; // how it is shown in game
};
const Name kNames[] = {
    {"ctrl", false, VK_CONTROL, "Ctrl"}, {"shift", false, VK_SHIFT, "Shift"}, {"alt", false, VK_MENU, "Alt"},
    {"lctrl", false, VK_LCONTROL, "LCtrl"}, {"rctrl", false, VK_RCONTROL, "RCtrl"},
    {"lshift", false, VK_LSHIFT, "LShift"}, {"rshift", false, VK_RSHIFT, "RShift"},
    {"lalt", false, VK_LMENU, "LAlt"}, {"ralt", false, VK_RMENU, "RAlt"},
    {"insert", false, VK_INSERT, "Insert"}, {"delete", false, VK_DELETE, "Delete"}, {"home", false, VK_HOME, "Home"},
    {"end", false, VK_END, "End"}, {"pageup", false, VK_PRIOR, "PgUp"}, {"pagedown", false, VK_NEXT, "PgDn"},
    {"tab", false, VK_TAB, "Tab"}, {"backspace", false, VK_BACK, "Backspace"}, {"enter", false, VK_RETURN, "Enter"},
    {"space", false, VK_SPACE, "Space"}, {"left", false, VK_LEFT, "Left"}, {"right", false, VK_RIGHT, "Right"},
    {"up", false, VK_UP, "Up"}, {"down", false, VK_DOWN, "Down"}, {"mouse3", false, VK_MBUTTON, "Mouse3"},
    {"middlemouse", false, VK_MBUTTON, "Mouse3"}, {"mouse4", false, VK_XBUTTON1, "Mouse4"},
    {"mouse5", false, VK_XBUTTON2, "Mouse5"}, {"capslock", false, VK_CAPITAL, "CapsLock"},
    {"`", false, VK_OEM_3, "`"}, {"-", false, VK_OEM_MINUS, "-"}, {"=", false, VK_OEM_PLUS, "="},
    {"[", false, VK_OEM_4, "["}, {"]", false, VK_OEM_6, "]"}, {";", false, VK_OEM_1, ";"},
    {"'", false, VK_OEM_7, "'"}, {"\\", false, VK_OEM_5, "\\"}, {"/", false, VK_OEM_2, "/"},
    {"gamepad_a", true, kPadA, "A"}, {"gamepad_b", true, kPadB, "B"}, {"gamepad_x", true, kPadX, "X"},
    {"gamepad_y", true, kPadY, "Y"}, {"gamepad_lb", true, kPadLB, "LB"}, {"gamepad_rb", true, kPadRB, "RB"},
    {"gamepad_lt", true, kPadLT, "LT"}, {"gamepad_rt", true, kPadRT, "RT"}, {"gamepad_ls", true, kPadLS, "LS"},
    {"gamepad_rs", true, kPadRS, "RS"}, {"gamepad_l3", true, kPadLS, "LS"}, {"gamepad_r3", true, kPadRS, "RS"},
    {"gamepad_back", true, kPadBack, "Back"}, {"gamepad_view", true, kPadBack, "View"},
    {"gamepad_start", true, kPadStart, "Start"}, {"gamepad_menu", true, kPadStart, "Menu"},
    {"gamepad_dpadup", true, kPadUp, "D-pad Up"}, {"gamepad_dpaddown", true, kPadDown, "D-pad Down"},
    {"gamepad_dpadleft", true, kPadLeft, "D-pad Left"}, {"gamepad_dpadright", true, kPadRight, "D-pad Right"},
};

bool parse_input(const std::string& token, Input& in, std::string& shown)
{
    std::string t = lower(token);
    for (const Name& n : kNames)
        if (t == n.name) {
            in = {n.pad, n.code};
            shown = n.shown;
            return true;
        }
    if (t.size() == 1 && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= '0' && t[0] <= '9'))) {
        in = {false, static_cast<U32>(std::toupper(static_cast<unsigned char>(t[0])))};
        shown = std::string(1, static_cast<char>(in.code));
        return true;
    }
    unsigned n = 0;
    if (t.size() >= 2 && t[0] == 'f' && std::sscanf(t.c_str() + 1, "%u", &n) == 1 && n >= 1 && n <= 24) {
        in = {false, static_cast<U32>(VK_F1 + n - 1)};
        shown = "F" + std::to_string(n);
        return true;
    }
    if (t.rfind("numpad", 0) == 0 && t.size() == 7 && t[6] >= '0' && t[6] <= '9') {
        in = {false, static_cast<U32>(VK_NUMPAD0 + (t[6] - '0'))};
        shown = "Num" + std::string(1, t[6]);
        return true;
    }
    return false;
}

Binding parse_binding(const char* what, const std::string& value)
{
    Binding b;
    if (trim(value).empty()) return b;
    for (const std::string& alt : split(value, ',')) {
        if (alt.empty()) continue;
        Combo c;
        std::string shown;
        bool ok = true, pad = false, key = false;
        for (const std::string& part : split(alt, '+')) {
            Input in;
            std::string s;
            if (!parse_input(part, in, s)) {
                LOG("config: %s: unknown input \"%s\" in \"%s\" (ignored)", what, part.c_str(), alt.c_str());
                ok = false;
                break;
            }
            c.push_back(in);
            (in.pad ? pad : key) = true;
            shown += (shown.empty() ? "" : "+") + s;
        }
        if (!ok || c.empty()) continue;
        if (pad && key) LOG("config: %s: \"%s\" mixes keyboard and controller inputs", what, alt.c_str());
        b.combos.push_back(c);
        std::string& text = pad ? b.pad_text : b.key_text;
        if (text.empty()) text = shown; // (the first combo of each kind is the one shown)
    }
    return b;
}

bool parse_bool(const std::string& v, bool def)
{
    std::string l = lower(trim(v));
    if (l == "true" || l == "1" || l == "yes" || l == "on") return true;
    if (l == "false" || l == "0" || l == "no" || l == "off") return false;
    return def;
}

} // namespace

std::string binding_text(const Binding& b, bool pad)
{
    if (pad && !b.pad_text.empty()) return b.pad_text;
    if (!pad && !b.key_text.empty()) return b.key_text;
    return pad ? b.key_text : b.pad_text;
}

void load_config()
{
    std::wstring path = g_dir + L"GearHideWheel.ini";
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
    } else {
        text = kDefaultIni;
        if (FILE* w = _wfopen(path.c_str(), L"wb")) {
            std::fwrite(kDefaultIni, 1, sizeof kDefaultIni - 1, w);
            std::fclose(w);
            LOG("config: GearHideWheel.ini not found, wrote the default one");
        }
    }
    std::map<std::string, std::string> kv; // "section.key" (lower case) -> value
    std::string section;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        std::string line = trim(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
        a = b == std::string::npos ? text.size() : b + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = lower(trim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string v = trim(line.substr(eq + 1));
        if (size_t c = v.find(" ;"); c != std::string::npos) v = trim(v.substr(0, c)); // (trailing comment)
        kv[section + "." + lower(trim(line.substr(0, eq)))] = v;
    }
    auto get = [&](const char* k, const char* def) {
        auto it = kv.find(k);
        return it == kv.end() ? std::string(def) : it->second;
    };
    g_cfg.toggle = parse_binding("Toggle", get("keys.toggle", "H, Gamepad_Y"));
    // (Secondary; "Underwear" is its name in 0.1.0 inis)
    g_cfg.secondary = parse_binding("Secondary", kv.count("keys.secondary") ? kv["keys.secondary"] : get("keys.underwear", "U, Gamepad_X"));
    g_cfg.show_prompt = parse_bool(get("options.showprompt", "true"), true);
    g_cfg.notifications = parse_bool(get("options.notifications", "true"), true);
    g_cfg.show_stowed_tools = parse_bool(get("options.showstowedtools", "true"), true);
    g_cfg.cape_offset_fix = parse_bool(get("options.capeoffsetfix", "true"), true);
    g_cfg.cape_offset_refresh = parse_bool(get("options.capeoffsetrefresh", "true"), true);
    LOG("config: stowed tools %s", g_cfg.show_stowed_tools ? "shown" : "game default");
    g_cfg.companions = parse_bool(get("options.companions", "true"), true);
    g_cfg.verbose = parse_bool(get("options.verbose", "false"), false);
    g_cfg.headgear_cycle.clear();
    for (const std::string& m : split(get("options.headgearcycle", "Shown, Hidden, Combat, Cutscenes"), ',')) {
        int v = headgear_mode_from_name(m);
        if (v < 0) {
            if (!m.empty()) LOG("config: HeadgearCycle: unknown mode \"%s\" (ignored)", m.c_str());
            continue;
        }
        if (std::find(g_cfg.headgear_cycle.begin(), g_cfg.headgear_cycle.end(), v) == g_cfg.headgear_cycle.end())
            g_cfg.headgear_cycle.push_back(v);
    }
    if (g_cfg.headgear_cycle.size() < 2) g_cfg.headgear_cycle = {kHeadShown, kHeadHidden, kHeadCombat, kHeadCutscenes};
    LOG("config: toggle [%s | %s], secondary [%s | %s], prompt %s, notifications %s, companions %s, headgear cycle of %zu",
        g_cfg.toggle.key_text.c_str(), g_cfg.toggle.pad_text.c_str(), g_cfg.secondary.key_text.c_str(),
        g_cfg.secondary.pad_text.c_str(), g_cfg.show_prompt ? "on" : "off", g_cfg.notifications ? "on" : "off",
        g_cfg.companions ? "on" : "off", g_cfg.headgear_cycle.size());
}

} // namespace ghw
