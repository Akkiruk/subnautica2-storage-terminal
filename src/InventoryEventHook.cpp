#include "InventoryEventHook.hpp"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>

#include "HookTargets.hpp"
#include "ReflectionUtils.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace StorageTerminal {

bool InventoryEventHook::try_install(const std::function<void()>& on_changed)
{
    if (m_installed) {
        return true;
    }

    auto* component = ReflectionUtils::find_first(StorageTerminalTargets::kInventoryComponentClass);
    if (!component) {
        return false;
    }

    // OnInventoryUpdated alone only fires on registration (placing/
    // reconstructing a storage), not on ordinary item deposit/withdraw --
    // confirmed in-game. Watch all three so real everyday interactions
    // actually trigger a refresh.
    auto* updated_function = ReflectionUtils::find_function(component, StorageTerminalTargets::kOnInventoryUpdated);
    auto* added_function = ReflectionUtils::find_function(component, StorageTerminalTargets::kOnItemAddedToInventory);
    auto* removed_function = ReflectionUtils::find_function(component, StorageTerminalTargets::kOnItemRemovedFromInventory);

    if (!updated_function && !added_function && !removed_function) {
        return false;
    }

    // {bOnce, bReadonly, OwnerModName, HookName} -- matches the construction
    // pattern used by the working EventViewerMod example. bReadonly=true
    // since this only observes calls, never alters them.
    const Hook::FCallbackOptions options{false, true, STR("StorageTerminal"), STR("InventoryChangeWatch")};

    Hook::RegisterProcessEventPostCallback(
        [updated_function, added_function, removed_function, on_changed](auto&, UObject*, UFunction* function, void*) {
            if (function == updated_function || function == added_function || function == removed_function) {
                on_changed();
            }
        },
        options);

    m_installed = true;
    return true;
}

}
