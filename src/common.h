// Gear Hide Wheel: shared types, game function signatures and constants. Included by every module.
//
// The game-facing knowledge here (offsets, function signatures, the visibility-layer model) comes from Outfit Manager
// by Strahlimeier (MIT licence, see third_party/OutfitManager-LICENSE.txt), which reverse-engineered it.
#pragma once

#include "game.h"
#include "log.h"
#include "resolver.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <intrin.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ghw {

using U64 = std::uint64_t;
using U32 = std::uint32_t;
using U16 = std::uint16_t;
using U8 = std::uint8_t;

constexpr U64 kUserMax = 0x00007FFFFFFEFFFFull;
constexpr U64 kVfUiMgr = 0x118; // any UI script's vfunc returning the UI manager (toasts)

// --- Game function signatures (as Outfit Manager uses them) ----------------------------------------
using RecordGetFn = U64(__fastcall*)(const U16* key); // the game's table getters by key (u32 by value before 1.0.0.3663)
using VoidFn = void(__fastcall*)(U64);
using SetTextFn = void(__fastcall*)(U64, const char*, U64);
using FloatFn = float(__fastcall*)(U64);
using CtxFn = U64(__fastcall*)(U64, U64*);
using CtxReleaseFn = void(__fastcall*)(U64*);
using IconNameFn = const char*(__fastcall*)(U64);
using StrAssignFn = void(__fastcall*)(U64*, const char*);
using StrFn = void(__fastcall*)(U64*);
using SetStylePropFn = void(__fastcall*)(U64, const U64*, U64*, const U64*);
using TextureExistsFn = bool(__fastcall*)(U64, U64*);
using UiMgrFn = U64(__fastcall*)(U64);
using SetStateFn = void(__fastcall*)(U64, const U32*, U64);
using FocusedSlotFn = U64(__fastcall*)(U64, U64);
using EquipFn = int*(__fastcall*)(U64, int*, U16, short, short, U64);
using UnequipFn = int*(__fastcall*)(U64, int*, short);
using InventoryFn = U64(__fastcall*)(U64);
using ErrorMgrFn = U64(__fastcall*)();
using ErrorTextFn = U64(__fastcall*)(U64, const int*);
using ItemMoveFn = int*(__fastcall*)(U64, int*, U32, U32, short, U16, U64, U16, U16, U32);
using BagInfoFn = U64(__fastcall*)(U64, U32);
using EquipRequestFn = int*(__fastcall*)(U64, int*, const void*, short);
using UnequipRequestFn = int*(__fastcall*)(U64, int*, short);
using ToastFn = void(__fastcall*)(U64, const char*, U32);
using FindViewFn = U64(__fastcall*)(U64, const char*);
using OpenTextModalFn = U64(__fastcall*)(U64, U64, unsigned char*, int);
using GetTextFn = void(__fastcall*)(U64, char*, int);
using NameInvalidFn = bool(__fastcall*)(const char*);
using OpenCommonModalFn = U64(__fastcall*)(U64, U64, unsigned char*, const char*);
using InternFn = void(__fastcall*)(U64*, const char*, U32, U32);
using VecAddFn = bool(__fastcall*)(U64, const U32*);
using VecRemoveFn = U32(__fastcall*)(U64, const U32*);

// The root gear wheel (RootQuickSlot) vtable callbacks.
using OpenFn = U64(__fastcall*)(U64, U64, U64, U64);
using TabFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64);

// Visibility: the model's per-part layers (part+0x70: bit 16+layer = the layer decides, bit layer = shown; the
// highest deciding layer wins - confirmed in the game's own setter). The game uses layers 0-5 and 7 (4 = the headgear
// option), Outfit Manager uses 6. This mod uses 8: above all of them, and it only ever HIDES or lets go.
using VisSetFn = U8(__fastcall*)(U64 model, U64 list, U8 shown, U8 layer, U64, U64, U8);
using VisReleaseFn = U8(__fastcall*)(U64 model, U64 list, U8 layer, U8, U8);
using BattleVisFn = void(__fastcall*)(U64 inv, U8 in_battle, U8 show_headgear);
using SlotHandleFn = int*(__fastcall*)(U64 inv, int* out, short slot);
using EquipVisCheckFn = float(__fastcall*)(U64 vc, const U32* part, U8* rec, U8 drawn);
using ModelSwapFn = void(__fastcall*)(U64 job, U64, U64, char, U8*);
using FrameFn = float(__fastcall*)(U64);

constexpr U8 kLayer = 8;

// --- Characters (the key Outfit Manager reads at actor component +0x20 -> +0x30) -------------------
constexpr U16 kNoCharacter = 0xFFFF;
constexpr U16 kKliff = 0, kDamiane = 3, kOongka = 5;
const char* character_name(U16 key);

// --- Equipment slots ("equipment-N") ----------------------------------------------------------------
constexpr U16 kSlotHat = 3, kSlotChest = 4, kSlotCloak = 16, kSlotBag = 19, kBackShieldSlot = 23;
constexpr U16 kHandR = 0, kHandL = 1, kBowSlot = 2;
constexpr U16 kWeaponRecord = 0, kArmorRecord = 1; // QuickSlotInfo records = the gear wheel's tabs
constexpr U32 kRecSlotCount = 0x0c, kRecSlots = 0x18, kSlotSize = 0x50, kSlotEquipIdx = 0x20;
constexpr U32 kMaxWheelSlots = 16;

// (defined in main.cpp)
extern U64 g_base;
extern HMODULE g_self;
extern std::wstring g_dir; // the folder the .asi is in (with a trailing backslash)

} // namespace ghw

#include "mem.h"
