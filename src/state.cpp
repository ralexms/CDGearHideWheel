// What each character hides: kept in GearHideWheel.state beside the .asi (per character, for every save).
#include "common.h"
#include "state.h"
#include "headgear.h"

namespace ghw {

namespace {

std::mutex g_mu;
std::map<U16, CharState> g_state;
// The headgear modes again, lock-free (the headgear hooks run on every headgear decision of every character).
std::atomic<int> g_head[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

std::wstring path() { return g_dir + L"GearHideWheel.state"; }

std::string section_name(U16 ch)
{
    if (const char* n = character_name(ch)) return n;
    return "Character " + std::to_string(ch);
}

U16 character_from(const std::string& s)
{
    if (s == "Kliff") return kKliff;
    if (s == "Damiane") return kDamiane;
    if (s == "Oongka") return kOongka;
    unsigned v = 0;
    if (std::sscanf(s.c_str(), "Character %u", &v) == 1 && v < 0xFFFF) return static_cast<U16>(v);
    return kNoCharacter;
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void publish_head(U16 ch, int mode)
{
    if (ch < std::size(g_head)) g_head[ch] = mode;
}

const char* mode_key(int m)
{
    switch (m) {
    case kHeadShown: return "Shown";
    case kHeadHidden: return "Hidden";
    case kHeadCombat: return "Combat";
    case kHeadCutscenes: return "Cutscenes";
    }
    return "Game";
}

const char* shield_key(int m) { return m == kShieldHidden ? "Hidden" : m == kShieldInHand ? "InHand" : "Shown"; }
const char* pack_key(int m) { return m == kPackHidden ? "Hidden" : m == kPackInUse ? "InUse" : "Shown"; }

// (g_mu held)
void save_locked()
{
    std::string out = "; Gear Hide Wheel: what each character hides (written by the mod; safe to edit with the game closed).\n"
                      "; Hidden = equipment slot numbers, Shields = Shown, Hidden or InHand, Underwear = hidden / shown,\n"
                      "; Headgear = Game, Shown, Hidden, Combat or Cutscenes, MobilityPack = Shown, Hidden or InUse\n";
    auto list = [](const std::set<U16>& v) {
        std::string s;
        for (U16 slot : v) s += (s.empty() ? " " : ", ") + std::to_string(slot);
        return s;
    };
    for (const auto& [ch, s] : g_state)
        out += "\n[" + section_name(ch) + "]\nHidden =" + list(s.hidden) + "\nShields = " + shield_key(s.shield) +
               "\nUnderwear = " + (s.underwear ? "hidden" : "shown") + "\nHeadgear = " + mode_key(s.headgear) +
               "\nMobilityPack = " + pack_key(s.pack) + "\n";
    std::wstring tmp = path() + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        LOG("state: cannot write %ls", tmp.c_str());
        return;
    }
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok &= std::fclose(f) == 0;
    if (!ok || !MoveFileExW(tmp.c_str(), path().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        LOG("state: saving failed (error %lu)", GetLastError());
}

} // namespace

void load_state()
{
    std::string text;
    if (FILE* f = _wfopen(path().c_str(), L"rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_state.clear();
    U16 ch = kNoCharacter;
    std::set<U16> pack_set; // (characters whose file says MobilityPack: before 0.6 the backpack slot covered every pack)
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find('\n', a);
        std::string line = trim(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
        a = b == std::string::npos ? text.size() : b + 1;
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            ch = character_from(trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (ch == kNoCharacter || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        CharState& s = g_state[ch];
        if (k == "Hidden") {
            for (size_t p = 0; p < v.size();) {
                size_t q = v.find(',', p);
                unsigned slot = 0;
                if (std::sscanf(v.substr(p, q == std::string::npos ? std::string::npos : q - p).c_str(), "%u", &slot) == 1 && slot < 64)
                    s.hidden.insert(static_cast<U16>(slot));
                p = q == std::string::npos ? v.size() : q + 1;
            }
        } else if (k == "Shields") {
            s.shield = v == "Hidden" ? kShieldHidden : v == "InHand" ? kShieldInHand : kShieldShown;
        } else if (k == "HiddenShields") { // (0.2 / 0.3: the slots whose shield was hidden)
            if (!v.empty()) s.shield = kShieldHidden;
        } else if (k == "Underwear") {
            s.underwear = v == "hidden";
        } else if (k == "Headgear") {
            s.headgear = headgear_mode_from_name(v); // ("Game": -1)
        } else if (k == "MobilityPack") {
            s.pack = v == "Hidden" ? kPackHidden : v == "InUse" ? kPackInUse : kPackShown;
            pack_set.insert(ch);
        }
    }
    for (auto& [c, s] : g_state) // (an older file: a hidden backpack slot went for the rocket pack too)
        if (!pack_set.count(c) && s.hidden.count(kSlotBag)) s.pack = kPackHidden;
    std::string log;
    for (const auto& [c, s] : g_state) {
        publish_head(c, s.headgear);
        log += " " + section_name(c) + ": " + std::to_string(s.hidden.size()) + " hidden" + (s.underwear ? ", underwear" : "") +
               ", headgear " + mode_key(s.headgear) + ";";
    }
    LOG("state: %s", log.empty() ? "nothing hidden yet" : log.c_str());
}

CharState state_of(U16 ch)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_state.find(ch);
    return it == g_state.end() ? CharState{} : it->second;
}

bool state_hidden(U16 ch, U16 slot)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_state.find(ch);
    return it != g_state.end() && it->second.hidden.count(slot);
}

bool state_underwear(U16 ch)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_state.find(ch);
    return it != g_state.end() && it->second.underwear;
}

int state_headgear(U16 ch) { return ch < std::size(g_head) ? g_head[ch].load() : -1; }

bool state_any(U16 ch)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_state.find(ch);
    return it != g_state.end() && (!it->second.hidden.empty() || it->second.shield != kShieldShown || it->second.underwear ||
                                   it->second.headgear >= 0 || it->second.pack != kPackShown);
}

const char* pack_mode_text(int mode)
{
    return mode == kPackHidden ? "Hidden" : mode == kPackInUse ? "Shown when in use" : "Shown";
}

int state_pack(U16 ch)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_state.find(ch);
    return it == g_state.end() ? kPackShown : it->second.pack;
}

void state_set_pack(U16 ch, int mode)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_state[ch].pack = mode;
    save_locked();
}

const char* shield_mode_text(int mode)
{
    return mode == kShieldHidden ? "Hidden" : mode == kShieldInHand ? "Shown when equipped" : "Shown";
}

int state_shield(U16 ch)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_state.find(ch);
    return it == g_state.end() ? kShieldShown : it->second.shield;
}

void state_set_shield(U16 ch, int mode)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_state[ch].shield = mode;
    save_locked();
}

void state_set_hidden(U16 ch, U16 slot, bool hidden)
{
    std::lock_guard<std::mutex> lock(g_mu);
    CharState& s = g_state[ch];
    if (hidden) s.hidden.insert(slot);
    else s.hidden.erase(slot);
    save_locked();
}

void state_set_underwear(U16 ch, bool hidden)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_state[ch].underwear = hidden;
    save_locked();
}

void state_set_headgear(U16 ch, int mode)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_state[ch].headgear = mode;
    publish_head(ch, mode);
    save_locked();
}

} // namespace ghw
