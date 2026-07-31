# Storage Network (StorageTerminal)

A UE4SS C++ mod for Subnautica 2 that searches every communal locker in your
base at once and takes you to the one holding what you need.

**You should never again have to remember which locker something is in.**
That is the whole product.

## What it does

- **Ask NoA.** Walk up to any NoA computer terminal and pick *"Access storage
  network"* from her dialogue. There is **no hotkey** — the base's storage
  index is something the base gives you, not something you carry around the
  world.
- **Type** — search the reachable communal storage plus your own pockets,
  incrementally. Matches the item's localized display name
  (`UUWEItemType::Name`), with the asset name as a secondary key.
- **Page Up / Page Down** — walk the lockers holding the current search term,
  nearest first. (Arrow keys are bound too, but the inventory widget consumes
  them for its own grid navigation.)
  With **no** search term this walks *every* locker, nearest first, **including
  empty ones** — which are exactly the ones you want when looking for somewhere
  to put something. Once you type a term, only lockers holding it are listed.
- **Escape** — close.
- The open screen's **title** carries one short line:
  `'tita'  Titanium x12  --  Ore Locker 14m  (2/5)`. The full match list goes
  to `UE4SS.log` — the screen's other text slots are not big enough to borrow
  (see `docs/UE4SS_API_REFERENCE.md`).

**Scope:** whatever `UWECraftingComponent::RegisteredSourceIds` says is
reachable — the same communal storage the fabricator draws from. No invented
radius. Falls back to every communal inventory (and logs it) if that is empty.

Taking items out is just the game's own storage screen, which already does
that well. The mod's job is to get you to the right screen.

## What it will never do

The mod **reads** game state and **opens** the game's own screens. Nothing
else. No copies, no mod-owned inventory, no injected UI entries, no item
moves. The single non-read call is
`UUWEInventoryInteractionComponent::InteractWithInventoryInteractionComponent`
— exactly what pressing interact on a locker does, and only when that
component's own `InventoryInteractionEnabled` gate allows it. So it is
server-authoritative, multiplayer-safe, and cannot corrupt a save.

Crafting integration, a buildable terminal, autocrafting and a merged
single-grid view are all out of scope. The merged grid and the mod-owned
mirror inventory were both built and abandoned — they caused real item
duplication in normal play. **Do not rebuild them.**

## Layout

| Path | What |
|---|---|
| `src/main.cpp` | Search, browse, screen open/close/reconcile, on-screen text |
| `src/NoaTerminal.*` | The way in: injects the dialogue option into every NoA terminal and detects the click |
| `src/SnapshotBuilder.*`, `src/SnapshotModel.hpp` | Read every communal locker + the player's inventory |
| `src/InventoryEventHook.*` | "Something changed" signal, nothing more |
| `src/ReflectionUtils.*`, `src/PropertyReflection.*` | Reflection primitives |
| `src/HookTargets.hpp` | Every game name used, with why it is safe |
| `docs/PROJECT.md` | **Start here.** Goal, non-goals, invariants, what cost a crash to learn, and what is genuinely unresolved. Every claim labelled CONFIRMED / SOURCE / THEORY. |
| `docs/UE4SS_API_REFERENCE.md` | UE4SS's own API: confirmed-safe vs confirmed-fatal calls, silent-failure traps, and the cost of each lookup primitive. **Read before writing reflection code.** |
| `docs/REVERSE_ENGINEERING_NOTES.md` | The game's inventory surface |

## Build

### vendor/ is not in this repo

`vendor/` is ~2.7 GB of third-party and game-derived material and is
deliberately gitignored. You need to recreate it locally:

- `vendor/RE-UE4SS-src` — clone [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)
  from source. Its private `deps/first/Unreal` submodule requires a GitHub
  account linked to an Epic Games account. Build with the **Visual Studio**
  generator, not Ninja (`corrosion`'s `/defaultlib:msvcrt` flag breaks Ninja on
  newer CMake).
- `vendor/SN2SDK` — a Dumper-7 dump of the game, used as **documentation only**;
  nothing compiles against it. Generate your own from your own copy of the game.
  It is not redistributed here.

The mod builds as a **cppmod inside the UE4SS source tree** — there is no
standalone CMake project. `vendor/RE-UE4SS-src/cppmods/StorageTerminal/
CMakeLists.txt` reaches back into `src/`.

```powershell
# build, close the game if it is holding the DLL, and deploy
.\scripts\install-mod.ps1 -Build -KillGame
```

Or by hand:

```
vendor\build_ue4ss_vs.bat            # full UE4SS + mod build (first time)
cmake --build vendor\RE-UE4SS-src\build_vs ^
      --config Game__Shipping__Win64 --target StorageTerminal
copy vendor\RE-UE4SS-src\build_vs\Game__Shipping__Win64\bin\StorageTerminal.dll ^
     ..\Subnautica2\Binaries\Win64\Mods\StorageTerminal\dlls\
```

Prerequisites: Visual Studio 2022 Build Tools (MSVC), CMake 3.25+, and a Rust
toolchain (UE4SS needs it).

## Install

The live mod root is `Subnautica2/Binaries/Win64/Mods` — **not** the nested
`Binaries/Win64/ue4ss/Mods`, which holds only a leftover `mods.txt`. The DLL
goes in `Mods/StorageTerminal/dlls/StorageTerminal.dll`, and
`StorageTerminal : 1` must be listed in `Mods/mods.txt`.

Log: `Subnautica2/Binaries/Win64/UE4SS.log`, **overwritten every launch**.
Grep for `[StorageTerminal]`.

## SDK

`vendor/SN2SDK` is a Dumper-7 signature dump used as **documentation only** —
nothing compiles against it. Every game call is resolved by reflection at
runtime, by property name and type. It is not committed or redistributed.

## Releasing

Version lives in `VERSION` and is mirrored into `ModVersion` in `src/main.cpp`.
Never edit either by hand — `bump-version.ps1` updates both plus a `CHANGELOG.md`
stub, and `publish-release.ps1` refuses to upload if the two ever disagree.

```powershell
.\scripts\bump-version.ps1 -Part patch     # or -Part minor / -Part major
#   ... edit the new section in CHANGELOG.md ...
.\scripts\publish-release.ps1 -WhatIf      # build + package, no upload
.\scripts\publish-release.ps1 -ProjectId <id>
```

The uploaded changelog is taken from the matching `## <version>` section of
`CHANGELOG.md`; publishing fails if that section is missing or is still the
generated stub.

### The CurseForge token

Read from the `CURSEFORGE_TOKEN` environment variable and **never stored in this
repo**. It is sent only as an `X-Api-Token` header — never in a URL query
string, never logged, never included in an error message.

```powershell
[Environment]::SetEnvironmentVariable("CURSEFORGE_TOKEN", "<token>", "User")
# then open a new shell
.\scripts\publish-release.ps1 -CheckAuth   # verify, and list game version ids
```

Optional: `CURSEFORGE_PROJECT_ID` and `CURSEFORGE_HOST` can be set the same way
so you do not have to pass them each time.
