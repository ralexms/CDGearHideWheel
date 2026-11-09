// UI element helpers (from Outfit Manager, MIT licence): classes, text, the game's toast; item names.
// Element: +0x20 class holder {element*, vector<u32 classId>*}, children +0x40 (count +0x48). Script object: +8 element.
#include "common.h"
#include "ui.h"
#include "addrs.h"

namespace ghw {

namespace {
using ItemInfoFn = U64(__fastcall*)(U64, U32);
using LocTextFn = const char*(__fastcall*)(U64, U32);
ItemInfoFn g_item_info = nullptr;
VoidFn g_restyle = nullptr; // (class holder*): re-styles an element after its classes changed
bool g_ui_ok = false;
std::mutex g_names_mu;
std::map<U16, std::string> g_item_names;

std::string class_name(U32 id)
{
    U64 tbl = rp(g_base + kRvaClassNames);
    U64 arr = tbl ? rp(tbl + 0x58) : 0;
    return arr ? cstr(rp(arr + U64(id) * 0x10)) : std::string();
}

const char* item_name_raw(U16 key)
{
    __try {
        U64 info = g_item_info(0, key);
        if (!info || info == *reinterpret_cast<U64*>(g_base + kRvaDefaultItemInfo)) return nullptr;
        U64 text = *reinterpret_cast<U64*>(info + 0x90); // localized name, vfunc 1 (text, 0) -> UTF-8
        if (!text) return nullptr;
        return reinterpret_cast<LocTextFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(text) + 8))(text, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void toast(U64 script, const char* text)
{
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(script) + kVfUiMgr))(script);
        if (ui) g_toast(ui, text, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("notify: the toast faulted");
    }
}

bool set_text_raw(U64 elem, const char* utf8)
{
    __try {
        g_set_text(elem, utf8, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool restyle_raw(U64 holder, U64 vec, U32 cid, bool on)
{
    __try {
        if (on) {
            if (!g_vec_add(vec, &cid)) return true; // already set
            g_vec_sort(vec);
        } else if (g_vec_remove(vec, &cid) == 0xFFFFFFFF) {
            return true; // not set
        }
        g_restyle(holder);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool install_ui()
{
    if (!have({kRvaClassNames, kRvaIntern, kRvaClassVecAdd, kRvaClassVecRemove, kRvaClassVecSort, kRvaRestyle, kRvaSetText})) {
        LOG("ui: element functions not found; no prompt in the wheel");
        return false;
    }
    g_intern = reinterpret_cast<InternFn>(g_base + kRvaIntern);
    g_vec_add = reinterpret_cast<VecAddFn>(g_base + kRvaClassVecAdd);
    g_vec_remove = reinterpret_cast<VecRemoveFn>(g_base + kRvaClassVecRemove);
    g_vec_sort = reinterpret_cast<VoidFn>(g_base + kRvaClassVecSort);
    g_restyle = reinterpret_cast<VoidFn>(g_base + kRvaRestyle);
    g_set_text = reinterpret_cast<SetTextFn>(g_base + kRvaSetText);
    if (have({kRvaToast})) g_toast = reinterpret_cast<ToastFn>(g_base + kRvaToast);
    if (have({kRvaItemInfo, kRvaDefaultItemInfo})) g_item_info = reinterpret_cast<ItemInfoFn>(g_base + kRvaItemInfo);
    g_ui_ok = true;
    return true;
}

bool is_element(U64 e)
{
    U64 holder = rp(e + 0x20);
    return holder && rp(holder) == e;
}

U64 find_by_class(U64 elem, const char* cls, int max_depth, int depth)
{
    if (!g_ui_ok || !elem || depth > max_depth) return 0;
    U64 holder = rp(elem + 0x20);
    U64 vec = holder ? rp(holder + 8) : 0;
    U64 data = vec ? rp(vec) : 0;
    U32 n = vec ? r32(vec + 8) : 0;
    for (U32 i = 0; data && i < n && i < 64; ++i)
        if (class_name(r32(data + i * 4ull)) == cls) return elem;
    U64 kids = rp(elem + 0x40);
    U32 count = r32(elem + 0x48);
    for (U32 i = 0; kids && i < count && i < 64; ++i)
        if (U64 hit = find_by_class(rp(kids + i * 8ull), cls, max_depth, depth + 1)) return hit;
    return 0;
}

bool has_class(U64 elem, const char* cls) { return elem && find_by_class(elem, cls, 0) == elem; }

bool set_class(U64 elem, const char* cls, bool on)
{
    if (!g_ui_ok) return false;
    U64 holder = rp(elem + 0x20);
    U64 vec = holder ? rp(holder + 8) : 0;
    if (!vec) return false;
    U64 pending = rp(rp(holder) + 0xa0);
    if (pending && rp(pending + 8)) return false; // (mid restyle)
    U64 id = 0;
    __try {
        g_intern(&id, cls, 1, 0x2ffff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    U32 cid = static_cast<U32>(id);
    return cid && restyle_raw(holder, vec, cid, on);
}

bool set_text(U64 elem, const char* utf8) { return g_ui_ok && elem && set_text_raw(elem, utf8); }

void notify(U64 script, const std::string& text)
{
    LOG("notify: %s", text.c_str());
    if (g_toast && script) toast(script, text.c_str());
}

std::string item_name(U16 key)
{
    if (!g_item_info || key == 0xFFFF) return {};
    std::lock_guard<std::mutex> lock(g_names_mu);
    if (auto f = g_item_names.find(key); f != g_item_names.end()) return f->second;
    const char* s = item_name_raw(key);
    return g_item_names[key] = s ? std::string(s, strnlen(s, 256)) : std::string();
}

} // namespace ghw
