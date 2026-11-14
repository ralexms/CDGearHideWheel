# Gear Hide Wheel (Crimson Desert)

Hide or show each piece of gear right from the game's own gear wheel, separately for Kliff, Damiane and Oongka.
No key combinations in play: open the gear wheel, highlight a piece, press one key.

Status: **0.6.3.** It is built for Crimson Desert 1.0.0.3663.

## What it does

- **Toggle in the gear wheel.** On the Weapons or Armour tab, highlight a piece and press the toggle key
  (default `H` / `Y`, Triangle on PlayStation). The piece is hidden or shown again. It stays equipped, so you keep its stats.
- **Per character, remembered.** Each character keeps their own choices. They are saved in
  `GearHideWheel.state` beside the `.asi` and applied automatically when you load any save.
- **The key shows in game.** The item name in the wheel's centre panel gets the piece's state and the key to press,
  e.g. `Leather Cloak - Hidden [H]`. The key shown follows the input you last used (keyboard or controller). Each
  press also shows a notification with the new state.
- **Headgear modes per character.** Highlighting the Headgear slot cycles that character's headgear mode:
  *Always shown → Always hidden → Shown in combat → Hidden in cutscenes*. The game's own headgear logic does the
  work: the mod gives it that character's mode in place of the global option, only while the game is deciding that
  character's headgear. Your setting in the game's options menu is left as it is and is used by any character you
  never set a mode for.
- **Secondary key** (default `U` / `X`, Square on PlayStation):
  - **Underwear:** with the Chest slot highlighted, it hides or shows the underwear. This helps with modded armour
    that clips through it.
  - **Shields:** with a weapon slot highlighted (the off hand, which takes a sword or a shield), it cycles that
    character's shield mode: *Shown → Hidden → Shown when equipped*. "Shown when equipped" shows the shield in the
    off hand and hides it on the back while a sword or another weapon takes the off hand. The toggle key covers only
    the other weapons in that slot, so you can hide the shield and keep the off-hand sword visible.
  - **Mobility packs:** with the Backpack slot highlighted, it cycles that character's mode for packs they move with
    (Oongka's Kuku Rocket Pack): *Shown → Hidden → Shown when in use*. "Shown when in use" hides the pack and shows it
    while flying (from take-off, when the game puts on the flight goggles, to landing). The toggle key covers the
    other packs (Disruptor, Explorer's Breathing Device), so each kind keeps its own setting when you switch packs.
- **Weapons.** A hidden weapon still appears in your hand when you draw it. Its scabbard stays hidden, including the
  two-handed sword's sheath on the back (`TwoHandWeapon_Sword_IN`). A hidden bow hides its quiver and arrows.
- **Tools.** The game normally shows tools (broom, pickaxe, shovel...) only in your hand. The mod also shows them on
  the character while they are put away, as Gear Hider did (`ShowStowedTools`). Hidden tools stay hidden.
- **No floating weapons under hidden capes.** The game hangs back weapons further out when a cape is worn, so they
  don't clip through it, and it counts the cape whether it is visible or not. With the cape hidden, the weapons hang
  as if no cape were worn; the back layout also follows what you hide on it (a hidden bow no longer pushes the shield
  out). See `docs/cape-offset-research.md` for how the game does it.
- **Coexists with Outfit Manager.** It waits for Outfit Manager to finish setting up, then chains onto its hooks. It
  hides on its own visibility layer (8), above Outfit Manager's (6) and the game's (0-5, 7), and only ever hides or
  lets go. So it never forces anything visible that another mod or the game is hiding.

## Install

**With DMM:** drop `GearHideWheel-0.6.3.zip` onto DMM (or Import it), enable `GearHideWheel` on the ASI Mods page,
then Mount Mods. DMM puts the `.asi` and `.ini` in `bin64`, and the mod writes its `.state` file there too.
- Your choices live in `bin64\GearHideWheel.state`. Back it up before you uninstall or replace the mod in DMM.
- Re-importing the zip may reset `GearHideWheel.ini` to the defaults. Keep a copy if you changed your keys.

**By hand:** with an ASI loader in `<Crimson Desert>/bin64` (e.g. Ultimate ASI Loader as `winmm.dll`), copy
`GearHideWheel.asi` and `GearHideWheel.ini` into `bin64`.

Then start the game. `GearHideWheel.log` appears beside the `.asi`. Its last start-up line should read
   `ready: hiding on, per-character headgear on, cape offset fix on, gear wheel on`.

**Gear Hider (mod 554):** this mod replaces it. If you keep both, anything either one hides stays hidden, and both
compete for the same "weapon drawn" hook. Remove `CrimsonDesertEquipHide.asi` (and its `.ini`) from `bin64`, or move
them out of the game folder entirely (the ASI loader also loads from subfolders). If you installed Gear Hider through
DMM, disable or delete it there instead.

To uninstall, delete `GearHideWheel.asi`. Nothing in your saves is changed: hidden pieces are only hidden on screen.

## Configure (`GearHideWheel.ini`)

```ini
[Keys]
Toggle = H, Gamepad_Y           ; hide / show the highlighted piece (cycles the mode on Headgear)
Secondary = U, Gamepad_X        ; underwear (Chest) / shields (weapon slots) / mobility packs (Backpack); "Underwear =" from 0.1.0 still works

[Options]
ShowPrompt = true               ; state + key on the item name in the wheel's centre panel
Notifications = true            ; a notification with the new state on every press
ShowStowedTools = true          ; tools also show while put away (as with Gear Hider)
CapeOffsetFix = true            ; hidden capes no longer push the weapons on the back outward
CapeOffsetRefresh = true        ; apply that at once on a toggle (false: on the next load / gear change)
Companions = true               ; apply a character's choices while they follow you as a companion
HeadgearCycle = Shown, Hidden, Combat, Cutscenes
Verbose = false                 ; more detail in the log (dumps the model's parts on each change)
```

Keys: `A`-`Z`, `0`-`9`, `F1`-`F24`, `Numpad0`-`9`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Tab`,
`Mouse3`-`Mouse5`, modifiers `Ctrl` / `Shift` / `Alt`. Controller buttons: `Gamepad_A/B/X/Y`, `Gamepad_LB/RB/LT/RT`,
`Gamepad_LS/RS` (stick clicks), `Gamepad_Back/Start`, `Gamepad_DpadUp/Down/Left/Right`. Xbox pads, Steam Input and
DualSense / DualShock 4 (read directly) all work. Combine with `+`, list alternatives with `,`.

Pick buttons the gear wheel itself doesn't use: the mod sees the press, and so does the game.

## First test checklist

Please check these, and send `GearHideWheel.log` (with `Verbose = true`) if something is off:

1. **Start-up.** The log ends with `ready: hiding on, per-character headgear on, cape offset fix on, gear wheel on`. The `headgear:`
   line names the option's value. With your menu set to *Always shown* it should say `is 0 (Always shown)`. If it
   shows a different number, the mode names are mapped in the wrong order, so tell me the number.
2. **Prompt.** Open the gear wheel and highlight a cloak: the centre panel's name reads `<cloak> - Shown [H]`.
3. **Toggle.** Press `H`: the cloak disappears, the name reads `<cloak> - Hidden [H]`, and a notification says so.
   Press again: it comes back.
4. **Weapons.** Hide a two-handed sword: the sword and its sheath on the back go. Draw it: the sword appears in your
   hands. Sheathe it: it goes again.
5. **Headgear.** Highlight the hat and press `H` a few times; each press moves to the next mode (also check *Hidden in
   cutscenes* in a cutscene, and *Shown in combat* in a fight).
6. **Underwear.** Highlight the chest and press `U`.
7. **Shields.** Highlight the off-hand slot: the name shows `Weapon: ... [H] | Shield: ... [U]`. Press `U` to cycle
   the shield mode. On *Shown when equipped*, put a sword in the off hand: the shield disappears from the back.
   **Mobility packs.** As Oongka with the rocket pack, highlight the Backpack slot and press `U` until *Shown when in
   use*: the pack is gone, and appears while flying. Switch to a utility pack: it follows the `H` setting instead.
8. **Per character.** Switch to Damiane: her own choices apply, and Kliff's stay his.
9. **Persistence.** Quit, start the game and load a save: everything is hidden as before.
10. **Outfit Manager** (if installed): its Outfits tab still works, and its log shows no `feature off` lines.

## Known limitations

- **The prompt** uses the centre panel's item-count line, which the game leaves empty for equipment. On a slot whose
  item shows a count (ammunition), the prompt is not shown, but the toggle still works.
- The PlayStation-to-Xbox button names are a mapping (Cross = A, Circle = B, Square = X, Triangle = Y, L1 = LB, ...).
- After a game update, the mod finds its game functions again by their code patterns. If something can't be found,
  only that feature turns off and the log says which.

## Files

| File | What |
|---|---|
| `GearHideWheel.asi` | the mod |
| `GearHideWheel.ini` | your settings (written with defaults if missing) |
| `GearHideWheel.state` | what each character hides (written by the mod, plain text) |
| `GearHideWheel.cache` | game addresses for your game version (rebuilt after an update) |
| `GearHideWheel.log` | the log, rewritten each start |

## Building

Visual Studio 2022 (Desktop C++), then `build.cmd`. Output: `build\GearHideWheel.asi`. MinHook v1.3.4 is vendored in
`third_party/minhook`.

| Source | What |
|---|---|
| `src/main.cpp` | start-up: waits for Outfit Manager, finds the game's functions, installs everything |
| `src/wheel.cpp` | the gear wheel hooks: the highlighted slot, the presses, the prompt |
| `src/vis.cpp` | the visibility engine (our layer 8): pieces, scabbards, quivers, drawn weapons, the underwear |
| `src/headgear.cpp` | the per-character headgear mode (the option byte swapped around the game's five readers) |
| `src/stack.cpp` | the back equipment stack: built without a hidden cape, rebuilt when the mod hides / shows pieces |
| `src/state.cpp`, `src/config.cpp` | the `.state` and `.ini` files |
| `src/input.cpp` | keyboard, XInput and PlayStation (HID) input |
| `src/gear.cpp`, `src/ui.cpp` | the player, characters and worn gear; UI elements, toasts, item names |
| `src/addrs.*`, `src/sigs.h`, `src/resolver.*`, `src/game.*` | game addresses and the resolver that finds them again after updates |

## Credits and licence

- **Outfit Manager** by Strahlimeier (MIT): the address resolver, signature table, gear-wheel layout, visibility-layer
  model, part names, weapon cases and controller reading this mod is built on. See
  `src/third_party/OutfitManager-LICENSE.txt`.
- **Gear Hider** by tkhquang: the original idea and the part-name lists that showed which scabbard parts exist.
- **MinHook** by Tsuda Kageyu (BSD 2-Clause), `src/third_party/MinHook-LICENSE.txt`.
- Crimson Desert belongs to Pearl Abyss.

Gear Hide Wheel is released under the MIT licence (`LICENSE.txt`).
