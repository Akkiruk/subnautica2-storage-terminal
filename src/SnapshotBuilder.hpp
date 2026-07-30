#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FProperty.hpp>

#include "SnapshotModel.hpp"

namespace StorageTerminal {

struct SnapshotBuildResult {
    bool get_items_function_found = false;
    int32_t components_scanned = 0;
    int32_t sources_discovered = 0;
    int32_t sources_with_items_read = 0;
    bool player_inventory_found = false;
    // How many inventories the game reported as reachable. 0 means the mod
    // fell back to indexing every communal inventory in the world.
    int32_t reachable_ids_found = 0;
};

// Assembles a real InventorySnapshot by scanning every LIVE
// UWEInventoryComponent instance directly (RC::Unreal::FindAllOf) and keeping
// the ones with bIsCommunal true, then calling GetItems() on each -- the same
// data UWECraftingComponent reads to find nearby communal materials. The
// player's own carried inventory is added separately (resolved through the
// player controller's pawn), so a search covers pockets as well as storage.
//
// Two earlier designs both trusted a subsystem's own bookkeeping/registration
// state instead of scanning live objects directly, and both turned out to have
// the same class of bug: UWEInventorySubsystem::HighestInventoryId /
// IsInventoryValid could stay stale indefinitely for pre-existing storages,
// and UWECommunalInventorySubsystem::CommunalInventories (a registration
// array) also missed a freshly-placed storage in testing. Scanning live
// objects directly sidesteps whatever registration-timing quirk either
// bookkeeping structure has, at the cost of scanning every inventory
// component in the world each rebuild (only bIsCommunal ones are kept, and
// rebuilds are gated behind the event hook in main.cpp, not run every tick).
class SnapshotBuilder {
public:
    SnapshotBuildResult rebuild(InventorySnapshot& out_snapshot);

private:
    struct ResolvedHandles {
        RC::Unreal::UFunction* get_items_function = nullptr;
        RC::Unreal::FProperty* get_items_array_return = nullptr;
        RC::Unreal::FProperty* item_type_field = nullptr;
        RC::Unreal::FProperty* item_count_field = nullptr;
        RC::Unreal::FProperty* inventory_id_field = nullptr;
        RC::Unreal::FProperty* is_communal_field = nullptr;
        RC::Unreal::FProperty* inventory_name_field = nullptr;
        bool valid = false;
    };

    ResolvedHandles m_handles;

    // UUWEItemType::Name is an FText holding the localized display name.
    // Dumper-7 calls it "Name_0" because it collides with UObject::Name, so
    // both spellings are tried and whichever resolves is cached here.
    RC::Unreal::FProperty* m_itemDisplayNameField = nullptr;
    bool m_itemDisplayNameFieldResolved = false;
    bool m_loggedReachableFallback = false;

    // The display and asset names for one UUWEItemType.
    struct ItemTypeNames {
        std::wstring display_name;
        std::wstring asset_name;
    };

    // Item names keyed by the UUWEItemType they came from.
    //
    // Why: the game has no stacking, so a locker holding twelve titanium
    // yields twelve separate one-count entries that all point at the SAME
    // UUWEItemType. Resolving names per ELEMENT meant a GetName() allocation
    // plus an FText::ToString() allocation for every stored item in the base
    // on every rebuild -- ~1000 allocations for a 500-item base holding ~40
    // distinct types. Keying on the type collapses that to one resolve per
    // type for the life of the process.
    //
    // The pointer is only ever a KEY -- never dereferenced on a cache hit --
    // so a stale entry is harmless, the same reasoning main.cpp uses for
    // m_ownedScreen. Bounded by the number of item types the game defines.
    //
    // std::unordered_map is node-based, so references into it stay valid
    // across later inserts; m_foldIndex below relies on that.
    std::unordered_map<RC::Unreal::UObject*, ItemTypeNames> m_itemTypeNames;

    // Scratch for folding stacks within ONE inventory: asset name -> index
    // into source.items. Cleared per component, kept as a member so its
    // buckets are reused instead of reallocated for every locker.
    //
    // Keyed by string view rather than by the item-type pointer so that the
    // folding rule is byte-for-byte the one it replaced (two distinct
    // UUWEItemType objects sharing an asset name still merge). The views point
    // into m_itemTypeNames, which owns the strings and keeps them stable.
    std::unordered_map<std::wstring_view, size_t> m_foldIndex;

    // Reused GetItems() params buffer. This was a fresh heap allocation per
    // component per rebuild; the size is fixed by the UFunction, so one buffer
    // serves every call.
    std::vector<uint8_t> m_itemParams;

    bool ensure_handles_resolved();
    void read_items_into(RC::Unreal::UObject* component, InventorySourceSnapshot& source);

    // Resolves (and memoizes) both names for an item type. Returns nullptr
    // only when `item_type` is null.
    const ItemTypeNames* resolve_item_type_names(RC::Unreal::UObject* item_type);
};

}
