# Reverse Engineering Notes

The game's own surface, as dumped by Dumper-7 into `vendor/SN2SDK`. This is
**documentation, not a compile target** — every call the mod makes is resolved
by reflection at runtime. For UE4SS's own API (and which of its calls are
confirmed fatal), see `UE4SS_API_REFERENCE.md`.

## Loader Status

- The live mod root is `Subnautica2/Binaries/Win64/Mods` — the loader reads its
  `mods.txt` from there (confirmed in `UE4SS.log`: *"Starting mods (from
  mods.txt (...\Binaries\Win64\Mods\mods.txt) load order)"*). The nested
  `Binaries/Win64/ue4ss/Mods` directory is a leftover and holds only a stale
  `mods.txt`; nothing loads from it.
- The root-level `UE4SS.dll` and `UE4SS-settings.ini` under the game's own root
  were disabled because they forced a broken loader path that failed to resolve
  `GUObjectArray`.
- The runtime successfully resolves:
  - `EngineVersion: 5.6`
  - `GUObjectArray`
  - `GMalloc`
  - `StaticConstructObject_Internal`

## Confirmed Inventory Surface

From the public Dumper-7 SDK dump used by community mods:

- `UUWECommunalInventorySubsystem`
  - field: `CommunalInventories`
  - methods: `RegisterInventory`, `UnregisterInventory`
- `UUWEInventoryComponent` — **the mod's main source**
  - fields: `InventoryId`, `bIsCommunal`, `MaxItems`, `Columns`,
    `InventoryName` (**FText — the locker's player-editable label; this is how
    a search result names the container it found something in**),
    `InventoryDescription`
  - methods: `GetItems` (const, what the snapshot calls), `GetItemIds`,
    `GetItemCountByType`, `RemoveItem`, `MoveInventoryItem`,
    `MoveAllInventoryItems`, `ServerDropItem`
  - handlers watched for change notifications: `OnInventoryUpdated`,
    `OnItemAddedToInventory`, `OnItemRemovedFromInventory` — note these are
    per-component handlers of a **subsystem-wide broadcast**, so one real
    change fires them on *every* live component (~25 observed). Only usable as
    a "something moved" bit unless deduplicated by context.
- `UUWEItemType` (a `UUWEActorDataAsset`)
  - `Name` — **FText, the localized display name.** Dumper-7 emits it as
    `Name_0` because it collides with `UObject::Name` in generated C++; the
    runtime reflected name is usually plain `Name`. This is what the player
    searches; the asset name is only a fallback.
  - also: `ItemDescription`, `Thumbnail` (`TSoftObjectPtr` — **never `memcpy` a
    struct containing this**), `TypeTag`, `GameplayTags` (a possible basis for
    category filters later)
- `FUWEInventoryItem`: `InventoryId`, `ItemId` (a bare 16-byte
  `FUWEInventoryItemId`/`FGuid`), `ItemType`, `Count`, `Attributes`. **`Count`
  is always 1** — the game does not stack.
- `USN2InventoryScreenViewModel` / `USN2InventoryViewModel` — the MVVM layer
  behind `WBP_Inventory_C`. Authoritative: it rebuilds from its bound
  inventory, which is why injected entries never survived.
- `UUWEInventorySubsystem`
  - fields: `StorageActors`, `OnInventoryUpdated`
  - methods: `GetStorageActors`, `GetStorageContainerForInventory`, `FindStorageActorForInventory`, `GetItemsForInventory`, `IsInventoryCommunal`, `MoveInventoryItem`, `ServerAddItemTypeToInventory`
- `AUWEInventoryStorage`
  - fields: `StorageContainers`, `ItemsContainer`

The dumped `FUWEInventoryStorageContainer` struct includes:

- `InventoryId`
- `InventoryLocation`
- `InventoryClass`
- `Actor`
- `MaxItems`
- `bIsCommunal`

## Opening and closing screens

- `UUWEInventoryInteractionComponent::InteractWithInventoryInteractionComponent(AController*, APawn*, const FHitResult&)`
  — the exact function that runs when a player walks up and presses interact.
  Calling it is the mod's only non-read action. Its sibling field
  `InventoryInteractionEnabled` (replicated) is the game's own gate on whether
  the container may be opened at all; honour it.
- `UWindowManager` (a `UGameInstanceSubsystem` in `UWECommonUI_classes.hpp`)
  owns every screen, one `UUWEWidgetLayer` stack per `EUWEWindowManagerLayer`
  (Bottom=0, HUD=1, AboveHUD=2, **Modal=3**, AboveModal=4, PauseScreen=5,
  AbovePauseScreen=6, Debug=7). `GetActiveWidget(LayerId)` + `Pop(Widget)`.
  Only ever touch Modal.
- `WBP_Inventory_C` has both a title (`Name_0`) and a `DescriptionText`
  (`UCommonTextBlock`, with a `DescriptionRoot` container and an
  `OnDescriptionSet` event) — the description block is what makes a multi-line
  result list possible instead of one crowded title string.

## Crafting Surface — surveyed, deliberately unused

Recorded because it keeps getting rediscovered. Crafting is **out of scope**
(see `../../NORTH_STAR.md`); nothing in the mod calls any of this.

- `UUWECraftingComponent`
  - field: `RegisteredSourceIds` — the proximity-registered communal
    inventories the game itself considers "reachable from here". This is the
    most promising basis for a real definition of *your* storage, which the
    mod currently lacks.
  - methods: `HasAnyCommunalInventoriesRegistered`, `GetAllNearbyItemsOfItemType`, `GetRequirementsString`, `ServerCraftItemFromRecipe`
- `UUWECrafterComponent`
  - fields: `LocalOutputInventory`, `MaxQueueSize`, `bCraftShouldGoIntoInventory`, `ActiveCrafts`
  - methods: `CanCraftItemFromRecipe`, `TryAddRecipeToLocalQueue`, `StartCrafting`, `NotifyCraftingStarted`, `NotifyCraftingCompleted`

`ServerCraftItemFromRecipe(Recipe, Crafter, OutputInventory, bForceImmediate)`
is server-authoritative.

## Registration Pattern — and why the mod ignores it

The dumped lifepod locker blueprint exposes `RegisterCommunalInventories` /
`UnregisterCommunalInventories`, so vanilla storage actors do explicitly
register with `UUWECommunalInventorySubsystem` rather than relying on a passive
world scan.

**The mod does not trust that registration anyway.** Three discovery designs
built on subsystem bookkeeping all failed the same way: `CommunalInventories`
missed a freshly-placed storage in testing, and
`HighestInventoryId`/`IsInventoryValid` stayed stale indefinitely for
pre-existing ones. `SnapshotBuilder` scans live `UWEInventoryComponent`
instances directly instead.

## Confirmed Recipe Registry Path

Community C++ mods already detour `SN2AssetRegistry::GetAllCraftingRecipes`, which confirms that recipe enumeration is asset-registry driven and is a viable extension point for recipe injection or inspection.