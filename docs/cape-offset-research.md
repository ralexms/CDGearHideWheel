# Cape offset: research notes

Crimson Desert 1.0.0.3576 (Steam build 26480954). Found by reading the game's own data archives and exe; nothing in
the game was modified.

## The mechanism: the "stack equip" system

Weapons on the back are not offset by the cape itself. Every worn mesh can declare a **thickness** at named
**stack points**, and the body adds up the thickness of its *inner parts* and pushes the attachment bone out by that.

### 1. The body defines the stacks

`character/descriptors/socketbonedata/1_pc/1_phm/phm_01.pab.sockets.xml` (Kliff; Oongka has
`phm_oongka_01.pab.sockets.xml`, Damiane a `phw` one):

```xml
<StackEquipInfo Count="5" EquipTypeName="Back" InnerPartNames="Cloak Upperbody"
                OriginBoneName="Bip_Weapon_Attach_In_02" Axis="z" PushOriginBone="True">
    <Socket Name="Spine2_B_MainWeapon_Socket"/>
    <Socket Name="Spine2_B_SubWeapon_Socket"/>
    <Socket Name="Spine2_B_RangeWeapon_Musket_Socket"/>
    <Socket Name="Spine2_B_RangeWeapon_Socket"/>
    <Socket Name="Spine2_B_Shield_Socket"/>
</StackEquipInfo>
<StackEquipInfo Count="1" EquipTypeName="Shoulder_L" InnerPartNames="Cloak Upperbody" OriginBoneName="Bip01 L Clavicle" Axis="y">
<StackEquipInfo Count="1" EquipTypeName="Shoulder_R" InnerPartNames="Cloak Upperbody" OriginBoneName="Bip01 R Clavicle" Axis="-y">
<StackEquipInfo ... EquipTypeName="Pelvis_L" / "Pelvis_R" InnerPartNames="Upperbody" ...>
<StackEquipInfo ... EquipTypeName="Ring_L" / "Ring_R" InnerPartNames="Hand" ...>
```

All five back sockets hang from the bone `Bip_Weapon_Attach_In_02`; with `PushOriginBone="True"` that bone is moved
along `z` by the summed thickness of the `Cloak` and `Upperbody` parts.

### 2. Each mesh declares its thickness

In the piece's `character/modelproperty/.../*.pac_xml`, `SkinnedMeshPropertyCommon` -> `_customGameData`:

```xml
<StackEquipDataContainer ItemID="586" _equipType="Back" _offsetLength="0.130000"/>
<StackEquipDataContainer ItemID="587" _equipType="Shoulder_L" _offsetLength="0.020000"/>
```

(`cd_phm_00_cloak_00_0052.pac_xml`.) Survey of the 6553 player `pac_xml` files, `Back` entries:

| Category | Files with Back | Thickness |
|---|---|---|
| 19_cloak | 173 of 241 (phm) | 0.015 - 0.28 m |
| 9_upperbody | 54 | 0.001 - 0.09 m |
| 3_shield | 140 | 0.02 - 0.44 m |
| 2_twohandweapon | 224 | 0.02 - 0.09 m |
| 4_bow, 8_musket, 12_pike, ... | | 0.03 - 0.11 m |

Weapons and shields have their own `Back` thickness too: they are the outer layers of the same stack (ordering per
`character/descriptors/backequipcategoryorder.xml`, whose entries are commented out = engine defaults).

**Why a hidden cape leaves weapons floating:** the sum counts the `Cloak` part because it is equipped, whatever its
visibility. Hiding it on a visibility layer (this mod, Gear Hider) doesn't change the sum.

### 3. In the exe

| What | Where (RVA) |
|---|---|
| `StackEquipDataContainer` reflection type | string `+5F016A0`, used at `+26E0C0` |
| `_equipType` setter | `+2F1A700` |
| `_offsetLength` setter: the float is at **container +0x2C** | `+2F1AB70` |
| `SocketBoneData` parser (reads `StackEquipInfo`, `EquipTypeName`, `InnerPartNames`, `OriginBoneName`, `Axis`, `PushOriginBone`, the sockets) | `+2FDF5E0` |
| `InnerPartNames`: split on spaces, each name interned with `+149F960` (the same part-name ids as a model part's `+0x48`) into a u32 vector | `+2FE0068` .. `+2FE016E` |

### 4. The stored records

The parser appends one 0x40-byte record per `StackEquipInfo` to a vector in the SocketBoneData object: pointer
`+0x48`, count `+0x50` (`+2FE054E` .. `+2FE05F8`):

| Offset | Field |
|---|---|
| `+0x00` | u8 stack type (Back, Shoulder_L, ...; the `_equipType` enum) |
| `+0x08` / `+0x10` / `+0x14` | inner part name ids / count / capacity |
| `+0x18` / `+0x20` / `+0x24` | socket name ids / count / capacity |
| `+0x28` | origin bone name id |
| `+0x2C` | axis, 3 floats |
| `+0x38` | u8 PushOriginBone |

A thickness entry (`StackEquipDataContainer`): `+0x28` u8 stack type, `+0x2C` float `_offsetLength`.

### 5. The consumer: `+2F2A410 (model, SocketBoneData**)`

Called (from the protected `.tls$` region, `+E79C172`) as a pair, when the model's skeleton/socket data is ready:

```
call +2F2A410 (model, &socketBoneData)   ; build the model's stacks
jmp  +2F2A840 (model)                    ; apply them: thickness * axis -> the origin bone / sockets
```

`+2F2A410`:
- takes `[arg2] + 0x48` (the record vector) - nothing else of the SocketBoneData is read;
- copies each record into the model's own stack state at `model+0x20` (its own copy of the inner-part ids);
- for each inner part id: binary search in the model's part table (`model+0x58`, count `+0x60` - the table this mod
  already uses), skips parts not loaded (`+0x15`);
- for each found part, walks its mesh's `_customGameData` entries and takes the one whose type (`+0x28`) matches the
  stack; the stack's thickness is the **maximum** (`vmaxss` at `+2F2A827`, not a sum) over the inner parts:
  state `+0x1C` (float), `+0x18` = 1.

It ignores part visibility (that's the floating).

`+2F2A840 (model)` multiplies the state thickness by the record's axis (`+0x2C..+0x34`) and applies it.

## Fix design

1. **Hook `+2F2A410`.** For a model whose character has the cape hidden by this mod, call the original with a
   *filtered copy* of the records: the `Cloak` part id (interned like the parser does, `+149F960`) removed from every
   record's inner list. The game then computes Back / Shoulder from the chest piece alone. The copies are kept per
   model while it lives (the function might keep pointers into the records).
2. **On a cape toggle:** run the pair again for that character's model (`+2F2A410` with the SocketBoneData pointer
   captured from the game's last call for that model, then `+2F2A840`). Which thread it is safe on is the open
   question: the diagnostic build logs the game's own calls (thread, model) first.
3. Model -> character: the mod's own model/inventory map, plus a re-run on the first sync if the builder ran before
   the mod knew the model.
4. Overhead: the hook runs only when a model's stacks are built (load, gear changes), never per frame.

Fallbacks: zero the hidden cape's runtime `_offsetLength` (shared per mesh resource), or a DMM data mod with
`InnerPartNames="Upperbody"` (weapons then clip through every visible cape).

## Shoulders and hair under hidden capes

`PostfixEval` (Gear Hider's name; `+2F45E00` in its log) evaluates the postfix rules over the equipped pieces and
ignores a piece whose **highest deciding visibility layer** says hidden (Gear Hider's bald fix sets layer 3 hidden for
the call; the game's own "always hide headgear" does the same: mask `0x00010001 -> 0x00090001`). This mod's layer 8
is the highest, so a cape it hides should already stop hiding the shoulders / hair once the rules are evaluated again.
To check in game with a cape that normally hides the shoulder armour.

## Related finding: capes also hide other parts

`character/descriptors/conditionalpartprefab/conditionalpartprefab_postfix.xml`: a piece whose prefab name ends in a
postfix hides other parts while worn. Capes use `_S`, `_T` (hide `Shoulder*`, `Bag_Belt*`), `_V` (also hair and
helm), `_Y` (helm, shoulder). A cape hidden by this mod therefore still hides the shoulder armour and bag belt it
covers. Gear Hider's `PostfixEval` hook (its "bald fix") works on the same rules.

## Tools used

Scratchpad scripts (not in the repo): a PAMT/PAZ reader (ChaCha20 with a filename-derived key, LZ4; format as
documented in Outfit Manager's `tools/pamt.py` / `paz.py`) and a capstone-based disassembler over the exe on disk.
