#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>

namespace RC::Unreal {
class UClass;
}

namespace StorageTerminal {

// Object-lookup and reflection primitives shared by the rest of the mod.
//
// Everything that constructed objects (construct_object_of_class,
// construct_object_like, find_class_default_object) was removed on
// 2026-07-28. The mod does not build UI or data objects any more -- it opens
// the game's own screens -- and find_class_default_object in particular
// wrapped UClass::CreateDefaultObject(), which is CONFIRMED to crash the game
// hard with no dump. Leaving a convenient wrapper around a known-fatal call
// in a shared utility header was an accident waiting to happen. See
// docs/UE4SS_API_REFERENCE.md before adding anything back.
class ReflectionUtils {
public:
    // First non-transient instance of a class.
    static RC::Unreal::UObject* find_first(std::wstring_view class_name);

    // All non-transient instances of a class.
    static std::vector<RC::Unreal::UObject*> find_all(std::wstring_view class_name);

    // Same as find_all but WITHOUT the transient-package filter. Required
    // for UMG widgets and game-instance subsystems, which legitimately live
    // under /Engine/Transient -- the filter (built for /Temp/ preview
    // worlds) silently hid them, which is why the WindowManager lookup and
    // the screen-title write both no-opped (2026-07-27).
    static std::vector<RC::Unreal::UObject*> find_all_unfiltered(std::wstring_view class_name);

    // Resolves a UClass* by name through the lower-level FindObject overload,
    // which (unlike FindAllOf) does not exclude UClass objects. Needed for
    // AActor::GetComponentsByClass, which wants the class itself. Stops at
    // the UClass -- it never calls CreateDefaultObject.
    static RC::Unreal::UClass* find_class_by_name(std::wstring_view class_name);

    // Constructs an instance of `class_name` via UObjectGlobals::NewObject.
    //
    // NewObject is the established-safe construction path (it creates the
    // class's CDO on demand through the engine's own mechanism if needed).
    // Do NOT reintroduce a helper that calls UClass::CreateDefaultObject()
    // yourself -- that is confirmed to crash the game hard with no dump, twice.
    // Used for the NoA dialogue-option data object.
    static RC::Unreal::UObject* construct_object(std::wstring_view class_name, RC::Unreal::UObject* outer);

    // True for objects in a transient/preview package (e.g. the
    // "/Temp/Untitled_0" world spawned during building placement) or the
    // ClientLobby menu world. Exposed so callers doing their own enumeration
    // can apply the same filter find_first/find_all use internally.
    static bool is_transient(RC::Unreal::UObject* object);

    // True if `object` is null, being torn down, unreachable, pending kill, or
    // not yet constructed -- i.e. NOT safe to reflect into.
    //
    // Why this exists: this mod pops and opens native screens, and every
    // enumeration (FindAllOf) hands back widgets and components belonging to
    // screens that were popped moments ago and are mid-teardown. Reading a
    // property chain off one of those is the most likely cause of the
    // browse crash (2026-07-30). Checking flags first is cheap and catches the
    // whole teardown window.
    //
    // HONEST LIMIT: this cannot make a genuinely dangling pointer safe. Once
    // an object's memory has been freed and reused, its flags describe whatever
    // now occupies that memory. It closes the realistic window (objects marked
    // for destruction but not yet collected), which is the window this mod
    // actually races -- it is not a substitute for not holding stale pointers.
    static bool is_dead(RC::Unreal::UObject* object);

    static RC::Unreal::UFunction* find_function(RC::Unreal::UObject* object, std::wstring_view function_name);

    static std::wstring safe_name(RC::Unreal::UObject* object);
    static std::wstring safe_full_name(RC::Unreal::UObject* object);
};

}
