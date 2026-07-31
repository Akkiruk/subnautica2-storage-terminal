#include "SnapshotBuilder.hpp"

#include <algorithm>
#include <vector>

#include "HookTargets.hpp"
#include "PropertyReflection.hpp"
#include "ReflectionUtils.hpp"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Property/FArrayProperty.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace StorageTerminal {

namespace {

FProperty* find_first_array_property(UFunction* function)
{
    if (!function) {
        return nullptr;
    }

    // TFieldRange with None matches what the deprecated ForEachProperty() did
    // (UStruct.cpp: TFieldRange<FProperty>(this, EFieldIterationFlags::None)) --
    // this function's OWN params only, which is exactly right for a UFunction's
    // parameter list. Deliberately not IncludeSuper here.
    for (FProperty* property : TFieldRange<FProperty>(function, EFieldIterationFlags::None)) {
        if (CastField<FArrayProperty>(property)) {
            return property;
        }
    }

    return nullptr;
}

// An FText that is a string-table reference does not always resolve through
// FText::ToString() from a mod's context -- the engine hands back the literal
// placeholder "<MISSING STRING TABLE ENTRY>" instead of the localized text.
// Confirmed in-game 2026-07-29: locker names and several item names all came
// back as that placeholder and were displayed verbatim to the player.
//
// So a read that "succeeded" is not necessarily usable. Anything empty, or
// wrapped in angle brackets (every engine placeholder of this family looks
// like <SOMETHING>), is rejected so the caller falls back to a real name.
bool is_usable_display_text(const std::wstring& text)
{
    if (text.empty()) {
        return false;
    }
    if (text.front() == L'<') {
        return false;
    }
    return text.find(L"MISSING STRING TABLE") == std::wstring::npos;
}

// Turns "BP_Locker_Wall_C /Game/Maps/..." into "Locker Wall". Only used when
// the locker has no InventoryName of its own -- an unnamed container should
// still read as a thing in the world, not as an asset path.
std::wstring tidy_actor_name(const std::wstring& full_name)
{
    const auto space = full_name.find(L' ');
    std::wstring name = (space == std::wstring::npos) ? full_name : full_name.substr(0, space);

    if (name.rfind(L"BP_", 0) == 0) {
        name = name.substr(3);
    }
    if (const auto suffix = name.rfind(L"_C"); suffix != std::wstring::npos && suffix == name.size() - 2) {
        name = name.substr(0, suffix);
    }
    std::replace(name.begin(), name.end(), L'_', L' ');
    return name.empty() ? std::wstring(L"Storage") : name;
}

// The pawn the local player controller is possessing.
//
// Resolved ONCE per rebuild and passed to both consumers below. Each of them
// used to do this itself, which meant two full GUObjectArray scans per rebuild
// (ReflectionUtils::find_first walks every live UObject) to answer the same
// question twice.
AActor* find_local_player_pawn()
{
    auto* player_controller = ReflectionUtils::find_first(StorageTerminalTargets::kPlayerControllerClass);
    if (!player_controller) {
        return nullptr;
    }

    auto* pawn_field = PropertyReflection::find_property(
        player_controller->GetClassPrivate(), StorageTerminalTargets::kFieldPawn);
    return pawn_field
        ? Cast<AActor>(PropertyReflection::read_object(pawn_field, reinterpret_cast<const uint8_t*>(player_controller)))
        : nullptr;
}

// The player's carried inventory is the non-communal UWEInventoryComponent on
// the pawn the local player controller is possessing.
UObject* find_player_inventory_component(AActor* pawn, FProperty* is_communal_field)
{
    if (!pawn) {
        return nullptr;
    }

    auto* inventory_class = ReflectionUtils::find_class_by_name(StorageTerminalTargets::kInventoryComponentClass);
    if (!inventory_class) {
        return nullptr;
    }

    auto components = pawn->GetComponentsByClass(inventory_class);
    for (int32_t index = 0; index < components.Num(); ++index) {
        auto* component = components[index];
        if (!component) {
            continue;
        }
        // The pawn also owns communal-flagged components in some vehicles;
        // the carried inventory is the non-communal one.
        const bool communal = PropertyReflection::read_bool(
            is_communal_field, reinterpret_cast<const uint8_t*>(component)).value_or(false);
        if (!communal) {
            return component;
        }
    }

    return nullptr;
}

// The communal inventories the GAME considers reachable from where the player
// is standing -- UWECraftingComponent::RegisteredSourceIds on the player
// character, the same set the fabricator draws from. Using it means the
// terminal indexes exactly the storage the base's own crafting does, with no
// radius for the mod to invent and get wrong.
//
// Returns empty if the component or field cannot be read, or if the game has
// not registered anything; the caller then falls back to every communal
// inventory rather than showing the player nothing.
std::vector<int32_t> read_reachable_inventory_ids(AActor* pawn)
{
    if (!pawn) {
        return {};
    }

    auto* crafting_class = ReflectionUtils::find_class_by_name(StorageTerminalTargets::kCraftingComponentClass);
    if (!crafting_class) {
        return {};
    }

    auto components = pawn->GetComponentsByClass(crafting_class);
    for (int32_t index = 0; index < components.Num(); ++index) {
        auto* crafting = components[index];
        if (!crafting) {
            continue;
        }
        auto* field = PropertyReflection::find_property(
            crafting->GetClassPrivate(), StorageTerminalTargets::kFieldRegisteredSourceIds);
        if (!field) {
            continue;
        }
        auto ids = PropertyReflection::read_int_array(field, reinterpret_cast<uint8_t*>(crafting));
        if (!ids.empty()) {
            return ids;
        }
    }

    return {};
}

}

bool SnapshotBuilder::ensure_handles_resolved()
{
    if (m_handles.valid) {
        return true;
    }

    auto* any_component = ReflectionUtils::find_first(StorageTerminalTargets::kInventoryComponentClass);
    if (!any_component) {
        return false;
    }

    ResolvedHandles handles{};
    handles.get_items_function = ReflectionUtils::find_function(any_component, StorageTerminalTargets::kGetItems);
    if (handles.get_items_function) {
        handles.get_items_array_return = find_first_array_property(handles.get_items_function);
        if (handles.get_items_array_return) {
            auto* as_array = CastField<FArrayProperty>(handles.get_items_array_return);
            auto* inner = as_array ? as_array->GetInner() : nullptr;

            handles.item_type_field = PropertyReflection::find_struct_field(inner, StorageTerminalTargets::kFieldItemType);
            handles.item_count_field = PropertyReflection::find_struct_field(inner, StorageTerminalTargets::kFieldCount);
        }
    }

    auto* component_class = any_component->GetClassPrivate();
    handles.inventory_id_field = PropertyReflection::find_property(component_class, StorageTerminalTargets::kFieldInventoryId);
    handles.is_communal_field = PropertyReflection::find_property(component_class, StorageTerminalTargets::kFieldIsCommunal);
    handles.inventory_name_field = PropertyReflection::find_property(component_class, StorageTerminalTargets::kFieldInventoryName);

    handles.valid = handles.get_items_function != nullptr
        && handles.get_items_array_return != nullptr
        && handles.is_communal_field != nullptr;
    m_handles = handles;

    if (handles.valid && !handles.inventory_name_field) {
        Output::send<LogLevel::Verbose>(
            STR("[StorageTerminal] InventoryName not found on UWEInventoryComponent; lockers will show actor names.\n"));
    }

    return handles.valid;
}

const SnapshotBuilder::ItemTypeNames* SnapshotBuilder::resolve_item_type_names(UObject* item_type)
{
    if (!item_type) {
        return nullptr;
    }

    // Cache hit with a real localized name: nothing to do, and no allocation.
    // A hit that is still on the asset-name fallback falls through and retries
    // the FText -- see ItemTypeNames::display_name_resolved.
    const auto cached = m_itemTypeNames.find(item_type);
    if (cached != m_itemTypeNames.end() && cached->second.display_name_resolved) {
        return &cached->second;
    }

    // Not latched on failure. The field is resolved from whatever item type
    // happens to be seen first, and early in world load that read can fail --
    // latching there meant the mod searched asset names for the whole session.
    // find_property is memoized by UE4SS, so retrying costs a hash probe.
    if (!m_itemDisplayNameField) {
        for (const auto candidate : StorageTerminalTargets::kItemDisplayNameCandidates) {
            auto* field = PropertyReflection::find_property(item_type->GetClassPrivate(), candidate);
            if (field && PropertyReflection::read_text(field, reinterpret_cast<const uint8_t*>(item_type))) {
                m_itemDisplayNameField = field;
                break;
            }
        }
        if (!m_itemDisplayNameField && !m_itemDisplayNameFieldResolved) {
            m_itemDisplayNameFieldResolved = true; // log once, keep retrying
            Output::send<LogLevel::Verbose>(
                STR("[StorageTerminal] UWEItemType display name not readable yet; using asset names until it is.\n"));
        }
    }

    std::wstring display_name;
    bool resolved = false;
    if (m_itemDisplayNameField) {
        // The field is resolved from one item type, but every UUWEItemType
        // shares the same class, so the same FProperty is valid for all of them.
        auto text = PropertyReflection::read_text(
            m_itemDisplayNameField, reinterpret_cast<const uint8_t*>(item_type)).value_or(std::wstring{});
        if (is_usable_display_text(text)) {
            display_name = std::move(text);
            resolved = true;
        }
    }

    if (cached != m_itemTypeNames.end()) {
        // Upgrade the existing fallback entry in place, if we can now do better.
        if (resolved) {
            cached->second.display_name = std::move(display_name);
            cached->second.display_name_resolved = true;
        }
        return &cached->second;
    }

    ItemTypeNames names{};
    names.asset_name = ReflectionUtils::safe_name(item_type);
    // An item whose FText is missing or is a string-table placeholder falls back
    // to the asset name, so it stays searchable and readable meanwhile.
    names.display_name = resolved ? std::move(display_name) : names.asset_name;
    names.display_name_resolved = resolved;

    return &m_itemTypeNames.emplace(item_type, std::move(names)).first->second;
}

void SnapshotBuilder::read_items_into(UObject* component, InventorySourceSnapshot& source)
{
    // Reused buffer rather than a fresh allocation per component. The size is
    // fixed by the UFunction, so it only ever grows once.
    const auto params_size = static_cast<size_t>(m_handles.get_items_function->GetPropertiesSize());
    m_itemParams.assign(params_size, 0);
    component->ProcessEvent(m_handles.get_items_function, m_itemParams.empty() ? nullptr : m_itemParams.data());

    const auto array_view = PropertyReflection::read_array(m_handles.get_items_array_return, m_itemParams.data());
    if (!array_view) {
        return;
    }

    // Fold index for THIS inventory only. Clearing keeps the buckets, so the
    // map allocates once for the whole session rather than once per locker.
    m_foldIndex.clear();

    for (int32_t item_index = 0; item_index < array_view->count; ++item_index) {
        uint8_t* item_element = array_view->element_at(item_index);

        UObject* item_type_object = m_handles.item_type_field
            ? PropertyReflection::read_object(m_handles.item_type_field, item_element)
            : nullptr;
        if (!item_type_object) {
            continue;
        }

        const int32_t count = m_handles.item_count_field
            ? PropertyReflection::read_int(m_handles.item_count_field, item_element).value_or(0)
            : 0;
        if (count <= 0) {
            continue;
        }

        // Memoized per item TYPE, so the twelve one-count titanium entries in
        // a locker resolve their names once, not twelve times.
        const auto* names = resolve_item_type_names(item_type_object);
        if (!names) {
            continue;
        }

        // Fold stacks of the same type together here rather than in the
        // search: the game has no stacking, so a locker with twelve titanium
        // reports twelve separate one-count entries.
        //
        // The key is a view into the cache's own asset_name string, which is
        // stable for the life of the process, so it stays valid for as long as
        // this index does.
        const std::wstring_view key{names->asset_name};
        const auto existing = m_foldIndex.find(key);
        if (existing != m_foldIndex.end()) {
            source.items[existing->second].count += count;
        } else {
            m_foldIndex.emplace(key, source.items.size());
            ItemCount entry{};
            entry.display_name = names->display_name;
            entry.asset_name = names->asset_name;
            // Folded once here rather than on every keystroke in the search.
            entry.display_lower = to_search_key(entry.display_name);
            entry.asset_lower = to_search_key(entry.asset_name);
            entry.count = count;
            source.items.push_back(std::move(entry));
        }
    }
}

SnapshotBuildResult SnapshotBuilder::rebuild(InventorySnapshot& out_snapshot)
{
    SnapshotBuildResult result{};

    if (!ensure_handles_resolved()) {
        return result;
    }

    result.get_items_function_found = true;

    // Age every known source rather than clearing, so a locker that misses a
    // single scan (which happens while native screens are being pushed and
    // popped) does not blink out of the player's search results mid-type.
    out_snapshot.begin_scan();

    // Every live UWEInventoryComponent in the world, not just the ones a
    // subsystem's own bookkeeping array happens to already know about --
    // see SnapshotBuilder.hpp for why that trust turned out to be misplaced
    // twice already.
    const auto components = ReflectionUtils::find_all(StorageTerminalTargets::kInventoryComponentClass);
    result.components_scanned = static_cast<int32_t>(components.size());

    // One pawn lookup for both consumers below -- each used to resolve the
    // player controller and pawn itself, costing a second full object-array
    // scan per rebuild to answer the identical question.
    AActor* const player_pawn = find_local_player_pawn();

    UObject* const player_component = find_player_inventory_component(player_pawn, m_handles.is_communal_field);
    result.player_inventory_found = player_component != nullptr;

    const auto reachable = read_reachable_inventory_ids(player_pawn);
    result.reachable_ids_found = static_cast<int32_t>(reachable.size());
    if (reachable.empty() && !m_loggedReachableFallback) {
        m_loggedReachableFallback = true;
        Output::send<LogLevel::Verbose>(
            STR("[StorageTerminal] No RegisteredSourceIds from the crafting component; indexing every communal inventory instead.\n"));
    }

    for (auto* component : components) {
        if (!component) {
            continue;
        }

        const bool is_communal = PropertyReflection::read_bool(
            m_handles.is_communal_field, reinterpret_cast<const uint8_t*>(component)).value_or(false);
        const bool is_player = component == player_component;
        if (!is_communal && !is_player) {
            continue;
        }

        const int32_t inventory_id = PropertyReflection::read_int(
            m_handles.inventory_id_field, reinterpret_cast<const uint8_t*>(component)).value_or(-1);

        // Scope to what the game itself says is reachable. Your own inventory
        // is always in scope; it is not "storage" the terminal reaches for.
        if (!is_player && !reachable.empty()
            && std::find(reachable.begin(), reachable.end(), inventory_id) == reachable.end()) {
            continue;
        }

        ++result.sources_discovered;

        InventorySourceSnapshot source{};
        source.inventory_id = inventory_id;
        source.is_player = is_player;
        source.component = component;

        auto* owner = component->GetTypedOuter<AActor>();

        if (is_player) {
            source.display_name = L"Carried";
        } else {
            if (m_handles.inventory_name_field) {
                auto named = PropertyReflection::read_text(
                    m_handles.inventory_name_field, reinterpret_cast<const uint8_t*>(component)).value_or(std::wstring{});
                if (is_usable_display_text(named)) {
                    source.display_name = std::move(named);
                }
            }
            if (source.display_name.empty() && owner) {
                source.display_name = tidy_actor_name(ReflectionUtils::safe_full_name(owner));
            }
            if (source.display_name.empty()) {
                source.display_name = L"Storage " + std::to_wstring(source.inventory_id);
            }
        }

        if (owner) {
            const auto location = owner->K2_GetActorLocation();
            source.x = location.X();
            source.y = location.Y();
            source.z = location.Z();
            source.has_location = true;
        }

        read_items_into(component, source);

        if (!source.items.empty()) {
            ++result.sources_with_items_read;
        }

        out_snapshot.upsert_source(std::move(source));
    }

    out_snapshot.drop_unseen_sources();

    return result;
}

}
