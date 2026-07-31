# Storage Network — what it is, and what is actually known

Rewritten from scratch **2026-07-30**, replacing `HANDOFF.md`, `NORTH_STAR.md`
and `docs/archive/`. Those had accumulated theories written in the same voice as
verified facts, and the bad entries cost real debugging time — a load-bearing
false claim about which game calls the mod makes, three "open issues" that were
already fixed, and an elegant wrong explanation for a bug whose real cause was a
missing field name.

**Everything below is labelled. Nothing goes in here unlabelled.**

| Label | Means |
|---|---|
| **CONFIRMED** | Observed directly — in game, in a screenshot, in `UE4SS.log`, or in a crash dump. Dated. |
| **SOURCE** | Read out of UE4SS or the SDK dump. Cited. Says what the *engine* does — **not** what this game's Blueprints do. |
| **THEORY** | Not tested. States what would falsify it. |

If you cannot label a claim, you do not know it yet — leave it out. When a
claim is disproven, **mark it wrong in place**; the stale version is what the
next reader finds first. Prefer deleting a stale entry over leaving it to be
trusted.

---

## 1. The goal

> **You should never again have to remember which locker something is in.**

One gesture, from a NoA terminal in your base, answers in order:

1. **Do I have it at all?** — one place that knows every container you own.
2. **Where is it?** — named and locatable, never "locker 87".
3. **Can I just have it?** — the game's own storage screen already does this
   well; the mod's job is to get you to the right one.

**The test for every future change:** *does this reduce the number of things
the player must remember, or the number of places they must walk to?* If not,
it is not on the path.

## 2. Non-goals (settled — do not relitigate)

- **No crafting integration.** Explicitly descoped by the user: "we do not need
  the craftable stuff since that exists in base game."
- **No mod-owned inventory, ever.** No copies, no mirror, no synthetic
  container. **CONFIRMED** this caused real item duplication in normal play,
  and the duplicates persist in saves.
- **No merged single-grid view** rendered by the game's inventory widget.
- **No wireless/AE2-style network, no autocrafting, no buildable terminal.**
- **No global hotkey.** Access is deliberately gated behind walking up to a NoA
  terminal — user's words: "the goal is to not have the storage accessible from
  anywhere in the world."

## 3. Invariants

1. The mod never holds an item. Not for a frame.
2. Any mutation must be a single call the game already arbitrates server-side.
3. **The mod never reacts automatically to an event it caused.** Guard by
   identity, or do not react at all. **CONFIRMED** — this bit the project four
   separate times (see §5).

## 4. What the mod does today

Reached by picking *"Access storage network"* at any NoA computer terminal.
Type to search every reachable communal locker plus your own pockets; Page
Up/Down or the arrow keys walk the lockers, nearest first (with no search term
that includes empty lockers). Escape closes. The screen's title carries the
state; the full match list goes to the log.

**Its complete game-facing surface** — **CONFIRMED** 2026-07-30 by grepping the
source. Re-check with `grep -rn "ProcessEvent(" src/` before trusting this:

- **Six `ProcessEvent` calls:** `GetItems` (const), `GetActiveWidget`, `Pop`,
  `SetText`, `Interact`, `CloseUI`.
- **Non-item state writes:** `NewObject` of one dialogue data asset per
  terminal; appending it into that component's `ExtraRootDialogueData` array;
  three `FText` writes into it; `ShowInventoryTitle` bool writes on inventory
  widgets.

None of those add, move or delete an item. That makes a mod-caused item
duplication unlikely — but it is a **conclusion from the above list**, not an
independently audited fact.

## 5. Things that cost a crash or a corrupted save to learn

These are the entries worth keeping: each was paid for once, and none can be
recovered by reading source.

- **Reacting to your own events breaks things — four times. CONFIRMED.**
  (a) mirror sync fired inventory events read back as player deposits, force-
  filling the base until it crashed; (b) a per-frame re-push fought the game's
  viewmodel rebuild; (c) refresh events re-entered our own hook, 628 events in
  2s; (d) opening a screen fired events → rescan → auto-advance → another
  screen → hang. Timing guards do **not** work: notifications arrive a tick or
  more late via fast-array replication.
- **Copying an item struct copies its `ItemId` GUID. CONFIRMED** — two
  "different" items share an identity, id-keyed operations hit the wrong one.
  Caused eating one item twice, and a deposit that cloned itself.
- **A mod-created inventory cannot be hidden. CONFIRMED** — it registers in
  `UWEInventorySubsystem::StorageActors`, crafting sees it, it persists into
  saves, and it dragged `HighestInventoryId` up so two real containers got the
  same id after a save/load.
- **The inventory viewmodel is authoritative. CONFIRMED** — it rebuilds from
  its bound inventory; injected entries do not survive (35 pushed, 17 alive a
  second later, widget content list never above 0).
- **The game has no item stacking. CONFIRMED** — every stack cap is 1;
  `WBP_InventoryEntry_C` has no count widget. `SnapshotBuilder` folds same-type
  stacks itself.
- **Never `memcpy` a struct containing an `FString`/`TArray`. CONFIRMED** —
  double free, crashes about a second later.
- **`UClass::CreateDefaultObject()` called by hand crashes hard, no dump.
  CONFIRMED, twice.** Use `NewObject`, or better, hijack a real game function.
- **`UFunction::RegisterPostHook` crashes when the hook first fires.
  CONFIRMED.** Use `Hook::RegisterProcessEventPostCallback` instead.
- **Registering a ProcessEvent callback from inside a keypress handler crashes
  instantly. CONFIRMED** — it mutates the dispatcher list mid-iteration.
  Register at startup, arm with flags.
- **Only ever pop the Modal layer (3). CONFIRMED** — popping other layers
  removed the HUD and broke input; popping the player's PDA crashed the game.
- **`DeactivateWidget()` does not close these screens. CONFIRMED.** Use
  `WindowManager::GetActiveWidget(3)` + `Pop(...)`.
- **`AddRootDialogueOption()` does not add a NoA option. CONFIRMED** — it
  leaves `ExtraRootDialogueData` untouched and only writes
  `MergedRootDialogueData`, which never surfaces. Append to
  `ExtraRootDialogueData` directly.
- **Calling interact while a screen is already open does nothing. CONFIRMED** —
  and the pop must be separated from the interact by at least one frame, or the
  pop lands with nothing to replace it and leaves the game with an empty Modal
  layer and stale input state.
- **Popping and pushing native screens 2–3× per second crashes the game.
  CONFIRMED** — two dumps, `crash_2026_07_30_19_01_41` and
  `..._20_46_29`. The second had every object-liveness guard already in place,
  which rules out the mod reading torn-down objects. Switching is now debounced
  to one screen cycle per input burst.
- **`FText::ToString()` can return `<MISSING STRING TABLE ENTRY>` and look like
  a successful read. CONFIRMED** — for locker names, item names, and the game's
  own dialogue options. Validate every text read and fall back to a derived
  name. **It tells you nothing about what the game does with that field** — that
  misreading nearly caused a working fix to be reverted.
- **Only the screen title is safe to write. CONFIRMED** — a multi-line list in
  `DescriptionText` rendered over the tab bar and inventory header.
- **`is_transient()` also hides `/Engine/Transient`,** where UMG widgets and
  `UGameInstanceSubsystem`s live. **CONFIRMED** — silently broke three
  features. Use `find_all_unfiltered` / raw `FindFirstOf` for those.

## 6. Open, and honestly unresolved

- **Does the debounce actually stop the browse crash?** Fix shipped
  2026-07-30, **not yet retested** under the spam that reproduced it.
- **Does a screen bound to an EMPTY inventory auto-close?** An old note claimed
  `WBP_Inventory_C::OnInventoryEmpty` does exactly that. **THEORY — untested,
  and it now matters**, because empty lockers were deliberately made browsable.
  Falsified by: opening an empty locker and seeing its screen stay up.
- **The 2026-07-29 item-duplication report** (spam-depositing with a macro) was
  never reproduced or attributed. Triage: run the macro with
  `StorageTerminal : 0` in `mods.txt`.
- **Opening a locker ignores distance** within the reachable set. The
  interaction-enabled gate is respected; range is not.

## 7. Build, deploy, debug

```powershell
StorageTerminalMod\scripts\install-mod.ps1 -Build -KillGame
Start-Process "steam://rungameid/1962700"
```

Builds as a cppmod inside `vendor/RE-UE4SS-src/cppmods/StorageTerminal`, which
reaches back into `src/`. There is no standalone CMake project.

`UE4SS.log` (`Subnautica2/Binaries/Win64/`) is **overwritten every launch** —
read it before relaunching after a crash, or the evidence is gone. Grep
`[StorageTerminal]`.

See also `UE4SS_API_REFERENCE.md` (safe/unsafe call patterns and the cost of
each lookup primitive) and `REVERSE_ENGINEERING_NOTES.md` (the game's inventory
surface).
