#include "ReflectionUtils.hpp"

#include <vector>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC::Unreal;

namespace StorageTerminal {

// Standard UE package-path prefixes for transient, non-gameplay objects --
// e.g. the "/Temp/Untitled_0" world spawned for a building-placement preview.
// FindFirstOf just returns whichever instance of a class happens to be first
// in the global object array, with zero guarantee that's the real persistent
// gameplay world's instance rather than a short-lived preview/transient one.
// Confirmed the hard way: a placement-preview world's UWEInventorySubsystem
// got picked up as "the" subsystem, producing an always-empty snapshot and
// very likely the crash when that transient world got torn down mid-call.
bool ReflectionUtils::is_transient(UObject* object)
{
    if (!object) {
        return true;
    }

    // ClientLobby (the main menu world) isn't a transient/preview package in
    // the engine sense, but it's a real, confirmed source of the exact same
    // class of bug: during the menu-to-gameplay transition, its objects can
    // briefly coexist with the real persistent world's before being garbage
    // collected. A one-shot lookup that races that window can permanently
    // latch onto the doomed ClientLobby instance instead of retrying until
    // the real world's instance exists -- confirmed in-game: the runtime
    // probe grabbed a UWECommunalInventorySubsystem living in
    // "/Game/Maps/L_ClientLobby", after which the snapshot builder silently
    // operated on dead objects for the rest of the session. Folding the
    // exclusion into this shared primitive means every find_first/find_all
    // call in the mod gets the same protection instead of needing its own
    // copy of the check.
    // All three markers below are PACKAGE-level path components, and a UE
    // package's own FName is its full path ("/Engine/Transient",
    // "/Temp/Untitled_0", "/Game/Maps/L_ClientLobby"). So the outermost
    // package's name carries everything this test needs.
    //
    // This used to call GetFullName(), which is GetClassPrivate()->GetName()
    // (one allocation) plus a RECURSIVE GetPathName() walk up the entire outer
    // chain with string appends (UObject.cpp:573) -- paid for every candidate
    // of every find_first/find_all, which is the hottest per-object path in
    // the mod. Reading just the outermost package's name is one allocation and
    // no recursion.
    //
    // The only behavioural difference: the old version also matched these
    // markers if they appeared in the object's CLASS name or in a sub-object
    // name further down the path. Since all three are package paths, that
    // could only ever have been a false positive (wrongly excluding a real
    // object), so narrowing to the package is strictly more precise.
    auto* outermost = object->GetOutermost();
    if (!outermost) {
        // Should be unreachable -- the walk terminates at the object itself
        // when it has no outer -- but fall back rather than guess.
        const auto full_name = object->GetFullName();
        return full_name.find(L"/Temp/") != std::wstring::npos
            || full_name.find(L"/Engine/Transient") != std::wstring::npos
            || full_name.find(L"ClientLobby") != std::wstring::npos;
    }

    const auto package_name = outermost->GetName();
    return package_name.find(L"/Temp/") != std::wstring::npos
        || package_name.find(L"/Engine/Transient") != std::wstring::npos
        || package_name.find(L"ClientLobby") != std::wstring::npos;
}

bool ReflectionUtils::is_dead(UObject* object)
{
    if (!object) {
        return true;
    }

    // RF_BeginDestroyed is set as soon as the engine starts tearing an object
    // down -- before its subobjects and properties are safe to walk -- so this
    // catches exactly the window in which a popped screen's widgets are still
    // enumerable but no longer safe to read.
    if (object->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed))) {
        return true;
    }

    // PendingKill: destroyed for gameplay purposes but not yet collected.
    // Unreachable: GC has already decided nothing references it.
    // PendingConstruction: memory exists but the class constructor has not run,
    // so its properties are not initialised yet.
    return object->HasAnyInternalFlags(EInternalObjectFlags::Unreachable
                                       | EInternalObjectFlags::PendingKill
                                       | EInternalObjectFlags::PendingConstruction);
}

UObject* ReflectionUtils::find_first(std::wstring_view class_name)
{
    std::vector<UObject*> candidates;
    UObjectGlobals::FindAllOf(class_name.data(), candidates);

    for (auto* candidate : candidates) {
        if (!is_transient(candidate)) {
            return candidate;
        }
    }

    return nullptr;
}

std::vector<UObject*> ReflectionUtils::find_all_unfiltered(std::wstring_view class_name)
{
    std::vector<UObject*> candidates;
    UObjectGlobals::FindAllOf(class_name.data(), candidates);
    return candidates;
}

std::vector<UObject*> ReflectionUtils::find_all(std::wstring_view class_name)
{
    std::vector<UObject*> candidates;
    UObjectGlobals::FindAllOf(class_name.data(), candidates);

    std::vector<UObject*> result;
    result.reserve(candidates.size());
    for (auto* candidate : candidates) {
        if (!is_transient(candidate)) {
            result.push_back(candidate);
        }
    }

    return result;
}

UClass* ReflectionUtils::find_class_by_name(std::wstring_view class_name)
{
    auto* found = UObjectGlobals::FindObject(FName(std::wstring(L"Class")), FName(std::wstring(class_name)));
    return Cast<UClass>(found);
}

UObject* ReflectionUtils::construct_object(std::wstring_view class_name, UObject* outer)
{
    auto* target_class = find_class_by_name(class_name);
    if (!target_class || !outer) {
        return nullptr;
    }

    return UObjectGlobals::NewObject<UObject>(outer, target_class);
}

UFunction* ReflectionUtils::find_function(UObject* object, std::wstring_view function_name)
{
    if (!object) {
        return nullptr;
    }

    // Resolve the name against the EXISTING name pool. The const-TCHAR*
    // overload of GetFunctionByNameInChain builds its FName with the default
    // FNAME_Add, so every lookup -- including the speculative ones that are
    // meant to miss -- interned a new entry into the engine's global name pool.
    // FNAME_Find only consults it. A name that was never interned cannot name
    // a real function, so NAME_None is simply "not found".
    const FName name(function_name, FNAME_Find);
    if (name.IsNone()) {
        return nullptr;
    }

    // GetFunctionByNameInChain looks up object->GetClassPrivate() internally;
    // it must be called on the instance itself, not on the UClass -- calling
    // it on the class would resolve the *metaclass's* functions instead
    // (always empty), which is why every single lookup was previously
    // failing regardless of world state or target object.
    //
    // NOTE: unlike UStruct::FindProperty, this is NOT memoized by UE4SS -- it
    // walks the function chain on every call. Callers on a hot path should
    // cache the returned UFunction*, which is class-level and so stays valid
    // for the life of the process. See main.cpp's cached screen handles.
    return object->GetFunctionByNameInChain(name);
}

std::wstring ReflectionUtils::safe_name(UObject* object)
{
    if (!object) {
        return L"<null>";
    }

    return object->GetName();
}

std::wstring ReflectionUtils::safe_full_name(UObject* object)
{
    if (!object) {
        return L"<null>";
    }

    return object->GetFullName();
}

}
