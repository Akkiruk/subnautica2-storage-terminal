# UE4SS Native API Reference & Safe Patterns

Read this before writing new reflection code. Almost every crash and dead-end
in this project's history came from hand-rolling something via generic
reflection (`ProcessEvent` + a manually built params buffer) that UE4SS
*already exposes as a real, compiled C++ function* on a native-modeled type.
Check the tables below first. Only fall back to generic `PropertyReflection`/
`ReflectionUtils` reflection helpers for game-specific types that UE4SS
doesn't model natively (almost everything under `vendor/SN2SDK/SDK/*.hpp` --
those are Dumper-7 signature dumps, not compiled types, so *do* need
reflection to call).

See `REVERSE_ENGINEERING_NOTES.md` for the game's own SDK surface (inventory,
crafting, etc.). This file is about UE4SS's own compiled API.

## Natively-modeled types (real C++ classes, not reflection)

UE4SS ships compiled C++ headers for a small set of core engine types under
`vendor/RE-UE4SS-src/deps/first/Unreal/include/Unreal/*.hpp`. If a type is in
this list, prefer calling its real member functions directly over reflection.

| Header | Type | Notable real (non-reflected) functions |
|---|---|---|
| `UObject.hpp` | `UObject` | `IsA<T>()`, `GetName()`, `GetFName()`, `GetOutermost()`, `GetTypedOuter<T>()`, `GetPathName()`, `GetFullName()`, `ProcessEvent(Function, Params)`, `GetPropertyByName(Name)` / `GetPropertyByNameInChain(Name)` **(returns `FProperty*` directly -- equivalent to our hand-rolled `PropertyReflection::find_property`)**, `GetValuePtrByPropertyName<T>(Name)` / `GetValuePtrByPropertyNameInChain<T>(Name)` **(returns a typed value pointer directly, skipping the FProperty+ContainerPtrToValuePtr dance for simple cases)**, `GetFunctionByName(Name)` / `GetFunctionByNameInChain(Name)` (already our standard pattern for function lookup) |
| `UClass.hpp` | `UClass` | `CreateDefaultObject()` -- **CONFIRMED UNSAFE to call manually** (crashed hard, no dump, twice in this project). Don't call it. See "Confirmed-unsafe calls" below. |
| `UStruct.hpp` / `UField.hpp` | `UStruct`, `UField` | property/function iteration (`ForEachProperty`, `ForEachPropertyInChain` -- both deprecated in favor of `TFieldRange<FProperty>`, but still work and are what this project uses) |
| `UFunction.hpp` | `UFunction` | `GetPropertiesSize()`, hooking via `RegisterPostHook` -- **CONFIRMED UNSAFE**, see below |
| `FProperty.hpp` / `CoreUObject/UObject/UnrealType.hpp` | `FProperty` family (`FIntProperty`, `FBoolProperty`, `FObjectProperty`, `FClassProperty` (extends `FObjectProperty`), `FWeakObjectProperty`, `FArrayProperty`, `FStructProperty`, `FTextProperty`, `FStrProperty`, `FNameProperty`) | `ContainerPtrToValuePtr<T>()`, `CastField<T>()`. This project's `PropertyReflection` wraps these. |
| `AActor.hpp` | `AActor` | `GetComponentsByClass(UClass*) -> TArray<UObject*>` **(real, compiled, no-ProcessEvent component enumeration -- use this instead of guessing component ownership by string-matching full names)**, `GetTypedOuter<T>()` (inherited), `GetWorld()`, `K2_GetActorLocation()`, etc. |
| `UActorComponent.hpp` | `UActorComponent`, `USceneComponent` | Just type tags for `Cast<T>()`/`IsA<T>()` -- no extra members beyond `UObject`. |
| `UObjectGlobals.hpp` | free functions | `FindFirstOf`/`FindAllOf(ClassName)` -- see "Object lookup" below for their real filtering behavior; `FindObject(ClassName, ObjectShortName)` -- lower-level, no CDO filtering; `NewObject<T>(Outer, Class)` -- safe object construction (see below) |
| `GameplayStatics.hpp` | `UGameplayStatics` | `GetAllActorsOfClass`, `BeginDeferredActorSpawnFromClass`/`FinishSpawningActor`, `FindNearestActor` |
| `World.hpp` | `UWorld` | `SpawnActor(...)` |
| `Hooks.hpp` | `RC::Unreal::Hook` | `RegisterProcessEventPostCallback` -- **CONFIRMED SAFE** event-observation mechanism, see below |
| `Mod/CppUserModBase.hpp` | `CppUserModBase` | `register_keydown_event(Input::Key, callback)` -- real hotkey registration, used for this project's F9/F10 test keys |

Everything else (every class under `vendor/SN2SDK/SDK/*.hpp`, i.e. the entire
game) is a Dumper-7 *signature dump*, not a compiled type here -- there is no
native C++ way to call `UUWEInventoryComponent::GetItems()` directly. Those
calls must go through `ReflectionUtils::find_function` +
`PropertyReflection::call`/`ProcessEvent`.

## Object lookup: what actually matches what

- `FindAllOf(ClassName, OutStorage)` walks every live `UObject` and matches if
  the object's own class name **or any class in its superclass chain**
  equals `ClassName` -- so searching for a base class name *does* also match
  subclass instances. It explicitly **excludes** class-default-objects
  (`RF_ClassDefaultObject`) and `UClass` objects themselves
  (`IsValidObjectForFindXOf` in UE4SS's own `UObjectGlobals.cpp`).
- Consequence: a class that only ever exists as its own CDO (e.g. a
  stateless Blueprint Function Library like `UWidgetBlueprintLibrary`, or an
  MVVM ViewModel that's torn down when its screen closes) can **never** be
  found via `find_first`/`find_all`. Use `FindObject(FName("Class"),
  FName(ClassName))` instead (see `ReflectionUtils::find_class_by_name`-style
  helpers) -- this lower-level overload has no such filtering and returns the
  `UClass*` object directly.
- `ReflectionUtils::is_transient()` (this project's shared filter, used by
  `find_first`/`find_all`) also excludes `/Temp/` preview worlds,
  `/Engine/Transient`, **and `ClientLobby`** (the main menu world -- added
  after a confirmed bug where a menu-to-gameplay transition left stale
  ClientLobby objects briefly resolvable, permanently poisoning a one-shot
  cached lookup for the rest of the session).
- **That filter hides things you legitimately need.** UMG widgets
  (`WBP_*_C`, their text blocks, etc.) and `UGameInstanceSubsystem`s
  (`WindowManager`) genuinely live under `/Engine/Transient`, so
  `find_first`/`find_all` silently return nothing for them. This caused two
  separate "the call did nothing" mysteries (the WindowManager close path
  and the screen-title write). Use `ReflectionUtils::find_all_unfiltered`
  or a raw `UObjectGlobals::FindFirstOf`/`FindAllOf` for those. **When a
  lookup returns empty for an object you can see in the game, suspect this
  filter first.**

## Cost of the lookup primitives (read before putting one on a hot path)

Correctness aside, these differ by orders of magnitude. Audited 2026-07-29
against the vendored UE4SS source.

| Call | Cost | Memoized by UE4SS? |
|---|---|---|
| `UStruct::FindProperty(FName)` | chain walk, `FName` compares | **Yes** -- static `{UStruct*, FName}` map (`src/UStruct.cpp:155`) |
| `UObject::GetFunctionByNameInChain` | chain walk, `FName` compares | **No** -- walks every call (`src/UObject.cpp:637`) |
| `FindAllOf` / `FindFirstOf` | **full walk of every live `UObject`**, `std::function` indirect call per object, superclass-chain walk per object | No |
| `UObject::GetFullName()` | class-name allocation + **recursive** `GetPathName` up the whole outer chain with string appends (`src/UObject.cpp:573`) | No |
| `FField::GetName()` | `GetFName().ToString()` -- **allocates a `std::wstring` per call** (`FField.hpp:222`) | No |

Rules that follow:

- **Never hand-roll a property search.** `UStruct::FindProperty` already walks
  with `IncludeSuper` (so it handles the Blueprint-subclass case that broke
  `ForEachProperty`), compares `FName`s instead of allocating a string per
  property, *and* memoizes. This project hand-rolled a slower uncached version
  for three sessions before noticing.
- **Build `FName`s with `FNAME_Find`, not the default.** `FName(str)` defaults
  to `FNAME_Add`, which interns the name into the engine's global name pool --
  so speculative lookups (the `"Name"` / `"Name_0"` candidate pairs this
  project uses) permanently added pool entries on every call. `FNAME_Find`
  only consults the existing pool and yields `NAME_None` on a miss; treat
  `IsNone()` as "not found" rather than comparing it, or a lookup for a
  nonexistent field will match any nameless member. UE4SS's own
  `BPMacros.hpp` uses `FNAME_Find` throughout.
- **Cache every `UFunction*` you resolve on a hot path.** It is *not* memoized,
  and `UFunction*`/`FProperty*` are class-level -- valid for the life of the
  process once found.
- **Do not cache the `UObject*` you found it from** if you will `ProcessEvent`
  on it. A stale `UFunction*` is inert; a stale object pointer is a crash.
  Resolve the object once per operation and pass it down instead. (`main.cpp`'s
  `open_locker` resolved the `WindowManager` five separate times -- five full
  object-array walks -- before this was applied.)
- **Prefer `GetOutermost()->GetName()` over `GetFullName()`** for package
  tests. `/Temp/`, `/Engine/Transient` and `ClientLobby` are all package-level
  path components, and a package's own `FName` is its full path, so the
  outermost package's name is sufficient -- without the recursive path walk.
  This is what `ReflectionUtils::is_transient` now does.

## Constructing new objects: what's actually safe

| Approach | Status | Notes |
|---|---|---|
| `UObjectGlobals::NewObject<UObject>(Outer, TargetClass)` | **Safe, established.** | Used for the NoA dialogue data object, the `SN2InventoryViewModel`, and the `WBP_Inventory_C` widget instance itself. `TargetClass` can come from an existing instance's `GetClassPrivate()`, or be resolved directly by name via `FindObject(FName("Class"), FName(name))` + `Cast<UClass>` (no CDO/manual construction needed first). |
| `UClass::CreateDefaultObject()` called manually | **CONFIRMED UNSAFE.** Crashed hard (no crash dump at all) twice: once resolving `UWidgetBlueprintLibrary`'s CDO before calling `Create()`, once resolving `SN2InventoryViewModel`'s CDO directly. | Don't call this yourself. If you need a `UClass*` to hand to `NewObject`, resolve it via `FindObject` and stop there -- let `NewObject`'s own internal construction path create the CDO on demand through the engine's normal mechanism, not your own explicit call. |
| `UWidgetBlueprintLibrary::Create(WorldContextObject, WidgetType, OwningPlayer)` via raw `ProcessEvent` | **CONFIRMED UNSAFE.** Crashed hard (no dump) the one time it was tried. | Complex multi-param static library call doing substantial internal Slate/UMG setup -- too much surface area to trust via a hand-built params buffer. Use `NewObject` + the widget's own simple setters (e.g. `SetOwningPlayer`) instead, or better, avoid constructing the widget yourself entirely (see next section). |

## The real lesson of this project: hijack real game functions instead of rebuilding UI

Building `WBP_Inventory_C` + `SN2InventoryViewModel` + wiring `SetViewModel`/
`AddToViewport` by hand is fragile and mostly unnecessary. The game already
has a function that does all of this correctly:
`UUWEInventoryInteractionComponent::InteractWithInventoryInteractionComponent
(AController*, APawn*, FHitResult)` -- the exact function that fires when a
player walks up to any storage and presses interact. Calling it directly
(with a real player Controller/Pawn and a zeroed `FHitResult`) opens the real
native screen with the game's own tested construction/binding code, no
crash. **Before hand-building any other UI interaction in this project,
check whether there's an equivalent real trigger function first** -- the
SDK's `*_classes.hpp` files are searchable by keyword (`Interact`, `Open`,
`Activate`, etc.) and are far more likely to reveal one than assuming none
exists.

Closing screens: Escape does not close a screen opened via the above (likely
because the normal walk-up-and-interact flow sets some "currently
interacting" state on the player that Escape depends on to know what to
cancel, which the direct call bypasses). `UCommonActivatableWidget::
DeactivateWidget()` looked like the real generic close but was **CONFIRMED
IN-GAME (2026-07-26) to NOT close the screen** even when called on every
live `CommonActivatableWidget`. The game's actual screen lifecycle owner is
**`UWindowManager`** (`UWECommonUI_classes.hpp`), a `UGameInstanceSubsystem`
holding one `UUWEWidgetLayer` stack per `EUWEWindowManagerLayer` (Bottom=0,
HUD=1, AboveHUD=2, Modal=3, AboveModal=4, PauseScreen=5, AbovePauseScreen=6,
Debug=7); its `OnWidgetPushed`/`OnWidgetPopped` delegates show every screen
enters and leaves through it. Close via `GetActiveWidget(LayerId)` +
`Pop(Widget)` (param names confirmed in `UWECommonUI_parameters.hpp`).
**CONFIRMED WORKING IN-GAME** (2026-07-28 log: repeated open/close cycles).
Never pop layers Bottom/HUD (the persistent HUD lives there).

`GetActiveWidget(Modal)` is also the only trustworthy answer to "is my screen
still up". A mod-side boolean is a belief, not a fact: the player can close or
replace the screen by routes the mod never sees, after which acting on that
boolean pops somebody else's modal. Compare the returned widget by **address
only** -- never dereference it, so a stale pointer stays harmless.

Judge "did my interact actually open a screen" the same way: capture
`GetActiveWidget(Modal)` before the call and require a *different*, non-null
widget after. `ProcessEvent` returning tells you nothing, and interact on a
locker while a storage screen is already up is a confirmed silent no-op.

## Event hooking: what's actually safe

| Approach | Status |
|---|---|
| `RC::Unreal::Hook::RegisterProcessEventPostCallback` | **Confirmed safe and working.** Hooks UE4SS's own already-installed global `ProcessEvent` dispatcher; your callback filters by `UFunction*` identity. Never touches the target function's own dispatch. Modeled on the working `cppmods/EventViewerMod/src/Middleware.cpp` example already in the UE4SS source tree. |
| `UFunction::RegisterPostHook` | **CONFIRMED UNSAFE.** Crashed the game hard (silent UE4SS.log, no further output) the first time the hooked function fired for real. This is a per-function pointer-swap trampoline, unlike the global-dispatcher approach above. |

## Finding the ONE live widget when several match

UMG widgets are not collected promptly. A `WBP_Inventory_C` from a screen the
player closed minutes ago is still returned by `FindAllOf`, and still reports
the same bound `InventoryId` as the live one -- so "the grid showing inventory
87" matched four different objects across four opens (2026-07-28 log:
`title written to 1 grid(s)` climbing to `4`), and text was written to all of
them.

A widget's outer chain runs `widget -> WidgetTree -> owning screen`, so the
live one is reachable with a single real compiled call:

```cpp
widget->GetTypedOuter(screen->GetClassPrivate()) == screen
```

where `screen` is `WindowManager::GetActiveWidget(Modal)`. Prefer that match;
fall back to the **last** `FindAllOf` result (later in the global object array
means more recently created) rather than writing to every match.

## `FText::ToString()` can "succeed" and give you a placeholder

An `FText` backed by a **string table** does not reliably resolve through
`FText::ToString()` from a mod's context. The engine hands back the literal
string `<MISSING STRING TABLE ENTRY>`, and the cast succeeded, so nothing
about the call looks wrong. **CONFIRMED IN-GAME 2026-07-29** by screenshot:
locker names and several item names were all displayed to the player as that
placeholder.

Treat a text read as *possibly* unusable even when it returns a value. Reject
empty strings and anything wrapped in angle brackets (every engine placeholder
of this family looks like `<SOMETHING>`), then fall back to a name you can
derive yourself — the asset's `GetName()`, or a tidied owning-actor name.

## Writing text into a game screen's widgets: stay in the title

The mod may write into `WBP_Inventory_C`'s title (`Name_0`) safely — it is a
short, centred slot. **Do not write a multi-line block into `DescriptionText`.**
It is laid out for a one-line item blurb; a list rendered straight over the tab
bar and the inventory header (confirmed by screenshot, 2026-07-29), and forcing
its visibility does not give it room. A borrowed widget only has the space its
own design assumed. If a feature needs more room than one short line, it needs
a surface of its own, not a bigger string.

## Reading player-facing text and world positions

- **`FText` fields** (`UUWEItemType::Name`, `UUWEInventoryComponent::InventoryName`)
  read cleanly through `CastField<FTextProperty>` + `FText::ToString()`. A
  `nullopt` from a failed cast is useful signal: Dumper-7 renames members that
  collide with `UObject`'s own (`Name` -> `Name_0`), so try both spellings and
  let the cast decide which one is the real text field.
- **`AActor::K2_GetActorLocation()`** is a real compiled UE4SS member -- no
  `ProcessEvent`, no params buffer. Calling it on a component's
  `GetTypedOuter<AActor>()` and on the player's pawn is the whole of "how far
  away is that locker". There is no need for the game's own
  `GetStorageContainerForInventory` reflected struct return.

## Diagnosing "which component is this on": use `AActor`, not string-matching

`AActor::GetTypedOuter<AActor>()` (called on a component) returns the real
owning actor. `owner->GetComponentsByClass(SomeClass)` returns every
component of that class on the actor, both as genuine compiled calls with no
manual `ProcessEvent`/params-buffer work at all. This project initially
tried correlating components by comparing string prefixes of
`GetFullName()` -- unnecessary and more fragile than just using the real
API.

## When you can't find a native API for something

1. Search the relevant `vendor/SN2SDK/SDK/*_classes.hpp` file(s) for a
   function whose *name* suggests it already does what you want (the game's
   own code almost always already does it -- see the interaction-hijacking
   lesson above).
2. If nothing exists, fall back to `ReflectionUtils`/`PropertyReflection`
   (this project's generic reflection helpers), but keep the call as close
   as possible to a proven-safe shape: read-only property reads and
   `const`-marked function calls have never caused a crash in this project;
   object construction and complex multi-step engine setup calls have,
   repeatedly.
3. Update this file and `feedback_ue4ss_reflection_bugs` (memory) with
   whatever you learn, so the next session doesn't re-discover it the hard
   way.
