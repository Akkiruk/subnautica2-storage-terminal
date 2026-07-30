#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UStruct.hpp>
#include <Unreal/FProperty.hpp>

namespace StorageTerminal {

// Every reflected call and field access in this mod is resolved by property
// name/type at runtime rather than against a fixed C++ struct layout, so the
// mod does not need hand-written param structs kept in sync with the game's
// binary. vendor/SN2SDK is used as documentation of what exists, not compiled
// against.
//
// This is deliberately small. A dozen unused writers accumulated here while
// the mirror-inventory and widget-building designs were alive (write_int,
// write_name, write_string, write_double, read_name, read_string,
// read_weak_object, write_guid_bytes, read_guid_bytes, find_int_properties,
// append_object_to_array, append_zeroed_to_array, and a generic `call`);
// all were removed on 2026-07-28 with the designs that used them. Keep it
// that way -- an unused array-mutation helper in a mod that must never mutate
// game state is a trap, not a convenience.
class PropertyReflection {
public:
    // Finds a property by name on any UStruct's property chain -- a
    // UFunction's params, or a UClass's member fields (pass
    // object->GetClassPrivate() to read a UObject's own UPROPERTY).
    static RC::Unreal::FProperty* find_property(RC::Unreal::UStruct* owner, std::wstring_view name);

    // Finds a field by name inside a struct-typed property's inner UStruct.
    static RC::Unreal::FProperty* find_struct_field(RC::Unreal::FProperty* struct_property, std::wstring_view field_name);

    static std::optional<int32_t> read_int(RC::Unreal::FProperty* property, const uint8_t* container_base);

    // uint8-backed enum/byte params (e.g. EUWEWindowManagerLayer LayerId on
    // WindowManager functions, ESlateVisibility on UWidget::SetVisibility)
    // come through as FByteProperty or FEnumProperty.
    static bool write_byte(RC::Unreal::FProperty* property, uint8_t* container_base, uint8_t value);

    static std::optional<bool> read_bool(RC::Unreal::FProperty* property, const uint8_t* container_base);
    // Goes through FBoolProperty's own SetPropertyValueInContainer so packed
    // bitfield bools are masked correctly rather than byte-stomped.
    static bool write_bool(RC::Unreal::FProperty* property, uint8_t* container_base, bool value);

    static RC::Unreal::UObject* read_object(RC::Unreal::FProperty* property, const uint8_t* container_base);
    static bool write_object(RC::Unreal::FProperty* property, uint8_t* container_base, RC::Unreal::UObject* value);

    // FText via FText's own real constructor / ToString, never a raw memory
    // copy. read_text returns nullopt when the property is not an FText,
    // which is how the item-display-name and locker-name lookups tell a real
    // FText field apart from a same-named field of another type.
    static bool write_text(RC::Unreal::FProperty* property, uint8_t* container_base, const std::wstring& value);
    static std::optional<std::wstring> read_text(RC::Unreal::FProperty* property, const uint8_t* container_base);

    // A struct-layout-agnostic view over a reflected TArray property's raw
    // backing storage, so callers can walk elements without knowing the
    // element type ahead of time.
    struct ArrayView {
        RC::Unreal::FProperty* inner = nullptr;
        uint8_t* data = nullptr;
        int32_t count = 0;
        int32_t element_size = 0;

        [[nodiscard]] uint8_t* element_at(int32_t index) const
        {
            return data + (static_cast<size_t>(index) * static_cast<size_t>(element_size));
        }
    };

    static std::optional<ArrayView> read_array(RC::Unreal::FProperty* property, uint8_t* container_base);

    // Reads a TArray<int32> UPROPERTY (e.g.
    // UWECraftingComponent::RegisteredSourceIds) in one go.
    static std::vector<int32_t> read_int_array(RC::Unreal::FProperty* property, uint8_t* container_base);

    // Appends a UObject* to a TArray<UObject*>-shaped property using the
    // container's own real AddZeroed growth logic (the same array-resize code
    // path the engine itself uses), rather than hand-rolled reallocation.
    //
    // This exists because the obvious-looking API for the job does NOT do the
    // job. `UWEComputerTextInterfaceComponent::AddRootDialogueOption` looks
    // like the sanctioned way to add a NoA dialogue option, but was confirmed
    // in-game to leave `ExtraRootDialogueData` -- the array the NoA UI
    // actually builds its root menu from -- untouched, while adding exactly
    // one entry to `MergedRootDialogueData` that never surfaced on screen.
    // Appending straight to `ExtraRootDialogueData` with this worked
    // immediately and the option was clickable. See
    // docs/UE4SS_API_REFERENCE.md.
    static bool append_object_to_array(RC::Unreal::FProperty* property, uint8_t* container_base, RC::Unreal::UObject* value);

    // True if a TArray<UObject*>-shaped property already contains `value`.
    // Used to keep the dialogue-option injection idempotent across rescans.
    static bool array_contains_object(RC::Unreal::FProperty* property, uint8_t* container_base, RC::Unreal::UObject* value);
};

}
